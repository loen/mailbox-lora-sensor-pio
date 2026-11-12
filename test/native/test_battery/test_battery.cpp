#include <unity.h>

#include "battery.h"

using namespace mailbox;

void setUp() {}
void tearDown() {}

void test_full_and_above() {
  TEST_ASSERT_EQUAL(100, batteryPercent(4200));
  TEST_ASSERT_EQUAL(100, batteryPercent(4350));   // on the charger
}

void test_empty_and_below() {
  TEST_ASSERT_EQUAL(0, batteryPercent(3300));
  TEST_ASSERT_EQUAL(0, batteryPercent(3000));
  TEST_ASSERT_EQUAL(0, batteryPercent(0));
  TEST_ASSERT_EQUAL(0, batteryPercent(-100));
}

void test_curve_points() {
  TEST_ASSERT_EQUAL(90, batteryPercent(4100));
  TEST_ASSERT_EQUAL(80, batteryPercent(4000));
  TEST_ASSERT_EQUAL(65, batteryPercent(3900));
  TEST_ASSERT_EQUAL(45, batteryPercent(3800));
  TEST_ASSERT_EQUAL(30, batteryPercent(3750));
  TEST_ASSERT_EQUAL(20, batteryPercent(3700));
  TEST_ASSERT_EQUAL(12, batteryPercent(3650));
  TEST_ASSERT_EQUAL(7, batteryPercent(3600));
  TEST_ASSERT_EQUAL(3, batteryPercent(3500));
}

void test_interpolation() {
  TEST_ASSERT_EQUAL(95, batteryPercent(4150));    // halfway 4100..4200
  TEST_ASSERT_EQUAL(55, batteryPercent(3850));    // halfway 3800..3900
  TEST_ASSERT_EQUAL(5, batteryPercent(3550));     // halfway 3500..3600
  TEST_ASSERT_EQUAL(1, batteryPercent(3400));     // 3300..3500 covers 0..3 %, rounded down
}

void test_interpolation_steps_at_exact_millivolts() {
  // 4100..4200 mV is 1 % per 10 mV, rounded down – the step must land exactly on the 10 mV mark
  TEST_ASSERT_EQUAL(90, batteryPercent(4109));
  TEST_ASSERT_EQUAL(91, batteryPercent(4110));
  TEST_ASSERT_EQUAL(98, batteryPercent(4189));
  TEST_ASSERT_EQUAL(99, batteryPercent(4190));
  TEST_ASSERT_EQUAL(99, batteryPercent(4199));
  // 3800..3900 mV is 1 % per 5 mV
  TEST_ASSERT_EQUAL(45, batteryPercent(3804));
  TEST_ASSERT_EQUAL(46, batteryPercent(3805));
  TEST_ASSERT_EQUAL(64, batteryPercent(3899));
}

void test_just_below_each_curve_point() {
  TEST_ASSERT_EQUAL(89, batteryPercent(4099));
  TEST_ASSERT_EQUAL(79, batteryPercent(3999));
  TEST_ASSERT_EQUAL(29, batteryPercent(3749));
  TEST_ASSERT_EQUAL(2, batteryPercent(3499));
  TEST_ASSERT_EQUAL(0, batteryPercent(3301));
}

void test_never_increases_as_voltage_drops() {
  int previous = 100;
  for (int mv = 4300; mv >= 3200; mv--) {
    int pct = batteryPercent(mv);
    TEST_ASSERT_TRUE(pct <= previous);
    TEST_ASSERT_TRUE(pct >= 0 && pct <= 100);
    previous = pct;
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_full_and_above);
  RUN_TEST(test_empty_and_below);
  RUN_TEST(test_curve_points);
  RUN_TEST(test_interpolation);
  RUN_TEST(test_interpolation_steps_at_exact_millivolts);
  RUN_TEST(test_just_below_each_curve_point);
  RUN_TEST(test_never_increases_as_voltage_drops);
  return UNITY_END();
}
