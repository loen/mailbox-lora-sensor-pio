#include <Arduino.h>

#include "driver/rtc_io.h"
#include <SPI.h>
#include <RadioLib.h>
#include <sys/time.h>

#include "heltec_v2_pins.h"
#include "protocol.h"
#include "radio_config.h"
#include "sender_schedule.h"

using namespace mailbox;

#define REED_PIN          GPIO_NUM_13   // reed switch GPIO13 <-> GND, closed = flap open
#define MAX_ATTEMPTS      3             // how many times to send if no ACK
#define ACK_TIMEOUT_MS    4000          // ACK wait time (an ACK at SF12 takes ~1.2 s)
#define SERVICE_WINDOW_MS 10000         // after reset: time to upload firmware / open Serial Monitor

// ---------- Battery ----------
// WiFi LoRa 32 V2.1 (hardware >= 2.3): VBAT -> 220k/100k divider -> GPIO37, the divider is
// switched on together with Vext (GPIO21 LOW). Older boards used GPIO13 instead.
#define BATTERY_PIN       37
#define BATTERY_DIVIDER   3.2f          // (220k + 100k) / 100k – tune against a multimeter if needed
#define BATTERY_SAMPLES   16

SX1276 radio = new Module(LORA_NSS, LORA_DIO0, LORA_RST, LORA_DIO1);

volatile bool packetFlag = false;       // set by the DIO0 interrupt

RTC_DATA_ATTR uint32_t msgId = 0;       // message number – survives deep sleep
RTC_DATA_ATTR uint32_t nextBatteryReportS = 0;   // RTC time (s) of the next battery heartbeat

uint16_t batteryMv = 0;                 // measured once per wake-up, before any transmission

// Seconds on the RTC clock – keeps counting through deep sleep, restarts at 0 on power-on
uint32_t nowS() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint32_t) tv.tv_sec;
}

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

// ================= LoRa =================
void IRAM_ATTR onPacket() {
  packetFlag = true;
}

void startListening() {
  packetFlag = false;
  int st = radio.startReceive();              // continuous receive – no gaps
  if (st != RADIOLIB_ERR_NONE) Serial.printf("!! startReceive failed: %d\n", st);
}

bool configureLoRa() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  // RadioLib enables Low Data Rate Optimize automatically for SF11/SF12 at 125 kHz
  int st = radio.begin(lora::kFrequencyMhz, lora::kBandwidthKhz, lora::kSpreadingFactor,
                       lora::kCodingRate, lora::kSyncWord, lora::kTxPowerDbm,
                       lora::kPreambleLength);
  if (st == RADIOLIB_ERR_NONE) st = radio.setCRC(true);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("!! Radio init failed: %d\n", st);
    return false;
  }
  radio.setPacketReceivedAction(onPacket);
  return true;
}

bool waitForAck(uint32_t id) {
  startListening();
  unsigned long t0 = millis();
  while (millis() - t0 < ACK_TIMEOUT_MS) {
    if (packetFlag) {
      String rx;
      int st = radio.readData(rx);
      if (st == RADIOLIB_ERR_NONE) {
        Serial.printf("Received: %s (RSSI %d, SNR %.1f)\n",
                      rx.c_str(), (int) radio.getRSSI(), radio.getSNR());
        uint32_t ackId;
        if (parseAck(rx.c_str(), ackId) && ackId == id) return true;
      } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
        Serial.println("!! Packet dropped: CRC error");
      } else {
        Serial.printf("!! readData failed: %d\n", st);
      }
      startListening();                                  // foreign or corrupted – keep listening
    }
    delay(2);
  }
  return false;
}

// Sends the message and waits for its ACK. Every message carries the battery voltage,
// so a flap event doubles as a battery report.
bool sendMessage(MessageType type) {
  Message m;
  m.type = type;
  m.id = msgId;
  m.batteryMv = batteryMv;
  String msg = encodeMessage(m).c_str();

  bool acked = false;
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    digitalWrite(LED_PIN, HIGH);
    int st = radio.transmit(msg);             // blocks until transmission ends (~1.3 s)
    digitalWrite(LED_PIN, LOW);
    if (st == RADIOLIB_ERR_NONE) {
      Serial.printf("Attempt %d/%d: sent %s\n", attempt, MAX_ATTEMPTS, msg.c_str());
    } else {
      Serial.printf("Attempt %d/%d: transmit failed: %d\n", attempt, MAX_ATTEMPTS, st);
    }

    if (waitForAck(msgId)) {
      Serial.println("ACK received");
      acked = true;
      break;
    }
    Serial.println("No ACK");
    if (attempt < MAX_ATTEMPTS) delay(random(500, 2000));   // random back-off before retry
  }
  if (!acked) Serial.println("Message was not acknowledged");
  msgId++;
  nextBatteryReportS = nextReportAfterSend(type, acked, nowS(), nextBatteryReportS);
  return acked;
}

// ================= Sleep =================
void goToSleep() {
  bool stillOpen = (digitalRead(REED_PIN) == LOW);

  radio.sleep();                              // put radio to sleep
  digitalWrite(LED_PIN, LOW);
  digitalWrite(VEXT_PIN, HIGH);               // OLED off

  SleepPlan plan = planSleep(stillOpen, nowS(), nextBatteryReportS);
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
  batteryMv = readBatteryMv();                // before any transmission
  Serial.printf("Battery: %u mV\n", batteryMv);
  randomSeed(esp_random());

  if (!configureLoRa()) {
    // no radio – nothing can be sent; don't let an overdue heartbeat wake us every second
    if (nowS() >= nextBatteryReportS) nextBatteryReportS = nowS() + kBatteryRetryS;
    goToSleep();
  }

  switch (decideWakeAction(cause, nowS(), nextBatteryReportS)) {
    case WakeAction::SendMailboxOpen:
      Serial.println("Wake-up: flap opened");
      sendMessage(MessageType::MailboxOpen);
      break;

    case WakeAction::SendBattery:
      Serial.println("Wake-up: battery report");
      sendMessage(MessageType::Battery);
      break;

    case WakeAction::CheckFlapOnly:
      Serial.println("Wake-up: checking whether the flap has closed");
      break;

    case WakeAction::ServiceWindowThenBattery:
      Serial.println("Power-on / reset - service window");
      delay(SERVICE_WINDOW_MS);
      sendMessage(MessageType::Battery);
      break;
  }

  goToSleep();
}

void loop() {
  // unused – after deep sleep the program restarts from setup()
}
