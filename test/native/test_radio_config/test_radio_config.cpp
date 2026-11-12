// The LoRa settings are a contract with boards that are already deployed: a sender in the
// mailbox keeps running its old firmware. Changing any of these values on purpose means
// reflashing both boards – this test makes sure it never happens by accident.
#include <unity.h>

#include "radio_config.h"

using namespace mailbox;

void setUp() {}
void tearDown() {}

void test_lora_settings_match_deployed_boards() {
  TEST_ASSERT_EQUAL_FLOAT(869.525f, lora::kFrequencyMhz);
  TEST_ASSERT_EQUAL_FLOAT(125.0f, lora::kBandwidthKhz);
  TEST_ASSERT_EQUAL_UINT8(12, lora::kSpreadingFactor);
  TEST_ASSERT_EQUAL_UINT8(8, lora::kCodingRate);
  TEST_ASSERT_EQUAL_HEX8(0x34, lora::kSyncWord);
  TEST_ASSERT_EQUAL_UINT16(8, lora::kPreambleLength);
}

void test_tx_power_within_sx1276_and_band_limits() {
  // PA_BOOST range of the SX1276; 20 dBm is also the legal limit of the 869.4–869.65 MHz sub-band
  TEST_ASSERT_TRUE(lora::kTxPowerDbm >= 2);
  TEST_ASSERT_TRUE(lora::kTxPowerDbm <= 20);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_lora_settings_match_deployed_boards);
  RUN_TEST(test_tx_power_within_sx1276_and_band_limits);
  return UNITY_END();
}
