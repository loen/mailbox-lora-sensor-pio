// Receiver side: takes packets off the radio, acknowledges the sender's messages and keeps
// the "is there mail?" state.
#pragma once

#include <stdint.h>

#include "mailbox_hal.h"
#include "mail_tracker.h"

namespace mailbox {

struct ReceiverConfig {
  uint32_t ackDelayMs = 100;   // gives the sender time to switch to receive before the ACK
};

class MailboxReceiver {
 public:
  enum class Event {
    None,            // nothing arrived
    Ignored,         // corrupted or foreign packet
    NewMail,         // the flap was opened
    Duplicate,       // retransmission of a flap event that was already counted
    BatteryReport,   // heartbeat
  };

  MailboxReceiver(Radio &radio, Clock &clock, Logger &logger, Mutex &stateMutex,
                  ReceiverConfig config = ReceiverConfig());

  void begin();                // start listening

  // Call often. Handles at most one packet; blocks for ~1.5 s while an ACK is sent.
  Event poll();

  void clearMail();            // "mail collected"
  MailState state();           // snapshot, safe to call from another task

 private:
  void sendAck(uint32_t id);

  Radio &radio_;
  Clock &clock_;
  Logger &logger_;
  Mutex &stateMutex_;
  ReceiverConfig config_;
  MailTracker tracker_;        // guarded by stateMutex_
};

}  // namespace mailbox
