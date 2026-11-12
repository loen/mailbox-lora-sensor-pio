#include <Arduino.h>

#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <SSD1306Wire.h>
#include "arduino_secrets.h"   // Wi-Fi and MQTT – see arduino_secrets.h.example

#include "battery.h"
#include "heltec_v2_pins.h"
#include "mail_tracker.h"
#include "protocol.h"
#include "radio_config.h"
#include "time_format.h"

using namespace mailbox;

// ---------- Wi-Fi (2.4 GHz only) ----------
const char* WIFI_SSID    = SECRET_WIFI_SSID;
const char* WIFI_PASS    = SECRET_WIFI_PASS;

// ---------- MQTT (Mosquitto in Home Assistant) ----------
const char* MQTT_HOST    = SECRET_MQTT_HOST;
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER    = SECRET_MQTT_USER;
const char* MQTT_PASS    = SECRET_MQTT_PASS;

#define SCREEN_TIMEOUT_MS 60000   // OLED turns off after this much inactivity (prevents burn-in)

// ---------- MQTT topics (must match configuration.yaml) ----------
#define MQTT_CLIENT_ID "mailbox_lora"
#define T_AVAIL  "custom/mailbox/availability"   // online / offline (LWT)
#define T_MAIL   "custom/mailbox/mail"           // ON / OFF
#define T_COUNT  "custom/mailbox/count"
#define T_RSSI   "custom/mailbox/rssi"
#define T_SNR    "custom/mailbox/snr"
#define T_CMD    "custom/mailbox/cmd"            // RESET from Home Assistant
#define T_BATT   "custom/mailbox/battery"          // sender battery, 0–100 %
#define T_BATT_V "custom/mailbox/battery_voltage"  // sender battery, volts

SX1276 radio = new Module(LORA_NSS, LORA_DIO0, LORA_RST, LORA_DIO1);
SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);

volatile bool packetFlag = false;       // set by the DIO0 interrupt

MailTracker tracker;                              // state shared between cores, guarded by stateMux
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;

volatile bool statePending   = false;   // LoRa -> network: "publish new state"
volatile bool resetRequested = false;   // network -> LoRa: "RESET from Home Assistant"
volatile bool netWifiOk      = false;   // network status for the display
volatile bool netMqttOk      = false;

WiFiClient espClient;
PubSubClient mqtt(espClient);

unsigned long mailTime = 0;

bool screenOn = true;                   // OLED state (core 1 only)
unsigned long screenOnSince = 0;        // last activity that kept the display on

// ================= Radio =================
void IRAM_ATTR onPacket() {
  packetFlag = true;
}

void startListening() {
  packetFlag = false;
  int st = radio.startReceive();              // continuous receive
  if (st != RADIOLIB_ERR_NONE) Serial.printf("!! startReceive failed: %d\n", st);
}

MailState getState() {
  taskENTER_CRITICAL(&stateMux);
  MailState copy = tracker.state();
  taskEXIT_CRITICAL(&stateMux);
  return copy;
}

// ================= Core 0: Wi-Fi + MQTT =================
void publishState() {
  MailState s = getState();
  mqtt.publish(T_MAIL, s.mail ? "ON" : "OFF", true);
  mqtt.publish(T_COUNT, String(s.count).c_str(), true);
  if (s.hasPacket) {
    mqtt.publish(T_RSSI, String(s.rssi).c_str(), true);
    mqtt.publish(T_SNR, String(s.snr, 1).c_str(), true);
  }
  if (s.batteryMv > 0) {
    mqtt.publish(T_BATT, String(batteryPercent(s.batteryMv)).c_str(), true);
    mqtt.publish(T_BATT_V, String(s.batteryMv / 1000.0, 2).c_str(), true);
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int len) {
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char) payload[i];
  if (String(topic) == T_CMD && msg == "RESET") {
    Serial.println("MQTT: RESET from Home Assistant");
    resetRequested = true;                        // handled by the LoRa loop
  }
}

void netTask(void *param) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setSocketTimeout(2);

  unsigned long lastTry = 0;

  for (;;) {
    netWifiOk = (WiFi.status() == WL_CONNECTED);

    if (netWifiOk && !mqtt.connected() && millis() - lastTry >= 5000) {
      lastTry = millis();
      Serial.print("MQTT: connecting... ");
      // may block for a few seconds – fine here, LoRa runs on the other core
      if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS, T_AVAIL, 0, true, "offline")) {
        Serial.println("OK");
        mqtt.publish(T_AVAIL, "online", true);
        mqtt.subscribe(T_CMD);
        statePending = true;                      // publish current state after connecting
      } else {
        Serial.printf("failed, rc=%d\n", mqtt.state());
      }
    }

    netMqttOk = mqtt.connected();
    if (netMqttOk) {
      mqtt.loop();
      if (statePending) {
        statePending = false;
        publishState();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ================= Core 1: LoRa, display, button =================
void configureLoRa() {
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  // RadioLib enables Low Data Rate Optimize automatically for SF11/SF12 at 125 kHz
  int st = radio.begin(lora::kFrequencyMhz, lora::kBandwidthKhz, lora::kSpreadingFactor,
                       lora::kCodingRate, lora::kSyncWord, lora::kTxPowerDbm,
                       lora::kPreambleLength);
  if (st == RADIOLIB_ERR_NONE) st = radio.setCRC(true);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("!! Radio init failed: %d\n", st);
    display.clear();
    display.drawString(0, 0, "Radio init failed: " + String(st));
    display.display();
    for (;;) delay(1000);
  }
  radio.setPacketReceivedAction(onPacket);
}

void drawScreen() {
  if (!screenOn) return;                      // display off – nothing to draw
  MailState s = getState();

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  display.setFont(ArialMT_Plain_16);
  display.drawString(0, 0, s.mail ? "MAIL!" : "No mail");

  display.setFont(ArialMT_Plain_10);
  if (s.mail) {
    display.drawString(0, 20, "Opened " + String(formatAgo(millis() - mailTime).c_str()) + " ago");
  }
  String net = String("WiFi:") + (netWifiOk ? "OK" : "--") +
               " MQTT:" + (netMqttOk ? "OK" : "--");
  display.drawString(0, 36, "Opens: " + String(s.count) + "  " + net);
  if (s.hasPacket) {
    display.drawString(0, 50, "RSSI " + String(s.rssi) + "  SNR " + String(s.snr, 1));
  }
  if (s.batteryMv > 0) {
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(128, 3, String(batteryPercent(s.batteryMv)) + "%");
  }
  display.display();
}

// Turns the display on (if needed) and restarts the inactivity timeout
void wakeScreen() {
  screenOnSince = millis();
  if (!screenOn) {
    screenOn = true;
    display.displayOn();
  }
  drawScreen();
}

void sleepScreen() {
  screenOn = false;
  display.displayOff();               // the LED still signals new mail
}

void clearMail() {
  taskENTER_CRITICAL(&stateMux);
  tracker.clearMail();
  taskEXIT_CRITICAL(&stateMux);
  digitalWrite(LED_PIN, LOW);
  statePending = true;
  drawScreen();
}

void sendAck(uint32_t id) {
  delay(100);                                 // give the sender time to switch to receive
  String ack = encodeAck(id).c_str();
  int st = radio.transmit(ack);               // blocks until transmission ends
  startListening();                           // back to continuous receive
  if (st == RADIOLIB_ERR_NONE) Serial.println("Sent " + ack);
  else Serial.printf("!! ACK transmit failed: %d\n", st);
}

void handlePacket() {
  String p;
  int st = radio.readData(p);
  int rssi = (int) radio.getRSSI();
  float snr = radio.getSNR();
  if (st != RADIOLIB_ERR_NONE) {
    if (st == RADIOLIB_ERR_CRC_MISMATCH) Serial.println("!! Packet dropped: CRC error");
    else Serial.printf("!! readData failed: %d\n", st);
    startListening();
    return;
  }

  Serial.printf("Received: %s | RSSI %d dBm | SNR %.1f dB\n", p.c_str(), rssi, snr);

  Message msg;
  if (!parseMessage(p.c_str(), msg)) {
    startListening();                         // foreign packet – go straight back to listening
    return;
  }
  sendAck(msg.id);                            // ACK immediately (radio returns to receive afterwards)

  taskENTER_CRITICAL(&stateMux);
  bool isNew = tracker.onMessage(msg, rssi, snr, millis());
  taskEXIT_CRITICAL(&stateMux);

  if (msg.type == MessageType::Battery) {
    Serial.printf("Battery report: %u mV (%d %%)\n", msg.batteryMv, batteryPercent(msg.batteryMv));
  } else if (isNew) {
    mailTime = millis();
    digitalWrite(LED_PIN, HIGH);
  } else {
    Serial.println("Duplicate - already counted");
  }

  statePending = true;                        // core 0 will publish this to MQTT
  if (isNew) wakeScreen();                    // new mail – show it
  else drawScreen();
}

// ================= Setup / loop =================
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(OLED_RST, OUTPUT);                  // hardware reset of the OLED
  digitalWrite(OLED_RST, LOW);
  delay(20);
  digitalWrite(OLED_RST, HIGH);
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  configureLoRa();
  wakeScreen();                               // display on, timeout starts now

  // Wi-Fi and MQTT in a separate task on core 0 (loop() runs on core 1)
  xTaskCreatePinnedToCore(netTask, "net", 8192, NULL, 1, NULL, 0);

  Serial.println("Receiver ready, waiting for the mailbox to open...");
  startListening();
}

void loop() {
  if (packetFlag) {
    packetFlag = false;
    handlePacket();
  }

  if (resetRequested) {                       // RESET from Home Assistant
    resetRequested = false;
    clearMail();
  }

  static bool lastBtn = HIGH;
  bool btn = digitalRead(BUTTON_PIN);
  if (btn == LOW && lastBtn == HIGH) {        // PRG pressed (falling edge)
    if (!screenOn) {
      wakeScreen();                           // first press only wakes the display
    } else {
      screenOnSince = millis();               // keep the display on
      if (getState().mail) clearMail();       // PRG = "mail collected"
    }
    delay(50);                                // debounce
  }
  lastBtn = btn;

  if (screenOn && millis() - screenOnSince >= SCREEN_TIMEOUT_MS) {
    sleepScreen();
  }

  static unsigned long lastDraw = 0;
  if (millis() - lastDraw >= 1000) {          // refreshes "x min ago" and network status
    lastDraw = millis();
    drawScreen();
  }
  delay(5);
}
