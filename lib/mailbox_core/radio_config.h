// LoRa settings shared by the sender and the receiver – both sides must use the same values
#pragma once

#include <stdint.h>

namespace mailbox {
namespace lora {

constexpr float kFrequencyMhz      = 869.525f;  // 869.4–869.65 MHz sub-band: 20 dBm allowed, 10% duty cycle
constexpr float kBandwidthKhz      = 125.0f;
constexpr uint8_t kSpreadingFactor = 12;        // 12 = maximum range
constexpr uint8_t kCodingRate      = 8;         // 4/8 = strongest error correction
constexpr uint8_t kSyncWord        = 0x34;      // value used by the original Heltec library
constexpr uint16_t kPreambleLength = 8;
constexpr int8_t kTxPowerDbm       = 20;        // maximum for SX1276 (PA_BOOST)

}  // namespace lora
}  // namespace mailbox
