// Sender side: one wake-up of the board in the mailbox – decide what to report, send it
// with retries until the receiver acknowledges, and plan the next sleep.
#pragma once

#include <stdint.h>

#include "mailbox_hal.h"
#include "protocol.h"
#include "sender_schedule.h"

namespace mailbox {

// Has to survive deep sleep (the firmware keeps it in RTC memory); all zeros at power-on.
struct SenderState {
  uint32_t msgId;                // number of the next message
  uint32_t nextBatteryReportS;   // RTC time of the next battery heartbeat
};

struct SenderConfig {
  int maxAttempts = 3;               // how many times to send if no ACK
  uint32_t ackTimeoutMs = 4000;      // ACK wait time (an ACK at SF12 takes ~1.2 s)
  uint32_t ackPollMs = 2;            // how often to look for the ACK while waiting
  uint32_t backoffMinMs = 500;       // random pause before a retry: min <= pause < max
  uint32_t backoffMaxMs = 2000;
  uint32_t serviceWindowMs = 10000;  // after reset: time to upload firmware / open Serial Monitor
};

class MailboxSender {
 public:
  MailboxSender(Radio &radio, Clock &clock, RandomSource &random, Logger &logger,
                SenderState &state, SenderConfig config = SenderConfig());

  // Runs the action for this wake-up (flap event, battery heartbeat, or nothing).
  void onWake(WakeCause cause, uint16_t batteryMv);

  // Sends "<type>;<id>;<battery mV>" and waits for "ACK;<id>", retrying when it does not
  // come. Every message carries the battery voltage, so a flap event doubles as a battery
  // report. Returns true when the receiver acknowledged.
  bool send(MessageType type, uint16_t batteryMv);

  SleepPlan planSleep(bool flapStillOpen);

 private:
  bool waitForAck(uint32_t id);

  Radio &radio_;
  Clock &clock_;
  RandomSource &random_;
  Logger &logger_;
  SenderState &state_;
  SenderConfig config_;
};

}  // namespace mailbox
