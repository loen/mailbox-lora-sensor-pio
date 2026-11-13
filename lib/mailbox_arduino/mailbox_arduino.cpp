#include "mailbox_arduino.h"

#include <SPI.h>
#include <sys/time.h>

#include "heltec_v2_pins.h"
#include "radio_config.h"

namespace mailbox {

namespace {

volatile bool packetFlag = false;       // set by the DIO0 interrupt

void IRAM_ATTR onPacket() {
  packetFlag = true;
}

std::string withCode(const char *what, int code) {
  return std::string(what) + std::to_string(code);
}

}  // namespace

// ================= Logger, clock, random =================
void SerialLogger::log(const std::string &line) {
  Serial.println(line.c_str());
}

uint32_t ArduinoClock::nowMs() {
  return millis();
}

void ArduinoClock::sleepMs(uint32_t ms) {
  delay(ms);
}

uint32_t ArduinoClock::rtcSeconds() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint32_t) tv.tv_sec;
}

uint32_t ArduinoRandom::between(uint32_t lo, uint32_t hi) {
  return (uint32_t) random((long) lo, (long) hi);
}

// ================= Radio =================
RadioLibRadio::RadioLibRadio(Logger &logger, int txLedPin)
    : module_(LORA_NSS, LORA_DIO0, LORA_RST, LORA_DIO1), radio_(&module_), logger_(logger),
      txLedPin_(txLedPin) {}

int RadioLibRadio::begin() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  // RadioLib enables Low Data Rate Optimize automatically for SF11/SF12 at 125 kHz
  int st = radio_.begin(lora::kFrequencyMhz, lora::kBandwidthKhz, lora::kSpreadingFactor,
                        lora::kCodingRate, lora::kSyncWord, lora::kTxPowerDbm,
                        lora::kPreambleLength);
  if (st == RADIOLIB_ERR_NONE) st = radio_.setCRC(true);
  if (st != RADIOLIB_ERR_NONE) return st;
  radio_.setPacketReceivedAction(onPacket);
  return RADIOLIB_ERR_NONE;
}

void RadioLibRadio::sleep() {
  radio_.sleep();
}

bool RadioLibRadio::transmit(const std::string &text) {
  if (txLedPin_ >= 0) digitalWrite(txLedPin_, HIGH);
  int st = radio_.transmit(text.c_str());     // blocks until transmission ends
  if (txLedPin_ >= 0) digitalWrite(txLedPin_, LOW);
  if (st != RADIOLIB_ERR_NONE) logger_.log(withCode("!! transmit failed: ", st));
  return st == RADIOLIB_ERR_NONE;
}

void RadioLibRadio::startReceive() {
  packetFlag = false;                         // also drops the "transmit done" interrupt
  int st = radio_.startReceive();             // continuous receive
  if (st != RADIOLIB_ERR_NONE) logger_.log(withCode("!! startReceive failed: ", st));
}

RxStatus RadioLibRadio::poll(RxPacket &out) {
  if (!packetFlag) return RxStatus::None;
  packetFlag = false;

  String data;
  int st = radio_.readData(data);
  out.rssi = (int) radio_.getRSSI();
  out.snr = radio_.getSNR();
  if (st == RADIOLIB_ERR_NONE) {
    out.text = data.c_str();
    return RxStatus::Packet;
  }
  if (st == RADIOLIB_ERR_CRC_MISMATCH) return RxStatus::CrcError;
  logger_.log(withCode("!! readData failed: ", st));
  return RxStatus::Error;
}

}  // namespace mailbox
