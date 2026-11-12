#include <unity.h>

#include "mail_tracker.h"

using namespace mailbox;

void setUp() {}
void tearDown() {}

static Message open(uint32_t id, uint16_t mv = 4000) {
  Message m;
  m.type = MessageType::MailboxOpen;
  m.id = id;
  m.batteryMv = mv;
  return m;
}

static Message battery(uint32_t id, uint16_t mv = 4000) {
  Message m;
  m.type = MessageType::Battery;
  m.id = id;
  m.batteryMv = mv;
  return m;
}

void test_initial_state() {
  MailTracker t;
  TEST_ASSERT_FALSE(t.state().mail);
  TEST_ASSERT_EQUAL_UINT32(0, t.state().count);
  TEST_ASSERT_FALSE(t.state().hasPacket);
  TEST_ASSERT_EQUAL(0, t.state().batteryMv);
}

void test_first_open_is_new_mail() {
  MailTracker t;
  TEST_ASSERT_TRUE(t.onMessage(open(1, 3987), -97, 6.5f, 1000));
  TEST_ASSERT_TRUE(t.state().mail);
  TEST_ASSERT_EQUAL_UINT32(1, t.state().count);
  TEST_ASSERT_TRUE(t.state().hasPacket);
  TEST_ASSERT_EQUAL(-97, t.state().rssi);
  TEST_ASSERT_EQUAL_FLOAT(6.5f, t.state().snr);
  TEST_ASSERT_EQUAL(3987, t.state().batteryMv);
}

void test_first_open_with_id_zero_at_time_zero_is_new() {
  MailTracker t;
  TEST_ASSERT_TRUE(t.onMessage(open(0), -90, 5, 0));
  TEST_ASSERT_EQUAL_UINT32(1, t.state().count);
}

// ---------- retransmissions ----------
void test_retransmission_is_not_counted_twice() {
  MailTracker t;
  TEST_ASSERT_TRUE(t.onMessage(open(7), -90, 5, 1000));
  TEST_ASSERT_FALSE(t.onMessage(open(7), -92, 4, 8000));
  TEST_ASSERT_FALSE(t.onMessage(open(7), -95, 3, 15000));
  TEST_ASSERT_EQUAL_UINT32(1, t.state().count);
  TEST_ASSERT_TRUE(t.state().mail);
  TEST_ASSERT_EQUAL(-95, t.state().rssi);          // signal still follows the last packet
}

void test_retransmission_after_clear_does_not_raise_mail_again() {
  MailTracker t;
  t.onMessage(open(7), -90, 5, 1000);
  t.clearMail();
  TEST_ASSERT_FALSE(t.onMessage(open(7), -90, 5, 9000));
  TEST_ASSERT_FALSE(t.state().mail);
  TEST_ASSERT_EQUAL_UINT32(1, t.state().count);
}

void test_duplicate_window_boundary() {
  MailTracker t;
  t.onMessage(open(7), -90, 5, 1000);
  TEST_ASSERT_FALSE(t.onMessage(open(7), -90, 5, 1000 + kDuplicateWindowMs - 1));
  TEST_ASSERT_TRUE(t.onMessage(open(7), -90, 5, 1000 + kDuplicateWindowMs));
  TEST_ASSERT_EQUAL_UINT32(2, t.state().count);
}

void test_window_is_measured_from_the_first_copy() {
  // retransmissions must not keep the window open forever
  MailTracker t;
  t.onMessage(open(7), -90, 5, 0);
  TEST_ASSERT_FALSE(t.onMessage(open(7), -90, 5, kDuplicateWindowMs - 1));
  TEST_ASSERT_TRUE(t.onMessage(open(7), -90, 5, kDuplicateWindowMs + 10));
}

void test_window_covers_the_senders_retransmissions() {
  // 3 attempts: ~1.5 s on air + 4 s ACK wait + up to 2 s back-off each
  TEST_ASSERT_TRUE(kDuplicateWindowMs >= 3 * (1500 + 4000 + 2000));
}

void test_duplicate_detection_survives_clock_wraparound() {
  MailTracker t;
  uint32_t justBeforeWrap = UINT32_MAX - 2000;
  TEST_ASSERT_TRUE(t.onMessage(open(7), -90, 5, justBeforeWrap));
  TEST_ASSERT_FALSE(t.onMessage(open(7), -90, 5, justBeforeWrap + 1000));   // not wrapped yet
  TEST_ASSERT_FALSE(t.onMessage(open(7), -90, 5, 5000));          // 7 s later, clock wrapped
  TEST_ASSERT_TRUE(t.onMessage(open(7), -90, 5, kDuplicateWindowMs));
}

// ---------- sender restart: ids start over ----------
void test_same_id_after_sender_restart_is_new_mail() {
  MailTracker t;
  // first boot: BATTERY 0, then the flap -> MAILBOX_OPEN 1
  t.onMessage(battery(0), -90, 5, 10000);
  TEST_ASSERT_TRUE(t.onMessage(open(1), -90, 5, 20000));
  t.clearMail();
  // battery swapped an hour later: the very same ids again
  t.onMessage(battery(0), -90, 5, 3600000);
  TEST_ASSERT_TRUE(t.onMessage(open(1), -90, 5, 3620000));
  TEST_ASSERT_TRUE(t.state().mail);
  TEST_ASSERT_EQUAL_UINT32(2, t.state().count);
}

void test_lower_id_counts_after_sender_restart() {
  MailTracker t;
  t.onMessage(open(50), -90, 5, 1000);
  TEST_ASSERT_TRUE(t.onMessage(open(1), -90, 5, 2000));
  TEST_ASSERT_EQUAL_UINT32(2, t.state().count);
}

void test_each_new_id_counts() {
  MailTracker t;
  TEST_ASSERT_TRUE(t.onMessage(open(1), -90, 5, 1000));
  TEST_ASSERT_TRUE(t.onMessage(open(2), -90, 5, 1001));
  TEST_ASSERT_TRUE(t.onMessage(open(3), -90, 5, 1002));
  TEST_ASSERT_EQUAL_UINT32(3, t.state().count);
}

// ---------- battery reports ----------
void test_battery_report_is_not_mail() {
  MailTracker t;
  TEST_ASSERT_FALSE(t.onMessage(battery(1, 3811), -101, -2.5f, 1000));
  TEST_ASSERT_FALSE(t.state().mail);
  TEST_ASSERT_EQUAL_UINT32(0, t.state().count);
  TEST_ASSERT_TRUE(t.state().hasPacket);
  TEST_ASSERT_EQUAL(-101, t.state().rssi);
  TEST_ASSERT_EQUAL(3811, t.state().batteryMv);
}

void test_battery_report_does_not_hide_a_duplicate() {
  MailTracker t;
  t.onMessage(open(4), -90, 5, 1000);
  t.onMessage(battery(5), -90, 5, 2000);
  TEST_ASSERT_FALSE(t.onMessage(open(4), -90, 5, 3000));
  TEST_ASSERT_EQUAL_UINT32(1, t.state().count);
}

void test_open_with_same_id_as_battery_report_is_new() {
  MailTracker t;
  t.onMessage(battery(4), -90, 5, 1000);
  TEST_ASSERT_TRUE(t.onMessage(open(4), -90, 5, 2000));
}

void test_zero_battery_keeps_last_reading() {
  MailTracker t;
  t.onMessage(battery(1, 3900), -90, 5, 1000);
  t.onMessage(open(2, 0), -90, 5, 2000);           // message without a battery field
  TEST_ASSERT_EQUAL(3900, t.state().batteryMv);
}

// ---------- clearMail ----------
void test_clear_mail_keeps_everything_else() {
  MailTracker t;
  t.onMessage(open(1, 3950), -88, 7, 1000);
  t.clearMail();
  TEST_ASSERT_FALSE(t.state().mail);
  TEST_ASSERT_EQUAL_UINT32(1, t.state().count);
  TEST_ASSERT_TRUE(t.state().hasPacket);
  TEST_ASSERT_EQUAL(-88, t.state().rssi);
  TEST_ASSERT_EQUAL(3950, t.state().batteryMv);
}

void test_new_open_after_clear_raises_mail_again() {
  MailTracker t;
  t.onMessage(open(1), -90, 5, 1000);
  t.clearMail();
  TEST_ASSERT_TRUE(t.onMessage(open(2), -90, 5, 2000));
  TEST_ASSERT_TRUE(t.state().mail);
  TEST_ASSERT_EQUAL_UINT32(2, t.state().count);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_initial_state);
  RUN_TEST(test_first_open_is_new_mail);
  RUN_TEST(test_first_open_with_id_zero_at_time_zero_is_new);
  RUN_TEST(test_retransmission_is_not_counted_twice);
  RUN_TEST(test_retransmission_after_clear_does_not_raise_mail_again);
  RUN_TEST(test_duplicate_window_boundary);
  RUN_TEST(test_window_is_measured_from_the_first_copy);
  RUN_TEST(test_window_covers_the_senders_retransmissions);
  RUN_TEST(test_duplicate_detection_survives_clock_wraparound);
  RUN_TEST(test_same_id_after_sender_restart_is_new_mail);
  RUN_TEST(test_lower_id_counts_after_sender_restart);
  RUN_TEST(test_each_new_id_counts);
  RUN_TEST(test_battery_report_is_not_mail);
  RUN_TEST(test_battery_report_does_not_hide_a_duplicate);
  RUN_TEST(test_open_with_same_id_as_battery_report_is_new);
  RUN_TEST(test_zero_battery_keeps_last_reading);
  RUN_TEST(test_clear_mail_keeps_everything_else);
  RUN_TEST(test_new_open_after_clear_raises_mail_again);
  return UNITY_END();
}
