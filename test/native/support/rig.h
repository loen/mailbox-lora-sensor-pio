// A sender and a receiver wired together through the simulated air – the fixture of the
// integration tests.
#pragma once

#include <string>
#include <vector>

#include "mailbox_receiver.h"
#include "mailbox_sender.h"
#include "sim.h"

struct Rig {
  sim::World world;
  sim::SimRadio senderRadio{world};
  sim::SimRadio receiverRadio{world};
  sim::SimClock senderClock{world};
  sim::SimClock receiverClock{world};
  sim::FakeRandom random;
  sim::FakeLogger senderLog;
  sim::FakeLogger receiverLog;
  sim::CountingMutex mutex;

  mailbox::SenderState senderState = {0, 0};   // the sender's RTC memory
  uint16_t batteryMv = 4012;

  mailbox::MailboxReceiver receiver;
  std::vector<mailbox::MailboxReceiver::Event> events;   // everything poll() reported

  explicit Rig(mailbox::ReceiverConfig receiverConfig = mailbox::ReceiverConfig(),
               uint64_t startMs = 0)
      : world(startMs), receiver(receiverRadio, receiverClock, receiverLog, mutex, receiverConfig) {
    receiver.begin();
    // the receiver firmware's loop(): poll the radio, then delay(5)
    world.spawn([this] {
      while (!world.stopping()) {
        mailbox::MailboxReceiver::Event e = receiver.poll();
        if (e != mailbox::MailboxReceiver::Event::None) events.push_back(e);
        world.advance(5);
      }
    });
  }

  ~Rig() { world.stop(); }   // before the receiver and its radio are destroyed

  // The firmware builds a fresh MailboxSender after every wake-up; only senderState survives.
  mailbox::MailboxSender makeSender() {
    return mailbox::MailboxSender(senderRadio, senderClock, random, senderLog, senderState);
  }

  // Link level: one message with retries. Returns whether it was acknowledged.
  bool send(mailbox::MessageType type) { return makeSender().send(type, batteryMv); }

  // One complete wake-up, the way src/sender/main.cpp runs it.
  mailbox::SleepPlan wake(mailbox::WakeCause cause, bool flapStillOpen = false) {
    mailbox::MailboxSender sender = makeSender();
    sender.onWake(cause, batteryMv);
    return sender.planSleep(flapStillOpen);
  }

  void deepSleep(const mailbox::SleepPlan &plan) { world.jump((uint64_t) plan.seconds * 1000); }

  // Batteries swapped: RTC memory and RTC clock start from zero.
  void powerCycleSender() {
    senderState = {0, 0};
    senderClock.powerOn();
  }

  // e.g. "NewMail,Duplicate" – readable in a failing assertion
  std::string eventNames() const {
    std::string s;
    for (mailbox::MailboxReceiver::Event e : events) {
      if (!s.empty()) s += ",";
      switch (e) {
        case mailbox::MailboxReceiver::Event::None:          s += "None"; break;
        case mailbox::MailboxReceiver::Event::Ignored:       s += "Ignored"; break;
        case mailbox::MailboxReceiver::Event::NewMail:       s += "NewMail"; break;
        case mailbox::MailboxReceiver::Event::Duplicate:     s += "Duplicate"; break;
        case mailbox::MailboxReceiver::Event::BatteryReport: s += "BatteryReport"; break;
      }
    }
    return s;
  }

  static std::string join(const std::vector<std::string> &v) {
    std::string s;
    for (const std::string &x : v) {
      if (!s.empty()) s += " | ";
      s += x;
    }
    return s;
  }
  std::string senderSent() const { return join(senderRadio.sent); }
  std::string receiverSent() const { return join(receiverRadio.sent); }
};
