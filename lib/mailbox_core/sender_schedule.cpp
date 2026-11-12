#include "sender_schedule.h"

namespace mailbox {

WakeAction decideWakeAction(WakeCause cause, uint32_t nowS, uint32_t nextBatteryReportS) {
  switch (cause) {
    case WakeCause::Reed:
      // the flap was opened, even if it has closed again already
      return WakeAction::SendMailboxOpen;
    case WakeCause::Timer:
      // either the heartbeat is due or we are polling a flap that was left open
      return (nowS >= nextBatteryReportS) ? WakeAction::SendBattery : WakeAction::CheckFlapOnly;
    case WakeCause::PowerOn:
    default:
      return WakeAction::ServiceWindowThenBattery;
  }
}

uint32_t nextReportAfterSend(MessageType type, bool acked, uint32_t nowS,
                             uint32_t nextBatteryReportS) {
  // every message carries the battery voltage, so any acknowledged one counts as a report
  if (acked) return nowS + kBatteryReportS;
  if (type == MessageType::Battery) return nowS + kBatteryRetryS;
  return nextBatteryReportS;                    // lost flap event – heartbeat stays as planned
}

SleepPlan planSleep(bool flapStillOpen, uint32_t nowS, uint32_t nextBatteryReportS) {
  uint32_t untilReport = (nextBatteryReportS > nowS) ? nextBatteryReportS - nowS : 1;
  if (flapStillOpen) {
    // don't wake on the pin (it would wake repeatedly) – poll with a timer instead
    return {untilReport < kStuckCheckS ? untilReport : kStuckCheckS, false};
  }
  return {untilReport, true};
}

}  // namespace mailbox
