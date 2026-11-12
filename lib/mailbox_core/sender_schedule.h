// Sender side: what to do after a wake-up and how long to sleep afterwards.
// All times are seconds on the RTC clock (keeps counting through deep sleep).
#pragma once

#include <stdint.h>

#include "protocol.h"

namespace mailbox {

constexpr uint32_t kBatteryReportS = 24UL * 3600;  // heartbeat: report the battery at least this often
constexpr uint32_t kBatteryRetryS  = 1UL * 3600;   // retry sooner when a report was not acknowledged
constexpr uint32_t kStuckCheckS    = 30;           // how often to check whether an open flap has closed

enum class WakeCause {
  PowerOn,   // power-on or reset
  Reed,      // reed switch closed = flap opened
  Timer,
};

enum class WakeAction {
  ServiceWindowThenBattery,   // wait for a firmware upload / Serial Monitor, then report the battery
  SendMailboxOpen,
  SendBattery,
  CheckFlapOnly,              // timer fired only to see whether an open flap has closed
};

WakeAction decideWakeAction(WakeCause cause, uint32_t nowS, uint32_t nextBatteryReportS);

// When the next battery heartbeat is due after a message was sent (acknowledged or not).
uint32_t nextReportAfterSend(MessageType type, bool acked, uint32_t nowS,
                             uint32_t nextBatteryReportS);

struct SleepPlan {
  uint32_t seconds;    // timer wake-up
  bool wakeOnReed;     // also wake when the flap opens
};

SleepPlan planSleep(bool flapStillOpen, uint32_t nowS, uint32_t nextBatteryReportS);

}  // namespace mailbox
