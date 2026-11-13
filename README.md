# mailbox-lora-sensor-pio

LoRa mailbox notifier for two Heltec WiFi LoRa 32 V2 boards, built with PlatformIO.

- `src/sender` – board in the mailbox: a reed switch wakes it from deep sleep, it sends
  `MAILBOX_OPEN` / `BATTERY` over LoRa and waits for an ACK.
- `src/receiver` – indoor board: receives the packets, shows the state on the OLED and
  publishes it to Home Assistant over MQTT.

- `lib/mailbox_core` – all the logic, free of Arduino dependencies: packet format, LoRa
  settings, battery curve, and the sender (`MailboxSender`: retries, ACK wait, wake-up /
  sleep schedule) and receiver (`MailboxReceiver`: ACKs, mail state) themselves. They reach
  the hardware only through the interfaces in `mailbox_hal.h`.
- `lib/mailbox_arduino` – those interfaces implemented with RadioLib and Arduino.
- `lib/heltec_v2` – board pin assignment.
- `test/native` – tests run on the computer: unit tests of `lib/mailbox_core`, and
  integration tests (`test_integration_*`) where the sender and the receiver talk to each
  other through a simulated radio with a virtual clock (`test/native/support`).

## Setup

Copy `include/arduino_secrets.h.example` to `include/arduino_secrets.h` and fill in the
Wi-Fi and MQTT credentials (used by the receiver only).

## Build and upload

```
pio test -e native               # unit + integration tests (no board needed)
pio run                          # build both firmwares
pio run -e receiver -t upload    # flash the receiver
pio run -e sender -t upload      # flash the sender
pio device monitor
```

Always pass `-e` when uploading – without it PlatformIO flashes both firmwares to the same board.
