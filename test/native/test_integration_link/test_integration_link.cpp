// Sender and receiver logic talking to each other through the simulated radio:
// one message, its retries and its ACK.
#include <unity.h>

#include "rig.h"

using namespace mailbox;

void setUp() {}
void tearDown() {}

static const char kOpen0[] = "MAILBOX_OPEN;0;4012";

// ---------- the normal case ----------
void test_message_is_acknowledged_on_the_first_attempt() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));

  TEST_ASSERT_EQUAL_STRING(kOpen0, rig.senderSent().c_str());
  TEST_ASSERT_EQUAL_STRING("ACK;0", rig.receiverSent().c_str());
  TEST_ASSERT_EQUAL_STRING("NewMail", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT(0, rig.random.calls.size());          // no back-off needed

  MailState s = rig.receiver.state();
  TEST_ASSERT_TRUE(s.mail);
  TEST_ASSERT_EQUAL_UINT32(1, s.count);
  TEST_ASSERT_EQUAL(4012, s.batteryMv);
  TEST_ASSERT_EQUAL(-97, s.rssi);
  TEST_ASSERT_EQUAL_FLOAT(6.5f, s.snr);

  TEST_ASSERT_EQUAL_UINT32(1, rig.senderState.msgId);
  TEST_ASSERT_TRUE(rig.senderLog.contains("Attempt 1/3: sent MAILBOX_OPEN;0;4012"));
  TEST_ASSERT_TRUE(rig.senderLog.contains("ACK received"));
  TEST_ASSERT_TRUE(rig.receiverLog.contains("Sent ACK;0"));
}

void test_battery_report_is_acknowledged_but_is_not_mail() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.send(MessageType::Battery));

  TEST_ASSERT_EQUAL_STRING("BATTERY;0;4012", rig.senderSent().c_str());
  TEST_ASSERT_EQUAL_STRING("BatteryReport", rig.eventNames().c_str());
  TEST_ASSERT_FALSE(rig.receiver.state().mail);
  TEST_ASSERT_EQUAL_UINT32(0, rig.receiver.state().count);
  TEST_ASSERT_EQUAL(4012, rig.receiver.state().batteryMv);
  TEST_ASSERT_TRUE(rig.receiverLog.contains("Battery report: 4012 mV (81 %)"));
}

void test_ack_is_delayed_so_the_sender_can_switch_to_receive() {
  Rig rig;
  rig.send(MessageType::MailboxOpen);
  uint64_t messageEnd = rig.senderRadio.sentAtMs[0] + sim::airtimeMs(sizeof(kOpen0) - 1);
  TEST_ASSERT_TRUE(rig.receiverRadio.sentAtMs[0] >= messageEnd + 100);
  TEST_ASSERT_TRUE(rig.receiverRadio.sentAtMs[0] <= messageEnd + 110);
}

void test_ack_arrives_well_inside_the_timeout() {
  // 100 ms pause + ~0.9 s ACK on the air: the 4 s timeout leaves a comfortable margin
  Rig rig;
  rig.send(MessageType::MailboxOpen);
  uint64_t messageEnd = rig.senderRadio.sentAtMs[0] + sim::airtimeMs(sizeof(kOpen0) - 1);
  uint64_t waited = rig.world.nowMs() - messageEnd;
  TEST_ASSERT_TRUE(waited >= 100 + sim::airtimeMs(5));
  TEST_ASSERT_TRUE(waited < 2000);
}

void test_longest_possible_ack_still_fits_in_the_timeout() {
  Rig rig;
  rig.senderState.msgId = UINT32_MAX;           // "ACK;4294967295"
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));
  TEST_ASSERT_EQUAL_UINT(1, rig.senderRadio.sent.size());
  TEST_ASSERT_EQUAL_UINT32(0, rig.senderState.msgId);   // the id wraps around
}

void test_state_is_locked_for_every_access_and_always_released() {
  Rig rig;
  rig.send(MessageType::MailboxOpen);
  rig.receiver.state();
  rig.receiver.clearMail();
  TEST_ASSERT_TRUE(rig.mutex.locks >= 3);
  TEST_ASSERT_EQUAL(1, rig.mutex.maxDepth);    // never nested
  TEST_ASSERT_EQUAL(0, rig.mutex.depth);       // never left locked
}

// ---------- lost packets ----------
void test_lost_message_is_sent_again_and_counted_once() {
  Rig rig;
  rig.senderRadio.dropTx = {1};
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));

  TEST_ASSERT_EQUAL_UINT(2, rig.senderRadio.sent.size());
  TEST_ASSERT_EQUAL_STRING(kOpen0, rig.senderRadio.sent[1].c_str());   // same id on the retry
  TEST_ASSERT_EQUAL_STRING("NewMail", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
  TEST_ASSERT_TRUE(rig.senderLog.contains("No ACK"));

  TEST_ASSERT_EQUAL_UINT(1, rig.random.calls.size());                  // one back-off, 0.5–2 s
  TEST_ASSERT_EQUAL_UINT32(500, rig.random.calls[0].first);
  TEST_ASSERT_EQUAL_UINT32(2000, rig.random.calls[0].second);
}

void test_lost_ack_causes_a_retransmission_that_is_acked_but_not_counted_again() {
  Rig rig;
  rig.receiverRadio.dropTx = {1};
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));

  TEST_ASSERT_EQUAL_UINT(2, rig.senderRadio.sent.size());
  TEST_ASSERT_EQUAL_STRING("ACK;0 | ACK;0", rig.receiverSent().c_str());
  TEST_ASSERT_EQUAL_STRING("NewMail,Duplicate", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
  TEST_ASSERT_TRUE(rig.receiverLog.contains("Duplicate - already counted"));
}

void test_two_lost_acks_still_count_one_opening() {
  Rig rig;
  rig.receiverRadio.dropTx = {1, 2};
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));
  TEST_ASSERT_EQUAL_UINT(3, rig.senderRadio.sent.size());
  TEST_ASSERT_EQUAL_STRING("NewMail,Duplicate,Duplicate", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
}

void test_sender_gives_up_after_three_attempts() {
  Rig rig;
  rig.senderRadio.dropAll = true;               // receiver switched off / out of range
  uint64_t t0 = rig.world.nowMs();
  TEST_ASSERT_FALSE(rig.send(MessageType::Battery));

  TEST_ASSERT_EQUAL_UINT(3, rig.senderRadio.sent.size());
  TEST_ASSERT_EQUAL_UINT(2, rig.random.calls.size());   // a pause between attempts, none after the last
  TEST_ASSERT_EQUAL_STRING("", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT(3, rig.senderLog.count("No ACK"));
  TEST_ASSERT_TRUE(rig.senderLog.contains("Message was not acknowledged"));
  TEST_ASSERT_EQUAL_UINT32(1, rig.senderState.msgId);   // the id is used up either way

  // three full 4 s waits plus two 1 s pauses plus three transmissions
  uint64_t elapsed = rig.world.nowMs() - t0;
  TEST_ASSERT_TRUE(elapsed >= 3 * 4000 + 2 * 1000 + 3 * sim::airtimeMs(14));
  TEST_ASSERT_TRUE(elapsed < 3 * 4000 + 2 * 1000 + 3 * sim::airtimeMs(14) + 100);
}

void test_message_that_got_through_only_on_the_third_attempt() {
  Rig rig;
  rig.senderRadio.dropTx = {1, 2};
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));
  TEST_ASSERT_EQUAL_UINT(3, rig.senderRadio.sent.size());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
}

// ---------- heartbeat schedule after a send ----------
void test_acknowledged_message_postpones_the_heartbeat_by_a_day() {
  Rig rig;
  rig.send(MessageType::MailboxOpen);
  TEST_ASSERT_EQUAL_UINT32(rig.senderClock.rtcSeconds() + kBatteryReportS,
                           rig.senderState.nextBatteryReportS);
}

void test_unacknowledged_battery_report_is_retried_in_an_hour() {
  Rig rig;
  rig.senderRadio.dropAll = true;
  rig.send(MessageType::Battery);
  TEST_ASSERT_EQUAL_UINT32(rig.senderClock.rtcSeconds() + kBatteryRetryS,
                           rig.senderState.nextBatteryReportS);
}

void test_unacknowledged_flap_event_leaves_the_heartbeat_schedule_alone() {
  Rig rig;
  rig.senderState.nextBatteryReportS = 50000;
  rig.senderRadio.dropAll = true;
  TEST_ASSERT_FALSE(rig.send(MessageType::MailboxOpen));
  TEST_ASSERT_EQUAL_UINT32(50000, rig.senderState.nextBatteryReportS);
}

// ---------- corrupted packets and radio errors ----------
void test_corrupted_message_is_dropped_by_the_receiver_and_sent_again() {
  Rig rig;
  rig.senderRadio.corruptTx = {1};
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));

  TEST_ASSERT_EQUAL_STRING("Ignored,NewMail", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_STRING("ACK;0", rig.receiverSent().c_str());   // nothing acked for the bad copy
  TEST_ASSERT_TRUE(rig.receiverLog.contains("!! Packet dropped: CRC error"));
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
}

void test_corrupted_ack_is_dropped_by_the_sender_and_the_message_sent_again() {
  Rig rig;
  rig.receiverRadio.corruptTx = {1};
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));

  TEST_ASSERT_EQUAL_UINT(2, rig.senderRadio.sent.size());
  TEST_ASSERT_TRUE(rig.senderLog.contains("!! Packet dropped: CRC error"));
  TEST_ASSERT_EQUAL_STRING("NewMail,Duplicate", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
}

void test_failed_transmission_on_the_sender_counts_as_an_attempt() {
  Rig rig;
  rig.senderRadio.failTx = {1};
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));
  TEST_ASSERT_EQUAL_UINT(2, rig.senderRadio.sent.size());
  TEST_ASSERT_TRUE(rig.senderLog.contains("Attempt 1/3: transmit failed"));
  TEST_ASSERT_TRUE(rig.senderLog.contains("Attempt 2/3: sent"));
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
}

void test_failed_ack_transmission_still_counts_the_mail_and_keeps_listening() {
  Rig rig;
  rig.receiverRadio.failTx = {1};
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));
  TEST_ASSERT_TRUE(rig.receiverLog.contains("!! ACK transmit failed"));
  TEST_ASSERT_EQUAL_STRING("NewMail,Duplicate", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
}

// ---------- packets that are not for us ----------
void test_foreign_packet_while_waiting_for_the_ack_is_ignored() {
  Rig rig;
  rig.senderRadio.dropTx = {1};                 // so no ACK is on the air during the first wait
  rig.world.injectAt(rig.world.nowMs() + sim::airtimeMs(sizeof(kOpen0) - 1) + 500, "HELLO");
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));

  TEST_ASSERT_TRUE(rig.senderLog.contains("Received: HELLO"));
  TEST_ASSERT_EQUAL_UINT(2, rig.senderRadio.sent.size());   // HELLO did not end the wait early
  TEST_ASSERT_EQUAL_STRING("Ignored,NewMail", rig.eventNames().c_str());   // receiver heard it too
  TEST_ASSERT_EQUAL_STRING("ACK;0", rig.receiverSent().c_str());           // and did not ACK it
}

void test_foreign_packet_that_overlaps_the_ack_costs_one_retry() {
  // the sender re-arms its radio in the middle of the ACK and therefore misses it
  Rig rig;
  rig.world.injectAt(rig.world.nowMs() + sim::airtimeMs(sizeof(kOpen0) - 1) + 10, "HELLO");
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));

  TEST_ASSERT_EQUAL_UINT(2, rig.senderRadio.sent.size());
  TEST_ASSERT_EQUAL_STRING("NewMail,Duplicate", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
}

void test_sender_keeps_listening_for_the_ack_after_a_foreign_packet() {
  // a slow receiver: its ACK starts 2 s after the message, a foreign packet comes in between
  ReceiverConfig slow;
  slow.ackDelayMs = 2000;
  Rig rig(slow);
  rig.world.injectAt(rig.world.nowMs() + sim::airtimeMs(sizeof(kOpen0) - 1) + 10, "HELLO");
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));

  TEST_ASSERT_TRUE(rig.senderLog.contains("Received: HELLO"));
  TEST_ASSERT_EQUAL_UINT(1, rig.senderRadio.sent.size());   // the ACK was heard, no retry
}

void test_sender_keeps_listening_for_the_ack_after_a_corrupted_packet() {
  ReceiverConfig slow;
  slow.ackDelayMs = 2000;
  Rig rig(slow);
  rig.world.injectAt(rig.world.nowMs() + sim::airtimeMs(sizeof(kOpen0) - 1) + 10, "NOISE", true);
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));

  TEST_ASSERT_TRUE(rig.senderLog.contains("!! Packet dropped: CRC error"));
  TEST_ASSERT_EQUAL_UINT(1, rig.senderRadio.sent.size());
}

void test_ack_for_another_id_does_not_end_the_wait() {
  Rig rig;
  rig.senderRadio.dropAll = true;
  rig.world.injectAt(rig.world.nowMs() + sim::airtimeMs(sizeof(kOpen0) - 1) + 300, "ACK;999");
  TEST_ASSERT_FALSE(rig.send(MessageType::MailboxOpen));
  TEST_ASSERT_TRUE(rig.senderLog.contains("Received: ACK;999"));
  TEST_ASSERT_EQUAL_UINT(3, rig.senderRadio.sent.size());
}

void test_stale_ack_for_the_previous_message_does_not_end_the_wait() {
  Rig rig;
  rig.senderState.msgId = 5;
  rig.senderRadio.dropAll = true;
  rig.world.injectAt(rig.world.nowMs() + sim::airtimeMs(sizeof(kOpen0) - 1) + 300, "ACK;4");
  TEST_ASSERT_FALSE(rig.send(MessageType::MailboxOpen));
  TEST_ASSERT_EQUAL_UINT(3, rig.senderRadio.sent.size());
}

void test_receiver_ignores_acks_and_garbage_and_keeps_listening() {
  Rig rig;
  rig.world.injectAt(100, "ACK;5");
  rig.world.injectAt(3000, "HELLO;1;2");
  rig.world.injectAt(6000, "", true);           // corrupted
  rig.world.advance(9000);
  TEST_ASSERT_EQUAL_STRING("Ignored,Ignored,Ignored", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_STRING("", rig.receiverSent().c_str());
  TEST_ASSERT_FALSE(rig.receiver.state().hasPacket);

  rig.events.clear();
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));     // still works afterwards
  TEST_ASSERT_EQUAL_STRING("NewMail", rig.eventNames().c_str());
}

// ---------- half-duplex and timing ----------
void test_packet_arriving_while_the_receiver_sends_an_ack_is_lost() {
  Rig rig;
  // a second sender starts talking 200 ms after ours finished: the receiver is busy with the ACK
  rig.world.injectAt(rig.world.nowMs() + sim::airtimeMs(sizeof(kOpen0) - 1) + 200,
                     "MAILBOX_OPEN;77;3900");
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));
  rig.world.advance(5000);

  TEST_ASSERT_EQUAL_STRING("NewMail", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
  TEST_ASSERT_EQUAL_UINT(1, rig.receiverRadio.missed);

  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));     // the receiver is listening again
  TEST_ASSERT_EQUAL_UINT32(2, rig.receiver.state().count);
}

void test_ack_that_comes_after_the_timeout_is_not_accepted() {
  ReceiverConfig slow;
  slow.ackDelayMs = 5000;                       // longer than the sender's 4 s wait
  Rig rig(slow);
  TEST_ASSERT_FALSE(rig.send(MessageType::MailboxOpen));

  TEST_ASSERT_EQUAL_UINT(3, rig.senderRadio.sent.size());
  TEST_ASSERT_TRUE(rig.receiverRadio.sent.size() >= 1);     // ACKs were sent, just too late
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);  // the mail itself is not lost
  TEST_ASSERT_TRUE(rig.receiver.state().mail);
}

void test_ack_wait_spans_the_millis_wraparound() {
  // the wait starts ~1.3 s before millis() goes back to 0 and the ACK arrives right around it
  Rig rig(ReceiverConfig(), (1ULL << 32) - 3000);
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));
  TEST_ASSERT_EQUAL_UINT(1, rig.senderRadio.sent.size());   // no premature "No ACK"
  TEST_ASSERT_FALSE(rig.senderLog.contains("No ACK"));
}

void test_exchange_works_across_the_millis_wraparound() {
  // millis() goes back to 0 in the middle of the retry
  Rig rig(ReceiverConfig(), (1ULL << 32) - 3000);
  rig.receiverRadio.dropTx = {1};
  TEST_ASSERT_TRUE(rig.send(MessageType::MailboxOpen));
  TEST_ASSERT_TRUE(rig.world.nowMs() > (1ULL << 32));
  TEST_ASSERT_EQUAL_UINT(2, rig.senderRadio.sent.size());
  TEST_ASSERT_EQUAL_STRING("NewMail,Duplicate", rig.eventNames().c_str());
  TEST_ASSERT_EQUAL_UINT32(1, rig.receiver.state().count);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_message_is_acknowledged_on_the_first_attempt);
  RUN_TEST(test_battery_report_is_acknowledged_but_is_not_mail);
  RUN_TEST(test_ack_is_delayed_so_the_sender_can_switch_to_receive);
  RUN_TEST(test_ack_arrives_well_inside_the_timeout);
  RUN_TEST(test_longest_possible_ack_still_fits_in_the_timeout);
  RUN_TEST(test_state_is_locked_for_every_access_and_always_released);
  RUN_TEST(test_lost_message_is_sent_again_and_counted_once);
  RUN_TEST(test_lost_ack_causes_a_retransmission_that_is_acked_but_not_counted_again);
  RUN_TEST(test_two_lost_acks_still_count_one_opening);
  RUN_TEST(test_sender_gives_up_after_three_attempts);
  RUN_TEST(test_message_that_got_through_only_on_the_third_attempt);
  RUN_TEST(test_acknowledged_message_postpones_the_heartbeat_by_a_day);
  RUN_TEST(test_unacknowledged_battery_report_is_retried_in_an_hour);
  RUN_TEST(test_unacknowledged_flap_event_leaves_the_heartbeat_schedule_alone);
  RUN_TEST(test_corrupted_message_is_dropped_by_the_receiver_and_sent_again);
  RUN_TEST(test_corrupted_ack_is_dropped_by_the_sender_and_the_message_sent_again);
  RUN_TEST(test_failed_transmission_on_the_sender_counts_as_an_attempt);
  RUN_TEST(test_failed_ack_transmission_still_counts_the_mail_and_keeps_listening);
  RUN_TEST(test_foreign_packet_while_waiting_for_the_ack_is_ignored);
  RUN_TEST(test_foreign_packet_that_overlaps_the_ack_costs_one_retry);
  RUN_TEST(test_sender_keeps_listening_for_the_ack_after_a_foreign_packet);
  RUN_TEST(test_sender_keeps_listening_for_the_ack_after_a_corrupted_packet);
  RUN_TEST(test_ack_for_another_id_does_not_end_the_wait);
  RUN_TEST(test_stale_ack_for_the_previous_message_does_not_end_the_wait);
  RUN_TEST(test_receiver_ignores_acks_and_garbage_and_keeps_listening);
  RUN_TEST(test_packet_arriving_while_the_receiver_sends_an_ack_is_lost);
  RUN_TEST(test_ack_that_comes_after_the_timeout_is_not_accepted);
  RUN_TEST(test_ack_wait_spans_the_millis_wraparound);
  RUN_TEST(test_exchange_works_across_the_millis_wraparound);
  return UNITY_END();
}
