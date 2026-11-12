#include <unity.h>

#include "protocol.h"

using namespace mailbox;

void setUp() {}
void tearDown() {}

static Message make(MessageType type, uint32_t id, uint16_t mv) {
  Message m;
  m.type = type;
  m.id = id;
  m.batteryMv = mv;
  return m;
}

// ---------- encodeMessage ----------
void test_encode_mailbox_open() {
  TEST_ASSERT_EQUAL_STRING("MAILBOX_OPEN;17;3987",
                           encodeMessage(make(MessageType::MailboxOpen, 17, 3987)).c_str());
}

void test_encode_battery() {
  TEST_ASSERT_EQUAL_STRING("BATTERY;0;4120",
                           encodeMessage(make(MessageType::Battery, 0, 4120)).c_str());
}

void test_encode_max_values() {
  TEST_ASSERT_EQUAL_STRING("BATTERY;4294967295;65535",
                           encodeMessage(make(MessageType::Battery, UINT32_MAX, UINT16_MAX)).c_str());
}

// ---------- parseMessage ----------
void test_parse_mailbox_open() {
  Message m;
  TEST_ASSERT_TRUE(parseMessage("MAILBOX_OPEN;17;3987", m));
  TEST_ASSERT_TRUE(m.type == MessageType::MailboxOpen);
  TEST_ASSERT_EQUAL_UINT32(17, m.id);
  TEST_ASSERT_EQUAL_UINT16(3987, m.batteryMv);
}

void test_parse_battery() {
  Message m;
  TEST_ASSERT_TRUE(parseMessage("BATTERY;5;4120", m));
  TEST_ASSERT_TRUE(m.type == MessageType::Battery);
  TEST_ASSERT_EQUAL_UINT32(5, m.id);
  TEST_ASSERT_EQUAL_UINT16(4120, m.batteryMv);
}

void test_parse_without_battery_field() {
  Message m = make(MessageType::Battery, 99, 1234);
  TEST_ASSERT_TRUE(parseMessage("MAILBOX_OPEN;3", m));
  TEST_ASSERT_EQUAL_UINT32(3, m.id);
  TEST_ASSERT_EQUAL_UINT16(0, m.batteryMv);
}

void test_parse_ignores_extra_fields() {
  Message m;
  TEST_ASSERT_TRUE(parseMessage("BATTERY;5;4120;future;stuff", m));
  TEST_ASSERT_EQUAL_UINT32(5, m.id);
  TEST_ASSERT_EQUAL_UINT16(4120, m.batteryMv);
}

void test_parse_max_values() {
  Message m;
  TEST_ASSERT_TRUE(parseMessage("BATTERY;4294967295;65535", m));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, m.id);
  TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, m.batteryMv);
}

void test_parse_round_trip() {
  Message in = make(MessageType::MailboxOpen, 123456, 3701);
  Message out;
  TEST_ASSERT_TRUE(parseMessage(encodeMessage(in), out));
  TEST_ASSERT_TRUE(out.type == in.type);
  TEST_ASSERT_EQUAL_UINT32(in.id, out.id);
  TEST_ASSERT_EQUAL_UINT16(in.batteryMv, out.batteryMv);
}

void test_parse_accepts_leading_zeros() {
  Message m;
  TEST_ASSERT_TRUE(parseMessage("MAILBOX_OPEN;007;04000", m));
  TEST_ASSERT_EQUAL_UINT32(7, m.id);
  TEST_ASSERT_EQUAL_UINT16(4000, m.batteryMv);
  TEST_ASSERT_TRUE(parseMessage("BATTERY;0000000001", m));       // 10 digits is the limit
  TEST_ASSERT_EQUAL_UINT32(1, m.id);
}

void test_parse_rejects_malformed() {
  const char *bad[] = {
    "",
    "MAILBOX_OPEN",              // no id
    "MAILBOX_OPEN;",             // empty id
    "MAILBOX_OPEN;abc",
    "MAILBOX_OPEN;-1",
    "MAILBOX_OPEN; 1",
    "MAILBOX_OPEN;1x",
    "MAILBOX_OPEN;+1",
    "MAILBOX_OPEN;1.5",
    "MAILBOX_OPEN;0x10",
    "MAILBOX_OPEN;4294967296",   // id overflows 32 bits
    "MAILBOX_OPEN;9999999999",   // 10 digits, still too large
    "MAILBOX_OPEN;00000000001",  // more than 10 digits
    "MAILBOX_OPEN;1;4000\n",     // trailing newline
    "MAILBOX_OPEN;1;4000 ",
    " MAILBOX_OPEN;1;4000",
    "MAILBOX_OPEN;1;",           // empty battery field
    "MAILBOX_OPEN;1;x",
    "MAILBOX_OPEN;1;65536",      // battery overflows 16 bits
    "MAILBOX_OPENED;1;4000",     // unknown type
    "mailbox_open;1;4000",
    "HELLO;1;4000",
    "ACK;1",                     // an ACK is not a message
    ";1;4000",
  };
  for (const char *text : bad) {
    Message m = make(MessageType::Battery, 99, 1234);
    TEST_ASSERT_FALSE_MESSAGE(parseMessage(text, m), text);
    // a failed parse must not touch the output
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(99, m.id, text);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1234, m.batteryMv, text);
  }
}

// ---------- ACK ----------
void test_encode_ack() {
  TEST_ASSERT_EQUAL_STRING("ACK;42", encodeAck(42).c_str());
  TEST_ASSERT_EQUAL_STRING("ACK;0", encodeAck(0).c_str());
}

void test_parse_ack() {
  uint32_t id = 0;
  TEST_ASSERT_TRUE(parseAck("ACK;42", id));
  TEST_ASSERT_EQUAL_UINT32(42, id);
  TEST_ASSERT_TRUE(parseAck(encodeAck(UINT32_MAX), id));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, id);
}

void test_parse_ack_rejects_malformed() {
  const char *bad[] = {
    "", "ACK", "ACK;", "ACK;x", "ACK;1;2", "ACK;-1", "ACK;+1", "ACK;4294967296",
    "ACK;1\n", "ACK;1 ", " ACK;1", "ACK;;1",
    "ack;1", "NACK;1", "MAILBOX_OPEN;1;4000",
  };
  for (const char *text : bad) {
    uint32_t id = 7;
    TEST_ASSERT_FALSE_MESSAGE(parseAck(text, id), text);
  }
}

void test_ack_for_another_message_has_a_different_id() {
  // the sender compares the parsed id with the one it is waiting for
  uint32_t id = 0;
  TEST_ASSERT_TRUE(parseAck("ACK;12", id));
  TEST_ASSERT_NOT_EQUAL(1, id);
  TEST_ASSERT_NOT_EQUAL(2, id);
  TEST_ASSERT_TRUE(parseAck("ACK;007", id));       // leading zeros, same rule as messages
  TEST_ASSERT_EQUAL_UINT32(7, id);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_encode_mailbox_open);
  RUN_TEST(test_encode_battery);
  RUN_TEST(test_encode_max_values);
  RUN_TEST(test_parse_mailbox_open);
  RUN_TEST(test_parse_battery);
  RUN_TEST(test_parse_without_battery_field);
  RUN_TEST(test_parse_ignores_extra_fields);
  RUN_TEST(test_parse_max_values);
  RUN_TEST(test_parse_round_trip);
  RUN_TEST(test_parse_accepts_leading_zeros);
  RUN_TEST(test_parse_rejects_malformed);
  RUN_TEST(test_encode_ack);
  RUN_TEST(test_parse_ack);
  RUN_TEST(test_parse_ack_rejects_malformed);
  RUN_TEST(test_ack_for_another_message_has_a_different_id);
  return UNITY_END();
}
