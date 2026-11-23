// Receiver firmware: the indoor board. This file deals with the hardware and the network
// (OLED, button, LED, Wi-Fi, MQTT); packets, ACKs and the mail state are handled by
// MailboxReceiver in lib/mailbox_core.
#include <Arduino.h>

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <SSD1306Wire.h>
#include "arduino_secrets.h"   // Wi-Fi and MQTT – see arduino_secrets.h.example

#include "battery.h"
#include "heltec_v2_pins.h"
#include "mailbox_arduino.h"
#include "mailbox_receiver.h"
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
#define WIFI_RESTART_MS   60000   // restart the Wi-Fi connection if auto-reconnect has not
                                  // brought it back within this time
#define MQTT_RETRY_MS     5000    // pause between MQTT connection attempts

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

// The mail state is read by the network task on core 0 and written by loop() on core 1.
class PortMutex : public Mutex {
 public:
  void lock() override { taskENTER_CRITICAL(&mux_); }
  void unlock() override { taskEXIT_CRITICAL(&mux_); }

 private:
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

SerialLogger logger;
ArduinoClock clock_;
PortMutex stateMutex;
RadioLibRadio radio(logger);
MailboxReceiver receiver(radio, clock_, logger, stateMutex);

SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL);

volatile bool statePending   = false;   // LoRa -> network: "publish new state"
volatile bool resetRequested = false;   // network -> LoRa: "RESET from Home Assistant"
volatile bool netWifiOk      = false;   // network status for the display
volatile bool netMqttOk      = false;
volatile int netMqttState    = MQTT_DISCONNECTED;   // PubSubClient state code, shown when not connected

WiFiClient espClient;
PubSubClient mqtt(espClient);

unsigned long mailTime = 0;

bool screenOn = true;                   // OLED state (core 1 only)
unsigned long screenOnSince = 0;        // last activity that kept the display on

// ================= Core 0: Wi-Fi + MQTT =================
// Returns false if any of the messages could not be sent.
bool publishState() {
  MailState s = receiver.state();
  bool ok = mqtt.publish(T_MAIL, s.mail ? "ON" : "OFF", true);
  ok &= mqtt.publish(T_COUNT, String(s.count).c_str(), true);
  if (s.hasPacket) {
    ok &= mqtt.publish(T_RSSI, String(s.rssi).c_str(), true);
    ok &= mqtt.publish(T_SNR, String(s.snr, 1).c_str(), true);
  }
  if (s.batteryMv > 0) {
    ok &= mqtt.publish(T_BATT, String(batteryPercent(s.batteryMv)).c_str(), true);
    ok &= mqtt.publish(T_BATT_V, String(s.batteryMv / 1000.0, 2).c_str(), true);
  }
  return ok;
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
  unsigned long wifiDownSince = millis();

  for (;;) {
    bool wifiOk = (WiFi.status() == WL_CONNECTED);
    if (wifiOk != netWifiOk) {
      Serial.println(wifiOk ? "Wi-Fi: connected" : "Wi-Fi: connection lost");
      netWifiOk = wifiOk;
    }

    // Auto-reconnect does not always bring the connection back (e.g. after a long access
    // point outage), so start it again from scratch when it has been down for too long.
    if (wifiOk) {
      wifiDownSince = millis();
    } else if (millis() - wifiDownSince >= WIFI_RESTART_MS) {
      Serial.println("Wi-Fi: still down, restarting the connection");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      wifiDownSince = millis();
    }

    if (wifiOk && !mqtt.connected() && millis() - lastTry >= MQTT_RETRY_MS) {
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
    netMqttState = mqtt.state();
    if (netMqttOk) {
      mqtt.loop();
      if (statePending) {
        // cleared before publishing, so a change made meanwhile on core 1 is not lost
        statePending = false;
        if (!publishState()) {
          Serial.println("MQTT: publish failed, will retry");
          statePending = true;                    // sent again once the connection is back
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ================= Core 1: LoRa, display, button =================
void configureLoRa() {
  int st = radio.begin();
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("!! Radio init failed: %d\n", st);
    display.clear();
    display.drawString(0, 0, "Radio init failed: " + String(st));
    display.display();
    for (;;) delay(1000);
  }
}

void drawScreen() {
  if (!screenOn) return;                      // display off – nothing to draw
  MailState s = receiver.state();

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  display.setFont(ArialMT_Plain_16);
  display.drawString(0, 0, s.mail ? "MAIL!" : "No mail");

  display.setFont(ArialMT_Plain_10);
  if (s.mail) {
    display.drawString(0, 20, "Opened " + String(formatAgo(millis() - mailTime).c_str()) + " ago");
  }
  // MQTT shows the PubSubClient state code while disconnected, e.g. -2 = broker unreachable,
  // 4/5 = rejected credentials
  String mqttStatus = netMqttOk ? String("OK") : !netWifiOk ? String("--") : String(netMqttState);
  String net = String("WiFi:") + (netWifiOk ? "OK" : "--") + " MQTT:" + mqttStatus;
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
  receiver.clearMail();
  digitalWrite(LED_PIN, LOW);
  statePending = true;
  drawScreen();
}

void handleRadio() {
  switch (receiver.poll()) {
    case MailboxReceiver::Event::NewMail:
      mailTime = millis();
      digitalWrite(LED_PIN, HIGH);
      statePending = true;                    // core 0 will publish this to MQTT
      wakeScreen();                           // new mail – show it
      break;

    case MailboxReceiver::Event::Duplicate:
    case MailboxReceiver::Event::BatteryReport:
      statePending = true;                    // signal / battery readings changed
      drawScreen();
      break;

    case MailboxReceiver::Event::Ignored:
    case MailboxReceiver::Event::None:
      break;
  }
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
  receiver.begin();
}

void loop() {
  handleRadio();

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
      if (receiver.state().mail) clearMail();       // PRG = "mail collected"
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
