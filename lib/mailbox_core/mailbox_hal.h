// Everything the sender / receiver logic needs from the hardware. The firmware implements
// these with RadioLib and Arduino (lib/mailbox_arduino); the tests use in-memory fakes.
#pragma once

#include <stdint.h>
#include <string>

namespace mailbox {

struct RxPacket {
  std::string text;
  int rssi = 0;        // dBm
  float snr = 0;       // dB
};

enum class RxStatus {
  None,       // nothing received since the last call
  Packet,     // `out` holds a packet with a valid CRC
  CrcError,   // a packet arrived but was corrupted
  Error,      // the radio failed to hand over the packet
};

// Half-duplex packet radio: it either transmits or listens.
class Radio {
 public:
  virtual ~Radio() = default;

  // Sends one packet and blocks until it is on the air. The radio stops listening;
  // call startReceive() afterwards. Returns false when the radio reported an error.
  virtual bool transmit(const std::string &text) = 0;

  // Switches to continuous receive and forgets any packet that was not polled yet.
  virtual void startReceive() = 0;

  // Non-blocking. After anything other than None the radio has to be re-armed
  // with startReceive() before the next packet can arrive.
  virtual RxStatus poll(RxPacket &out) = 0;
};

class Clock {
 public:
  virtual ~Clock() = default;
  virtual uint32_t nowMs() = 0;            // since boot, wraps around after ~49.7 days
  virtual void sleepMs(uint32_t ms) = 0;   // blocking delay
  virtual uint32_t rtcSeconds() = 0;       // keeps counting through deep sleep, 0 at power-on
};

class RandomSource {
 public:
  virtual ~RandomSource() = default;
  virtual uint32_t between(uint32_t lo, uint32_t hi) = 0;   // lo <= result < hi
};

class Logger {
 public:
  virtual ~Logger() = default;
  virtual void log(const std::string &line) = 0;
};

// Guards state that is read from another task (the receiver's network task).
class Mutex {
 public:
  virtual ~Mutex() = default;
  virtual void lock() = 0;
  virtual void unlock() = 0;
};

}  // namespace mailbox
