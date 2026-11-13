// Arduino / RadioLib implementations of the interfaces in lib/mailbox_core/mailbox_hal.h.
// The only code that talks to the SX1276 – shared by the sender and the receiver firmware.
#pragma once

#include <Arduino.h>
#include <RadioLib.h>

#include "mailbox_hal.h"

namespace mailbox {

class SerialLogger : public Logger {
 public:
  void log(const std::string &line) override;
};

class ArduinoClock : public Clock {
 public:
  uint32_t nowMs() override;
  void sleepMs(uint32_t ms) override;
  uint32_t rtcSeconds() override;
};

class ArduinoRandom : public RandomSource {
 public:
  uint32_t between(uint32_t lo, uint32_t hi) override;
};

// SX1276 of the Heltec WiFi LoRa 32 V2, configured with the settings from radio_config.h.
// Only one instance may exist: the DIO0 interrupt sets a single shared flag.
class RadioLibRadio : public Radio {
 public:
  // txLedPin: LED that is lit while transmitting, -1 = none
  explicit RadioLibRadio(Logger &logger, int txLedPin = -1);

  // Returns the RadioLib status code, RADIOLIB_ERR_NONE (0) on success.
  int begin();
  void sleep();

  bool transmit(const std::string &text) override;
  void startReceive() override;
  RxStatus poll(RxPacket &out) override;

 private:
  Module module_;
  SX1276 radio_;
  Logger &logger_;
  int txLedPin_;
};

}  // namespace mailbox
