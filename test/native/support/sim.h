// In-memory replacement for the hardware, used by the integration tests: two (or more)
// half-duplex radios sharing one "air", and a virtual clock.
//
// The sender and the receiver are both written as blocking code (delay, blocking transmit),
// so each side runs in its own thread – but only one thread runs at a time. A thread that
// waits for virtual time to pass hands control to whichever thread wakes up next and the
// clock jumps straight to that moment. The result is deterministic and a 4 s ACK timeout
// takes microseconds.
//
// What the model covers: time on air, half-duplex (a radio only receives a packet it listened
// to from start to end, and has to be re-armed after each packet), lost / corrupted packets,
// failed transmissions and packets from foreign stations.
// What it does not cover: range, signal strength, and collisions of overlapping packets.
#pragma once

#include <stdint.h>

#include <cmath>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <vector>

#include "mailbox_hal.h"

namespace sim {

// LoRa time on air for the project's settings: SF12, 125 kHz, CR 4/8, preamble 8,
// explicit header, CRC on, low data rate optimisation on.
inline uint32_t airtimeMs(size_t payloadBytes) {
  const double symbolMs = 4096.0 / 125.0;                       // 2^SF / BW
  const double bits = 8.0 * (double) payloadBytes - 4.0 * 12 + 28 + 16;
  const double payloadSymbols = 8 + std::fmax(std::ceil(bits / (4.0 * (12 - 2))) * 8, 0.0);
  return (uint32_t) std::lround((8 + 4.25 + payloadSymbols) * symbolMs);
}

class SimRadio;

class World {
 public:
  // The thread that creates the world (the test itself) is its first actor.
  explicit World(uint64_t startMs = 0) : nowMs_(startMs) {
    main_.wakeAtMs = startMs;
    running_ = &main_;
    self() = &main_;
  }

  ~World() { stop(); }

  uint64_t nowMs() const { return nowMs_; }

  // Starts another actor (the receiver's loop) in its own thread. It gets to run whenever
  // it is the next one to wake up.
  void spawn(std::function<void()> body) {
    std::unique_lock<std::mutex> lock(mutex_);
    actors_.push_back(std::unique_ptr<Actor>(new Actor()));
    Actor *actor = actors_.back().get();
    actor->wakeAtMs = nowMs_;
    actor->order = ++orderCounter_;
    threads_.emplace_back([this, actor, body] {
      self() = actor;
      {
        std::unique_lock<std::mutex> l(mutex_);
        turn_.wait(l, [&] { return running_ == actor || stopping_; });
      }
      body();
      std::unique_lock<std::mutex> l(mutex_);
      actor->finished = true;
      if (!stopping_) handOver();
    });
  }

  // Called by the running actor: lets `ms` of virtual time pass. Other actors run meanwhile.
  void advance(uint32_t ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_) return;
    Actor *me = self();
    me->wakeAtMs = nowMs_ + ms;
    me->order = ++orderCounter_;
    handOver();
    turn_.wait(lock, [&] { return running_ == me || stopping_; });
  }

  // Skips time during which nothing happens at all (the sender is in deep sleep and the
  // receiver hears nothing). Every other actor simply continues where it was.
  void jump(uint64_t ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    nowMs_ += ms;
    for (auto &a : actors_) a->wakeAtMs += ms;
  }

  // A packet from a station outside the test, on the air from `startMs` for its time on air.
  void injectAt(uint64_t startMs, const std::string &text, bool corrupt = false) {
    std::unique_lock<std::mutex> lock(mutex_);
    flights_.push_back({startMs, startMs + airtimeMs(text.size()), text, corrupt, false});
  }

  // Ends the simulation: every actor's waits return at once so that its thread can finish.
  void stop() {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    turn_.notify_all();
    for (std::thread &t : threads_) {
      if (t.joinable()) t.join();
    }
    threads_.clear();
  }

  bool stopping() const { return stopping_; }

  void addRadio(SimRadio *radio) { radios_.push_back(radio); }
  inline void deliver(SimRadio *from, uint64_t startMs, const std::string &text, bool corrupt);

 private:
  struct Actor {
    uint64_t wakeAtMs = 0;
    uint64_t order = 0;       // who went to sleep first wakes first when times are equal
    bool finished = false;
  };

  struct Flight {
    uint64_t startMs;
    uint64_t endMs;
    std::string text;
    bool corrupt;
    bool done;
  };

  static Actor *&self() {
    static thread_local Actor *actor = nullptr;
    return actor;
  }

  // Picks the actor that wakes up next, moves the clock there and lets it run.
  // Caller holds the mutex.
  void handOver() {
    Actor *next = main_.finished ? nullptr : &main_;
    for (auto &a : actors_) {
      if (a->finished) continue;
      if (!next || a->wakeAtMs < next->wakeAtMs ||
          (a->wakeAtMs == next->wakeAtMs && a->order < next->order)) {
        next = a.get();
      }
    }
    if (!next) return;
    if (next->wakeAtMs > nowMs_) nowMs_ = next->wakeAtMs;
    // foreign packets that ended while everybody was asleep; nobody changed a radio since
    for (Flight &f : flights_) {
      if (!f.done && f.endMs <= nowMs_) {
        f.done = true;
        deliver(nullptr, f.startMs, f.text, f.corrupt);
      }
    }
    running_ = next;
    turn_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable turn_;
  uint64_t nowMs_;
  uint64_t orderCounter_ = 0;
  Actor main_;
  Actor *running_ = nullptr;
  bool stopping_ = false;
  std::vector<std::unique_ptr<Actor>> actors_;
  std::vector<std::thread> threads_;
  std::vector<SimRadio *> radios_;
  std::vector<Flight> flights_;
};

class SimRadio : public mailbox::Radio {
 public:
  explicit SimRadio(World &world) : world_(world) { world_.addRadio(this); }

  // ---------- fault injection: 1-based numbers of this radio's transmissions ----------
  std::set<size_t> dropTx;      // sent, but nobody hears it
  std::set<size_t> corruptTx;   // arrives with a CRC error
  std::set<size_t> failTx;      // the radio reports an error and sends nothing
  bool dropAll = false;

  // ---------- what the other side measures when it hears this radio ----------
  int rssi = -97;
  float snr = 6.5f;

  // ---------- observations ----------
  std::vector<std::string> sent;   // every transmit() call, including failed and lost ones
  std::vector<uint64_t> sentAtMs;  // when each of them started
  size_t heard = 0;                // packets that reached this radio (valid or corrupted)
  size_t missed = 0;               // packets on the air while this radio was not listening

  bool transmit(const std::string &text) override {
    sent.push_back(text);
    sentAtMs.push_back(world_.nowMs());
    const size_t n = sent.size();
    listening_ = false;
    if (failTx.count(n)) return false;

    const uint64_t start = world_.nowMs();
    world_.advance(airtimeMs(text.size()));
    if (!dropAll && !dropTx.count(n)) world_.deliver(this, start, text, corruptTx.count(n) > 0);
    return true;
  }

  void startReceive() override {
    listening_ = true;
    listeningSince_ = world_.nowMs();
    hasPending_ = false;
  }

  mailbox::RxStatus poll(mailbox::RxPacket &out) override {
    if (!hasPending_) return mailbox::RxStatus::None;
    hasPending_ = false;
    out = pending_;
    return pendingStatus_;
  }

  // Called by the world when a packet that was on the air since `startMs` ends.
  void onAir(uint64_t startMs, const std::string &text, bool corrupt, int rssi_, float snr_) {
    if (!listening_ || listeningSince_ > startMs) {
      missed++;
      return;
    }
    heard++;
    listening_ = false;          // one packet per startReceive(), as the Radio contract says
    hasPending_ = true;
    pendingStatus_ = corrupt ? mailbox::RxStatus::CrcError : mailbox::RxStatus::Packet;
    pending_.text = corrupt ? std::string() : text;
    pending_.rssi = rssi_;
    pending_.snr = snr_;
  }

 private:
  World &world_;
  bool listening_ = false;
  uint64_t listeningSince_ = 0;
  bool hasPending_ = false;
  mailbox::RxStatus pendingStatus_ = mailbox::RxStatus::None;
  mailbox::RxPacket pending_;
};

inline void World::deliver(SimRadio *from, uint64_t startMs, const std::string &text,
                           bool corrupt) {
  for (SimRadio *r : radios_) {
    if (r == from) continue;
    r->onAir(startMs, text, corrupt, from ? from->rssi : -110, from ? from->snr : -5.0f);
  }
}

class SimClock : public mailbox::Clock {
 public:
  explicit SimClock(World &world) : world_(world), rtcEpochMs_(world.nowMs()) {}

  uint32_t nowMs() override { return (uint32_t) world_.nowMs(); }   // wraps like millis()
  void sleepMs(uint32_t ms) override { world_.advance(ms); }
  uint32_t rtcSeconds() override { return (uint32_t) ((world_.nowMs() - rtcEpochMs_) / 1000); }

  void powerOn() { rtcEpochMs_ = world_.nowMs(); }                  // the RTC restarts at 0

 private:
  World &world_;
  uint64_t rtcEpochMs_;
};

// Always returns the same back-off, so the timing of a test is repeatable.
class FakeRandom : public mailbox::RandomSource {
 public:
  uint32_t value = 1000;
  std::vector<std::pair<uint32_t, uint32_t>> calls;

  uint32_t between(uint32_t lo, uint32_t hi) override {
    calls.push_back({lo, hi});
    if (value < lo) return lo;
    if (value >= hi) return hi - 1;
    return value;
  }
};

class FakeLogger : public mailbox::Logger {
 public:
  std::vector<std::string> lines;

  void log(const std::string &line) override { lines.push_back(line); }

  size_t count(const std::string &part) const {
    size_t n = 0;
    for (const std::string &l : lines) {
      if (l.find(part) != std::string::npos) n++;
    }
    return n;
  }
  bool contains(const std::string &part) const { return count(part) > 0; }
};

class CountingMutex : public mailbox::Mutex {
 public:
  int depth = 0;       // > 1 would be a nested lock, != 0 at the end a missing unlock
  int maxDepth = 0;
  int locks = 0;

  void lock() override {
    depth++;
    locks++;
    if (depth > maxDepth) maxDepth = depth;
  }
  void unlock() override { depth--; }
};

}  // namespace sim
