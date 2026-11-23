# Protocol

LoRa in point-to-point mode gives no delivery guarantee: a packet is either received or
silently lost. This document describes the small protocol built on top of it to make the
mailbox link reliable. The radio settings are described in [radio.md](radio.md).

The code is in [`protocol.cpp`](../lib/mailbox_core/protocol.cpp) (packet format),
[`mailbox_sender.cpp`](../lib/mailbox_core/mailbox_sender.cpp),
[`mailbox_receiver.cpp`](../lib/mailbox_core/mailbox_receiver.cpp),
[`mail_tracker.h`](../lib/mailbox_core/mail_tracker.h) (duplicate filter) and
[`sender_schedule.cpp`](../lib/mailbox_core/sender_schedule.cpp) (wake-up and heartbeat
schedule).

## Packet format

Packets are plain ASCII text with `;` as the field separator:

| Direction | Packet | Meaning |
|---|---|---|
| sender → receiver | `MAILBOX_OPEN;<id>;<mV>` | The flap was opened. |
| sender → receiver | `BATTERY;<id>;<mV>` | Heartbeat that carries only the battery voltage. |
| receiver → sender | `ACK;<id>` | The message with this id was received. |

- `<id>` is the message's sequence number (32-bit), and `<mV>` is the sender's battery voltage
  in millivolts (16-bit). Every message carries the voltage, so a flap event doubles as a
  battery report.
- Numbers are 1–10 decimal digits, with no sign and no whitespace.
- The parser is strict: anything that is not a well-formed message, such as LoRaWAN or other
  foreign traffic on the same channel, is dropped.
- Fields after the battery voltage are ignored, so new fields can be appended later without
  breaking an older receiver. An ACK has to match exactly.

Text was chosen over a binary format because the packets are short either way and can be read
directly in the serial log. The cost is a few extra bytes of time on air.

## Message exchange

```mermaid
sequenceDiagram
    participant S as Sender (mailbox)
    participant R as Receiver (house)
    Note over S: reed switch wakes the ESP32
    S->>R: MAILBOX_OPEN;7;4012  (~2.0 s on air)
    Note over R: wait 100 ms for the sender to switch to RX
    R->>S: ACK;7  (~1.2 s on air)
    Note over S: ACK id matches, so go back to deep sleep
```

When the data packet or the ACK is lost:

```mermaid
sequenceDiagram
    participant S as Sender (mailbox)
    participant R as Receiver (house)
    S-xR: MAILBOX_OPEN;7;4012  (lost)
    Note over S: no ACK within 4 s<br/>random back-off 0.5–2 s
    S->>R: MAILBOX_OPEN;7;4012  (attempt 2)
    Note over R: new id, so count the mail
    R--xS: ACK;7  (lost)
    Note over S: no ACK within 4 s<br/>random back-off 0.5–2 s
    S->>R: MAILBOX_OPEN;7;4012  (attempt 3)
    Note over R: same id within 60 s,<br/>so it is a duplicate: ACK it, don't count it
    R->>S: ACK;7
```

### Sender

- **Sequence numbers.** The id grows by one with every message. It is kept in RTC memory, so
  it survives deep sleep but starts again from 0 after a reset or a battery swap.
- **ACK wait.** After transmitting, the sender listens continuously for 4 s. The window fits
  the receiver's 100 ms delay plus a 1.2 s ACK with a wide margin. Only an ACK with the id just
  sent ends the wait. Foreign packets, corrupted packets and stale ACKs for earlier messages
  are ignored, and the sender keeps listening.
- **Retries.** Without an ACK the sender waits a random 0.5–2 s and transmits again, up to 3
  attempts. The random delay makes it less likely that a retransmission hits the same
  interference again.
- **Giving up.** After 3 failed attempts the message is dropped and the sender goes back to
  sleep. A lost flap event is not resent later, because the next opening is reported anyway.

### Receiver

- **Continuous receive.** The receiver is mains powered and always listening, except while it
  is sending an ACK. The radio is half-duplex, so a packet that arrives during those ~1.3 s is
  lost. The sender then simply retries.
- **ACK first.** A valid message is acknowledged immediately, before it is processed.
  Messages are acknowledged even when they are duplicates, so the sender stops retrying.
- **Duplicate filter.** If the data packet got through but the ACK was lost, the sender
  retransmits a message that was already counted. The receiver remembers the id of the last
  counted `MAILBOX_OPEN`, and the same id within 60 s is treated as a retransmission. The same
  id later than that is a new event, because the sender was reset and its ids started again.

## Sender schedule

| Wake-up cause | Action |
|---|---|
| Reed switch (flap opened) | Send `MAILBOX_OPEN`. |
| Timer, heartbeat due | Send `BATTERY`. |
| Timer, flap still open | Only check whether the flap has closed. |
| Power-on or reset | Wait 10 s (time to upload firmware or open the serial monitor), then send `BATTERY`. |

- **Heartbeat.** A battery report is due 24 h after the last acknowledged message. If a
  `BATTERY` report is not acknowledged, the next one comes after 1 h instead. A lost flap event
  leaves the heartbeat schedule unchanged.
- **Flap left open.** While the flap is open, the reed switch stays closed, and waking on it
  would wake the board over and over. The sender wakes on a 30 s timer instead until the flap
  closes, and sends nothing meanwhile.

## Known limitations

- **Reset right after an opening.** If the sender is reset and the flap is opened again within
  60 s of the last counted opening, the new message reuses the same id. The receiver takes it
  for a retransmission, so that opening is not counted. It is still acknowledged.
- **No recovery of a lost event.** A flap event that fails all three attempts is gone. The
  heartbeat shows that the link is down, but not that mail arrived meanwhile.

Both cases are covered by integration tests that document the current behaviour.
