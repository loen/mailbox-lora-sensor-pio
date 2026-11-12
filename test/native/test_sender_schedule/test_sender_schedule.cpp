#include <unity.h>

#include "sender_schedule.h"

using namespace mailbox;

void setUp() {}
void tearDown() {}

// prints both values on failure instead of just "Expected TRUE"
#define TEST_ASSERT_ACTION(expected, actual) TEST_ASSERT_EQUAL_INT((int) (expected), (int) (actual))

// ---------- decideWakeAction ----------
void test_power_on_opens_service_window() {
  TEST_ASSERT_ACTION(WakeAction::ServiceWindowThenBattery, decideWakeAction(WakeCause::PowerOn, 0, 0));
  TEST_ASSERT_ACTION(WakeAction::ServiceWindowThenBattery, decideWakeAction(WakeCause::PowerOn, 100, 5000));
}

void test_reed_always_sends_mailbox_open() {
  TEST_ASSERT_ACTION(WakeAction::SendMailboxOpen, decideWakeAction(WakeCause::Reed, 100, 5000));
  // even when a heartbeat is overdue – the flap message carries the battery voltage anyway
  TEST_ASSERT_ACTION(WakeAction::SendMailboxOpen, decideWakeAction(WakeCause::Reed, 9000, 5000));
}

void test_timer_sends_battery_when_due() {
  TEST_ASSERT_ACTION(WakeAction::SendBattery, decideWakeAction(WakeCause::Timer, 5000, 5000));
  TEST_ASSERT_ACTION(WakeAction::SendBattery, decideWakeAction(WakeCause::Timer, 5001, 5000));
}

void test_timer_before_due_only_checks_flap() {
  TEST_ASSERT_ACTION(WakeAction::CheckFlapOnly, decideWakeAction(WakeCause::Timer, 4999, 5000));
}

// ---------- nextReportAfterSend ----------
void test_acked_message_postpones_heartbeat_by_full_interval() {
  TEST_ASSERT_EQUAL_UINT32(1000 + kBatteryReportS,
                           nextReportAfterSend(MessageType::Battery, true, 1000, 500));
  TEST_ASSERT_EQUAL_UINT32(1000 + kBatteryReportS,
                           nextReportAfterSend(MessageType::MailboxOpen, true, 1000, 500));
}

void test_unacked_battery_report_is_retried_sooner() {
  TEST_ASSERT_EQUAL_UINT32(1000 + kBatteryRetryS,
                           nextReportAfterSend(MessageType::Battery, false, 1000, 500));
}

void test_unacked_mailbox_open_leaves_heartbeat_alone() {
  TEST_ASSERT_EQUAL_UINT32(77777,
                           nextReportAfterSend(MessageType::MailboxOpen, false, 1000, 77777));
}

void test_intervals() {
  // these decide the battery life of the sender – changing them should be a conscious choice
  TEST_ASSERT_EQUAL_UINT32(24 * 3600, kBatteryReportS);
  TEST_ASSERT_EQUAL_UINT32(3600, kBatteryRetryS);
  TEST_ASSERT_EQUAL_UINT32(30, kStuckCheckS);
}

// ---------- planSleep ----------
void test_closed_flap_sleeps_until_heartbeat_and_wakes_on_reed() {
  SleepPlan p = planSleep(false, 1000, 1000 + 3600);
  TEST_ASSERT_EQUAL_UINT32(3600, p.seconds);
  TEST_ASSERT_TRUE(p.wakeOnReed);
}

void test_open_flap_is_polled_without_reed_wakeup() {
  SleepPlan p = planSleep(true, 1000, 1000 + 3600);
  TEST_ASSERT_EQUAL_UINT32(kStuckCheckS, p.seconds);
  TEST_ASSERT_FALSE(p.wakeOnReed);
}

void test_open_flap_wakes_earlier_when_heartbeat_is_closer() {
  SleepPlan p = planSleep(true, 1000, 1010);
  TEST_ASSERT_EQUAL_UINT32(10, p.seconds);
  TEST_ASSERT_FALSE(p.wakeOnReed);
}

void test_overdue_heartbeat_sleeps_one_second() {
  // never 0: a zero-length timer would not give the chip a real sleep
  TEST_ASSERT_EQUAL_UINT32(1, planSleep(false, 5000, 5000).seconds);
  TEST_ASSERT_EQUAL_UINT32(1, planSleep(false, 5000, 100).seconds);
  TEST_ASSERT_EQUAL_UINT32(1, planSleep(true, 5000, 100).seconds);
}

// ---------- the pieces together ----------
void test_failed_report_then_timer_wakeup_retries() {
  uint32_t now = 100000;
  uint32_t next = nextReportAfterSend(MessageType::Battery, false, now, now);
  SleepPlan p = planSleep(false, now, next);
  TEST_ASSERT_EQUAL_UINT32(kBatteryRetryS, p.seconds);
  now += p.seconds;
  TEST_ASSERT_ACTION(WakeAction::SendBattery, decideWakeAction(WakeCause::Timer, now, next));
}

void test_open_flap_poll_does_not_send_until_heartbeat() {
  uint32_t now = 100000;
  uint32_t next = nextReportAfterSend(MessageType::MailboxOpen, true, now, 0);
  SleepPlan p = planSleep(true, now, next);
  now += p.seconds;
  TEST_ASSERT_ACTION(WakeAction::CheckFlapOnly, decideWakeAction(WakeCause::Timer, now, next));
}

void test_lost_flap_event_with_overdue_heartbeat_does_not_spin() {
  // flap message lost while the heartbeat was already due: one short sleep, one battery
  // attempt, and if that fails too the next try is an hour away – not a 1 s wake-up loop
  uint32_t now = 100000;
  uint32_t next = nextReportAfterSend(MessageType::MailboxOpen, false, now, now - 5);
  SleepPlan p = planSleep(false, now, next);
  TEST_ASSERT_EQUAL_UINT32(1, p.seconds);
  now += p.seconds;
  TEST_ASSERT_ACTION(WakeAction::SendBattery, decideWakeAction(WakeCause::Timer, now, next));
  next = nextReportAfterSend(MessageType::Battery, false, now, next);
  TEST_ASSERT_EQUAL_UINT32(kBatteryRetryS, planSleep(false, now, next).seconds);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_power_on_opens_service_window);
  RUN_TEST(test_reed_always_sends_mailbox_open);
  RUN_TEST(test_timer_sends_battery_when_due);
  RUN_TEST(test_timer_before_due_only_checks_flap);
  RUN_TEST(test_acked_message_postpones_heartbeat_by_full_interval);
  RUN_TEST(test_unacked_battery_report_is_retried_sooner);
  RUN_TEST(test_unacked_mailbox_open_leaves_heartbeat_alone);
  RUN_TEST(test_intervals);
  RUN_TEST(test_closed_flap_sleeps_until_heartbeat_and_wakes_on_reed);
  RUN_TEST(test_open_flap_is_polled_without_reed_wakeup);
  RUN_TEST(test_open_flap_wakes_earlier_when_heartbeat_is_closer);
  RUN_TEST(test_overdue_heartbeat_sleeps_one_second);
  RUN_TEST(test_failed_report_then_timer_wakeup_retries);
  RUN_TEST(test_open_flap_poll_does_not_send_until_heartbeat);
  RUN_TEST(test_lost_flap_event_with_overdue_heartbeat_does_not_spin);
  return UNITY_END();
}
