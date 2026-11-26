# The radio link

This document explains why the project uses LoRa, how the modem is configured and how the link
was made reliable. The packet format and the ACK/retry exchange are described in
[protocol.md](protocol.md).

## Why LoRa

The mailbox is only about 20 m from the flat, but the distance is not the problem. The
obstacles on the path are, and they rule out the usual home IoT radios:

- **No line of sight.** The mailbox is around the corner of the building, so there is no
  direct path. The signal reaches the receiver only by diffraction around the corner and by
  reflections, and both lose a lot of power.
- **An enclosed vestibule.** The mailbox stands in the building's vestibule, so the signal
  also has to pass through its walls and door.
- **The antenna has to be inside a metal box.** The mailbox is made of metal (most likely
  aluminium), and nothing can stick out of it. A metal enclosure behaves like a leaky Faraday
  cage: RF escapes mainly through the slot and gaps around the flap. The nearby metal walls
  also detune the antenna and change its radiation pattern.

Each of these losses would be manageable alone. Added together, they push 2.4 GHz Wi-Fi and BLE
past their link margin even at 20 m.

LoRa addresses all three:

- **Sub-GHz band.** The link runs at 868 MHz. Free-space path loss there is about 9 dB lower
  than at 2.4 GHz (20·log₁₀(2440/868)), and longer wavelengths pass through walls with less
  loss.
- **Chirp spread spectrum (CSS).** Each symbol is a frequency sweep across the whole channel
  bandwidth. The receiver correlates it against a reference chirp, which concentrates the
  symbol's energy and spreads narrowband interference. This lets the SX1276 demodulate packets
  that arrive **below the noise floor**: down to about −20 dB SNR at SF12.
- **Configurable trade-off.** The spreading factor (SF7–SF12), bandwidth and coding rate trade
  data rate for sensitivity. A mailbox sensor sends a few dozen bytes a few times a day, so it
  can spend almost all of that trade-off on robustness.
- **Low power.** The radio only draws current while sending a packet and waiting for its ACK.
  The rest of the time both the radio and the ESP32 sleep.

## Link configuration

All values are in [`lib/mailbox_core/radio_config.h`](../lib/mailbox_core/radio_config.h) and
are shared by both firmwares.

| Parameter | Value | Why |
|---|---|---|
| Frequency | 869.525 MHz | EU sub-band 869.40–869.65 MHz (ERC Rec. 70-03, band h1.4). It allows up to 500 mW ERP at a 10 % duty cycle, compared with 25 mW in most of the 868 MHz band. |
| TX power | 20 dBm | The maximum of the SX1276 PA_BOOST output, and within the sub-band limit. |
| Spreading factor | 12 | The highest sensitivity the SX1276 offers: about −136 dBm, compared with about −123 dBm at SF7. |
| Bandwidth | 125 kHz | The standard LoRa channel width, and a good balance between sensitivity and crystal tolerance. |
| Coding rate | 4/8 | The strongest forward error correction: every 4 data bits are sent as 8. |
| Low Data Rate Optimize | on | RadioLib enables it automatically for SF12 at 125 kHz, where a symbol lasts 32.8 ms and clock drift during a packet becomes significant. |
| CRC | on | Corrupted packets are dropped by the radio and never reach the protocol layer. |
| Sync word | 0x34 | The value the original Heltec library uses. It is also the LoRaWAN public sync word, so LoRaWAN traffic on the same channel reaches the receiver too. The strict packet parser drops it. |

### Link budget and time on air

Rough link budget: 20 dBm TX power and −136 dBm sensitivity give about **156 dB** of maximum
path loss, before antenna gains and the enclosure loss.

The cost is time on air:

| Packet | Bytes | SF12, CR 4/8 | SF7, CR 4/5 |
|---|---|---|---|
| `MAILBOX_OPEN;123;4100` | 21 | about 2.0 s | about 60 ms |
| `BATTERY;123;4100` | 16 | about 1.7 s | |
| `ACK;123` | 7 | about 1.2 s | |

Even the worst case (three attempts plus back-off, about 22 s) uses a small fraction of the
360 s per hour that the 10 % duty cycle allows.

### Measured link margin

The receiver publishes the RSSI and SNR of every packet to Home Assistant. A reading from the
closed mailbox, with the final antenna and modem settings, is **RSSI −113 dBm** and
**SNR +1.3 dB**.

| | Measured | Limit at SF12, BW 125 kHz | Margin |
|---|---|---|---|
| Signal strength | −113 dBm | about −136 dBm sensitivity | about 23 dB |
| Signal-to-noise ratio | +1.3 dB | about −20 dB demodulation floor | about 21 dB |

The SNR margin is the more meaningful of the two, because it accounts for the actual noise
at the receiver and not just the datasheet sensitivity. About 21 dB of margin means the link
would survive more than a hundredfold drop in received power, for example from rain, a wet
door, or a parked car, before packets start to fail. This is a single reading; the published
values let the margin be tracked over time.

For comparison, at SF7 the demodulation floor is about −7.5 dB SNR, which would leave only
about 9 dB of margin on the same path.

## How the link was improved

The first version used the boards out of the box: the stock antenna and default modem settings
with fast spreading factor and light error correction. Packets from inside the closed metal
mailbox arrived unreliably. Three changes made the link dependable, each working on a different
layer.

### 1. Antenna (physical layer)

The small antenna supplied with the board was replaced with a custom antenna, chosen and placed
for the 868 MHz band and the inside of the aluminium mailbox. Measured at the receiver, the
change **raised RSSI by 6 dB and SNR by 2 dB**. That means four times more received power, with
no extra transmit power or battery drain.

Antenna gain and matching act on both directions of the link at once, so the same gain also
improves the sensor's reception of the ACK. No amount of protocol work can recover a signal
that never reaches the receiver.

### 2. Slower, more robust modulation (modem layer)

The modem was moved to SF12 with coding rate 4/8:

- Each step up in spreading factor doubles the symbol duration and adds about 2.5 dB of
  sensitivity. Going from SF7 to SF12 gains about 13 dB, which in free space is more than four
  times the range.
- Coding rate 4/8 doubles the redundancy of the payload, so the receiver can correct bit errors
  caused by fading and interference instead of dropping the packet on a CRC failure.
- The price is a roughly 30× longer time on air. That is acceptable for a sensor that sends a
  few short messages a day, and the 869.525 MHz sub-band with its generous duty cycle leaves
  plenty of headroom.

### 3. Application-level ACK and retries (link layer)

LoRa in point-to-point mode has no delivery guarantee: a packet is either received or silently
lost. A reliable exchange was built on top of it:

- every message carries a sequence number and must be acknowledged with that number,
- an unacknowledged message is retransmitted after a random back-off, up to 3 attempts,
- the receiver suppresses duplicates, so a lost ACK does not count the mail twice,
- a daily heartbeat, retried hourly when it fails, detects a dead battery or a broken link,
- the RSSI and SNR of every packet are published to Home Assistant, so the link margin can be
  monitored over time and across seasons.

The details and timing are in [protocol.md](protocol.md).

## Verification

The protocol is tested without hardware. In `test/native/test_integration_*`, a sender and a
receiver exchange messages over a simulated channel with a virtual clock. The scenarios include
lost packets, lost ACKs, CRC errors, foreign packets, duplicates and sender resets. The tests
run with AddressSanitizer and UBSan on every push, in GitHub Actions.
