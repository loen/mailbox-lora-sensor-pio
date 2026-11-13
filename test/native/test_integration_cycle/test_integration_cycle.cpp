// Whole wake-ups of the sender (decide, send, plan the sleep) against a live receiver,
// including deep sleep and power cycles in between.
#include <unity.h>

#include "rig.h"

using namespace mailbox;

void setUp() {}
void tearDown() {}

// ---------- power-on ----------
void test_power_on_waits_for_the_service_window_then_reports_the_battery() {
  Rig rig;
  uint64_t t0 = rig.world.nowMs();
  SleepPlan plan = rig.wake(WakeCause::PowerOn);

  TEST_ASSERT_TRUE(rig.world.nowMs() - t0 >= 10000);
  TEST_ASSERT_EQUAL_STRING("BATTERY;0;4012", rig.senderSent().c_str());
  TEST_ASSERT_EQUAL_STRING("BatteryReport", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL(4012, rig.receiver.state().batteryMv);
  TEST_ASSERT_FALSE(rig.receiver.state().mail);
  TEST_ASSERT_TRUE(rig.senderLog.contains("Power-on / reset - service window"));

  // then: sleep until the flap opens, or a day at most
  TEST_ASSERT_TRUE(plan.wakeOnReed);
  TEST_ASSERT_EQUAL_UINT32(kBatteryReportS, plan.seconds);
}

void test_nothing_is_sent_during_the_service_window() {
  Rig rig;
  rig.senderRadio.dropAll = true;
  rig.wake(WakeCause::PowerOn);
  // first transmission is logged only after the 10 s window: 3 attempts, no earlier traffic
  TEST_ASSERT_EQUAL_UINT(3, rig.senderRadio.sent.size());
  TEST_ASSERT_EQUAL_STRING("Power-on / reset - service window", rig.senderLog.lines[0].c_str());
}

// ---------- flap events ----------
void test_reed_wakeup_reports_the_opening() {
  Rig rig;
  rig.deepSleep(rig.wake(WakeCause::PowerOn));
  rig.events.clear();

  SleepPlan plan = rig.wake(WakeCause::Reed);
  TEST_ASSERT_EQUAL_STRING("MAILBOX_OPEN;1;4012", rig.senderRadio.sent.back().c_str());
  TEST_ASSERT_EQUAL_STRING("NewMail", rig.eventNames().c_str());
  TEST_ASSERT_TRUE(rig.receiver.state().mail);
  TEST_ASSERT_TRUE(plan.wakeOnReed);
  TEST_ASSERT_EQUAL_UINT32(kBatteryReportS, plan.seconds);   // the flap message reset the heartbeat
}

void test_message_ids_grow_across_deep_sleeps_and_every_opening_counts() {
  Rig rig;
  rig.deepSleep(rig.wake(WakeCause::PowerOn));               // BATTERY;0
  for (int i = 0; i < 3; i++) {
    rig.wake(WakeCause::Reed);
    rig.world.jump(3600 * 1000);                             // next letter an hour later
  }
  TEST_ASSERT_EQUAL_STRING(
      "BATTERY;0;4012 | MAILBOX_OPEN;1;4012 | MAILBOX_OPEN;2;4012 | MAILBOX_OPEN;3;4012",
      rig.senderSent().c_str());
  TEST_ASSERT_EQUAL_UINT32(3, rig.receiver.state().count);
  TEST_ASSERT_EQUAL_UINT32(4, rig.senderState.msgId);
}

void test_openings_in_quick_succession_all_count() {
  // ids differ, so the duplicate window does not apply
  Rig rig;
  rig.wake(WakeCause::Reed);
  rig.wake(WakeCause::Reed);
  rig.wake(WakeCause::Reed);
  TEST_ASSERT_EQUAL_STRING("NewMail,NewMail,NewMail", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT32(3, rig.receiver.state().count);
}

void test_collecting_the_mail_and_new_mail_afterwards() {
  Rig rig;
  rig.wake(WakeCause::Reed);
  TEST_ASSERT_TRUE(rig.receiver.state().mail);

  rig.receiver.clearMail();                                  // PRG button / RESET from Home Assistant
  TEST_ASSERT_FALSE(rig.receiver.state().mail);
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);

  rig.world.jump(24 * 3600 * 1000);
  rig.wake(WakeCause::Reed);
  TEST_ASSERT_TRUE(rig.receiver.state().mail);
  TEST_ASSERT_EQUAL_UINT32(2, rig.receiver.state().count);
}

void test_lost_flap_event_is_not_recovered_later() {
  // documents the current design: after three failed attempts the event is gone
  Rig rig;
  rig.senderRadio.dropAll = true;
  rig.wake(WakeCause::Reed);
  rig.senderRadio.dropAll = false;
  rig.world.jump(3600 * 1000);
  rig.wake(WakeCause::Reed);                                 // the next opening has a new id

  TEST_ASSERT_EQUAL_STRING("MAILBOX_OPEN;1;4012", rig.senderRadio.sent.back().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
}

// ---------- flap left open ----------
void test_flap_left_open_is_polled_without_sending_anything() {
  Rig rig;
  SleepPlan plan = rig.wake(WakeCause::Reed, /*flapStillOpen=*/true);
  TEST_ASSERT_FALSE(plan.wakeOnReed);                        // the pin would wake us in a loop
  TEST_ASSERT_EQUAL_UINT32(kStuckCheckS, plan.seconds);
  size_t sentAfterOpening = rig.senderRadio.sent.size();

  for (int i = 0; i < 5; i++) {                              // still open at the next five checks
    rig.deepSleep(plan);
    plan = rig.wake(WakeCause::Timer, true);
    TEST_ASSERT_FALSE(plan.wakeOnReed);
    TEST_ASSERT_EQUAL_UINT32(kStuckCheckS, plan.seconds);
  }
  TEST_ASSERT_EQUAL_UINT(sentAfterOpening, rig.senderRadio.sent.size());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
  TEST_ASSERT_EQUAL_UINT(5, rig.senderLog.count("checking whether the flap has closed"));

  rig.deepSleep(plan);                                       // closed at last
  plan = rig.wake(WakeCause::Timer, false);
  TEST_ASSERT_TRUE(plan.wakeOnReed);
  TEST_ASSERT_EQUAL_UINT32(kBatteryReportS - 6 * kStuckCheckS, plan.seconds);
  TEST_ASSERT_EQUAL_UINT(sentAfterOpening, rig.senderRadio.sent.size());
}

void test_heartbeat_is_still_sent_while_the_flap_stays_open() {
  Rig rig;
  SleepPlan plan = rig.wake(WakeCause::Reed, true);
  size_t polls = 0;
  while (rig.senderRadio.sent.size() == 1 && polls < 5000) {
    rig.deepSleep(plan);
    plan = rig.wake(WakeCause::Timer, true);
    polls++;
  }
  TEST_ASSERT_EQUAL_UINT(kBatteryReportS / kStuckCheckS, polls);   // a day of 30 s checks
  TEST_ASSERT_EQUAL_STRING("BATTERY;1;4012", rig.senderRadio.sent.back().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);         // and it is not a new opening
}

// ---------- heartbeat ----------
void test_daily_heartbeat() {
  Rig rig;
  SleepPlan plan = rig.wake(WakeCause::PowerOn);
  for (int day = 1; day <= 3; day++) {
    rig.deepSleep(plan);
    plan = rig.wake(WakeCause::Timer);
    TEST_ASSERT_EQUAL_UINT32(kBatteryReportS, plan.seconds);
  }
  TEST_ASSERT_EQUAL_STRING("BATTERY;0;4012 | BATTERY;1;4012 | BATTERY;2;4012 | BATTERY;3;4012",
                           rig.senderSent().c_str());
  TEST_ASSERT_EQUAL_STRING("BatteryReport,BatteryReport,BatteryReport,BatteryReport",
                           rig.eventNames().c_str());
}

void test_failed_heartbeat_is_retried_after_an_hour() {
  Rig rig;
  rig.senderRadio.dropAll = true;                            // receiver unplugged
  SleepPlan plan = rig.wake(WakeCause::PowerOn);
  TEST_ASSERT_EQUAL_UINT32(kBatteryRetryS, plan.seconds);
  TEST_ASSERT_TRUE(plan.wakeOnReed);

  rig.deepSleep(plan);
  plan = rig.wake(WakeCause::Timer);                         // still unplugged
  TEST_ASSERT_EQUAL_UINT32(kBatteryRetryS, plan.seconds);
  TEST_ASSERT_EQUAL_UINT(6, rig.senderRadio.sent.size());    // 3 attempts per wake-up, no more

  rig.senderRadio.dropAll = false;                           // back online
  rig.deepSleep(plan);
  plan = rig.wake(WakeCause::Timer);
  TEST_ASSERT_EQUAL_STRING("BATTERY;2;4012", rig.senderRadio.sent.back().c_str());
  TEST_ASSERT_EQUAL_STRING("BatteryReport", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT32(kBatteryReportS, plan.seconds);
}

void test_battery_voltage_follows_every_message() {
  Rig rig;
  rig.batteryMv = 4100;
  rig.wake(WakeCause::PowerOn);
  TEST_ASSERT_EQUAL(4100, rig.receiver.state().batteryMv);
  rig.batteryMv = 3650;
  rig.wake(WakeCause::Reed);
  TEST_ASSERT_EQUAL(3650, rig.receiver.state().batteryMv);
}

// ---------- sender restart: message ids start over ----------
void test_same_ids_after_a_battery_swap_are_new_mail() {
  Rig rig;
  rig.wake(WakeCause::PowerOn);                              // BATTERY;0
  rig.wake(WakeCause::Reed);                                 // MAILBOX_OPEN;1
  rig.receiver.clearMail();

  rig.world.jump(3600 * 1000);
  rig.powerCycleSender();
  rig.wake(WakeCause::PowerOn);                              // BATTERY;0 again
  rig.wake(WakeCause::Reed);                                 // MAILBOX_OPEN;1 again

  TEST_ASSERT_EQUAL_STRING("MAILBOX_OPEN;1;4012", rig.senderRadio.sent.back().c_str());
  TEST_ASSERT_TRUE(rig.receiver.state().mail);
  TEST_ASSERT_EQUAL_UINT32(2, rig.receiver.state().count);
}

void test_known_limitation_restart_and_same_id_within_a_minute_is_dropped() {
  // Reset right after an opening, then the flap opens again before the 60 s duplicate window
  // is over: same id, so the receiver takes it for a retransmission. It is still ACKed.
  Rig rig;
  rig.wake(WakeCause::PowerOn);
  rig.wake(WakeCause::Reed);                                 // MAILBOX_OPEN;1
  rig.powerCycleSender();
  rig.wake(WakeCause::PowerOn);                              // ~13 s: service window + BATTERY;0
  rig.events.clear();
  rig.wake(WakeCause::Reed);                                 // MAILBOX_OPEN;1 again

  TEST_ASSERT_EQUAL_STRING("Duplicate", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
  TEST_ASSERT_EQUAL_STRING("ACK;1", rig.receiverRadio.sent.back().c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_power_on_waits_for_the_service_window_then_reports_the_battery);
  RUN_TEST(test_nothing_is_sent_during_the_service_window);
  RUN_TEST(test_reed_wakeup_reports_the_opening);
  RUN_TEST(test_message_ids_grow_across_deep_sleeps_and_every_opening_counts);
  RUN_TEST(test_openings_in_quick_succession_all_count);
  RUN_TEST(test_collecting_the_mail_and_new_mail_afterwards);
  RUN_TEST(test_lost_flap_event_is_not_recovered_later);
  RUN_TEST(test_flap_left_open_is_polled_without_sending_anything);
  RUN_TEST(test_heartbeat_is_still_sent_while_the_flap_stays_open);
  RUN_TEST(test_daily_heartbeat);
  RUN_TEST(test_failed_heartbeat_is_retried_after_an_hour);
  RUN_TEST(test_battery_voltage_follows_every_message);
  RUN_TEST(test_same_ids_after_a_battery_swap_are_new_mail);
  RUN_TEST(test_known_limitation_restart_and_same_id_within_a_minute_is_dropped);
  return UNITY_END();
}
