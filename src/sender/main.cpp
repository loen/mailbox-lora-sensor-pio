// Sender firmware: the board in the mailbox. This file only deals with the hardware (battery
// measurement, reed switch, deep sleep); what to send and when is decided by MailboxSender
// in lib/mailbox_core.
#include <Arduino.h>

#include "driver/rtc_io.h"

#include "heltec_v2_pins.h"
#include "mailbox_arduino.h"
#include "mailbox_sender.h"

using namespace mailbox;

#define REED_PIN          GPIO_NUM_13   // reed switch GPIO13 <-> GND, closed = flap open

// ---------- Battery ----------
// WiFi LoRa 32 V2.1 (hardware >= 2.3): VBAT -> 220k/100k divider -> GPIO37, the divider is
// switched on together with Vext (GPIO21 LOW). Older boards used GPIO13 instead.
#define BATTERY_PIN       37
#define BATTERY_DIVIDER   3.2f          // (220k + 100k) / 100k – tune against a multimeter if needed
#define BATTERY_SAMPLES   16

// message number and next heartbeat time – survive deep sleep
RTC_DATA_ATTR SenderState senderState = {0, 0};

SerialLogger logger;
ArduinoClock clock_;
ArduinoRandom randomSource;
RadioLibRadio radio(logger, LED_PIN);   // LED is lit while transmitting
MailboxSender sender(radio, clock_, randomSource, logger, senderState);

// ================= Battery =================
// Must run while the radio is idle: a 20 dBm transmission pulls the voltage down noticeably.
uint16_t readBatteryMv() {
  digitalWrite(VEXT_PIN, LOW);                // enable the divider (also powers the OLED rail)
  analogSetPinAttenuation(BATTERY_PIN, ADC_6db);   // ~0.15–1.75 V; 4.2 V / 3.2 = 1.31 V
  delay(20);                                  // let the divider settle
  analogReadMilliVolts(BATTERY_PIN);          // discard the first sample
  uint32_t sum = 0;
  for (int i = 0; i < BATTERY_SAMPLES; i++) sum += analogReadMilliVolts(BATTERY_PIN);
  digitalWrite(VEXT_PIN, HIGH);
  return (uint16_t) (sum / BATTERY_SAMPLES * BATTERY_DIVIDER);
}

// ================= Sleep =================
void goToSleep() {
  bool stillOpen = (digitalRead(REED_PIN) == LOW);

  radio.sleep();                              // put radio to sleep
  digitalWrite(LED_PIN, LOW);
  digitalWrite(VEXT_PIN, HIGH);               // OLED off

  SleepPlan plan = sender.planSleep(stillOpen);
  esp_sleep_enable_timer_wakeup(plan.seconds * 1000000ULL);
  if (plan.wakeOnReed) {
    Serial.printf("Sleeping until the flap opens (battery report in %u s)\n", plan.seconds);
    esp_sleep_enable_ext0_wakeup(REED_PIN, 0);   // wake when GPIO13 = LOW (closed)
    rtc_gpio_pullup_en(REED_PIN);                // internal pull-up active during deep sleep
    rtc_gpio_pulldown_dis(REED_PIN);
  } else {
    Serial.printf("Flap still open, checking again in %u s\n", plan.seconds);
  }
  Serial.flush();
  esp_deep_sleep_start();
}

WakeCause wakeCause() {
  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT0:  return WakeCause::Reed;
    case ESP_SLEEP_WAKEUP_TIMER: return WakeCause::Timer;
    default:                     return WakeCause::PowerOn;
  }
}

// ================= Setup / loop =================
void setup() {
  WakeCause cause = wakeCause();

  rtc_gpio_deinit(REED_PIN);                  // after wake-up, restore the pin to a regular GPIO
  pinMode(REED_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, HIGH);               // display not needed in the mailbox
  pinMode(LED_PIN, OUTPUT);
  uint16_t batteryMv = readBatteryMv();       // once per wake-up, before any transmission
  Serial.printf("Battery: %u mV\n", batteryMv);
  randomSeed(esp_random());

  int st = radio.begin();
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("!! Radio init failed: %d\n", st);
    // no radio – nothing can be sent; don't let an overdue heartbeat wake us every second
    uint32_t now = clock_.rtcSeconds();
    if (now >= senderState.nextBatteryReportS) senderState.nextBatteryReportS = now + kBatteryRetryS;
    goToSleep();
  }

  sender.onWake(cause, batteryMv);
  goToSleep();
}

void loop() {
  // unused – after deep sleep the program restarts from setup()
}
