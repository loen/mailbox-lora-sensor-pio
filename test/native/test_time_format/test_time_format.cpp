#include <stdint.h>

#include <unity.h>

#include "time_format.h"

using namespace mailbox;

void setUp() {}
void tearDown() {}

void test_seconds() {
  TEST_ASSERT_EQUAL_STRING("0 s", formatAgo(0).c_str());
  TEST_ASSERT_EQUAL_STRING("0 s", formatAgo(999).c_str());
  TEST_ASSERT_EQUAL_STRING("1 s", formatAgo(1000).c_str());
  TEST_ASSERT_EQUAL_STRING("59 s", formatAgo(59999).c_str());
}

void test_minutes() {
  TEST_ASSERT_EQUAL_STRING("1 min", formatAgo(60000).c_str());
  TEST_ASSERT_EQUAL_STRING("1 min", formatAgo(119999).c_str());
  TEST_ASSERT_EQUAL_STRING("59 min", formatAgo(3599999).c_str());
}

void test_hours() {
  TEST_ASSERT_EQUAL_STRING("1 h 0 min", formatAgo(3600000).c_str());
  TEST_ASSERT_EQUAL_STRING("2 h 7 min", formatAgo((2UL * 3600 + 7 * 60 + 30) * 1000).c_str());
  TEST_ASSERT_EQUAL_STRING("1 h 59 min", formatAgo((3600 + 59 * 60 + 59) * 1000).c_str());
  TEST_ASSERT_EQUAL_STRING("5 h 30 min", formatAgo((5 * 3600 + 30 * 60) * 1000).c_str());
  TEST_ASSERT_EQUAL_STRING("49 h 0 min", formatAgo(49UL * 3600 * 1000).c_str());
}

void test_largest_32_bit_value() {
  // millis() wraps after ~49.7 days
  TEST_ASSERT_EQUAL_STRING("1193 h 2 min", formatAgo(UINT32_MAX).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_seconds);
  RUN_TEST(test_minutes);
  RUN_TEST(test_hours);
  RUN_TEST(test_largest_32_bit_value);
  return UNITY_END();
}
