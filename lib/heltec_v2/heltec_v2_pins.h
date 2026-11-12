// Pin assignment of the Heltec WiFi LoRa 32 V2 board
#pragma once

// ---------- SX1276 ----------
#define LORA_SCK   5
#define LORA_MISO  19
#define LORA_MOSI  27
#define LORA_NSS   18
#define LORA_RST   14
#define LORA_DIO0  26      // "packet received" / "transmit done"
#define LORA_DIO1  35

// ---------- OLED (SSD1306, I2C) ----------
#define OLED_SDA   4
#define OLED_SCL   15
#define OLED_RST   16

// ---------- Other ----------
#define LED_PIN    25
#define BUTTON_PIN 0       // PRG button
#define VEXT_PIN   21      // external power rail: OLED and battery divider (LOW = on)
