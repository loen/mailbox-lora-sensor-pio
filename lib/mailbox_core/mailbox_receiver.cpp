#include "mailbox_receiver.h"

#include <stdio.h>

#include "battery.h"
#include "protocol.h"

namespace mailbox {

MailboxReceiver::MailboxReceiver(Radio &radio, Clock &clock, Logger &logger, Mutex &stateMutex,
                                 ReceiverConfig config)
    : radio_(radio), clock_(clock), logger_(logger), stateMutex_(stateMutex), config_(config) {}

void MailboxReceiver::begin() {
  radio_.startReceive();
}

MailboxReceiver::Event MailboxReceiver::poll() {
  RxPacket rx;
  RxStatus status = radio_.poll(rx);
  if (status == RxStatus::None) return Event::None;

  if (status != RxStatus::Packet) {
    if (status == RxStatus::CrcError) logger_.log("!! Packet dropped: CRC error");
    radio_.startReceive();
    return Event::Ignored;
  }

  char line[160];
  snprintf(line, sizeof(line), "Received: %s | RSSI %d dBm | SNR %.1f dB", rx.text.c_str(),
           rx.rssi, (double) rx.snr);
  logger_.log(line);

  Message msg;
  if (!parseMessage(rx.text, msg)) {
    radio_.startReceive();                    // foreign packet – go straight back to listening
    return Event::Ignored;
  }
  sendAck(msg.id);                            // ACK immediately (radio returns to receive afterwards)

  stateMutex_.lock();
  bool isNew = tracker_.onMessage(msg, rx.rssi, rx.snr, clock_.nowMs());
  stateMutex_.unlock();

  if (msg.type == MessageType::Battery) {
    snprintf(line, sizeof(line), "Battery report: %u mV (%d %%)", (unsigned) msg.batteryMv,
             batteryPercent(msg.batteryMv));
    logger_.log(line);
    return Event::BatteryReport;
  }
  if (isNew) return Event::NewMail;
  logger_.log("Duplicate - already counted");
  return Event::Duplicate;
}

void MailboxReceiver::sendAck(uint32_t id) {
  clock_.sleepMs(config_.ackDelayMs);
  const std::string ack = encodeAck(id);
  bool ok = radio_.transmit(ack);             // blocks until transmission ends
  radio_.startReceive();                      // back to continuous receive
  logger_.log(ok ? "Sent " + ack : "!! ACK transmit failed");
}

void MailboxReceiver::clearMail() {
  stateMutex_.lock();
  tracker_.clearMail();
  stateMutex_.unlock();
}

MailState MailboxReceiver::state() {
  stateMutex_.lock();
  MailState copy = tracker_.state();
  stateMutex_.unlock();
  return copy;
}

}  // namespace mailbox
