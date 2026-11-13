#include "mailbox_sender.h"

#include <stdio.h>

namespace mailbox {

MailboxSender::MailboxSender(Radio &radio, Clock &clock, RandomSource &random, Logger &logger,
                             SenderState &state, SenderConfig config)
    : radio_(radio), clock_(clock), random_(random), logger_(logger), state_(state),
      config_(config) {}

void MailboxSender::onWake(WakeCause cause, uint16_t batteryMv) {
  switch (decideWakeAction(cause, clock_.rtcSeconds(), state_.nextBatteryReportS)) {
    case WakeAction::SendMailboxOpen:
      logger_.log("Wake-up: flap opened");
      send(MessageType::MailboxOpen, batteryMv);
      break;

    case WakeAction::SendBattery:
      logger_.log("Wake-up: battery report");
      send(MessageType::Battery, batteryMv);
      break;

    case WakeAction::CheckFlapOnly:
      logger_.log("Wake-up: checking whether the flap has closed");
      break;

    case WakeAction::ServiceWindowThenBattery:
      logger_.log("Power-on / reset - service window");
      clock_.sleepMs(config_.serviceWindowMs);
      send(MessageType::Battery, batteryMv);
      break;
  }
}

bool MailboxSender::send(MessageType type, uint16_t batteryMv) {
  Message m;
  m.type = type;
  m.id = state_.msgId;
  m.batteryMv = batteryMv;
  const std::string text = encodeMessage(m);

  char line[96];
  bool acked = false;
  for (int attempt = 1; attempt <= config_.maxAttempts; attempt++) {
    if (radio_.transmit(text)) {              // blocks until transmission ends (~1.3 s)
      snprintf(line, sizeof(line), "Attempt %d/%d: sent %s", attempt, config_.maxAttempts,
               text.c_str());
    } else {
      snprintf(line, sizeof(line), "Attempt %d/%d: transmit failed", attempt,
               config_.maxAttempts);
    }
    logger_.log(line);

    if (waitForAck(m.id)) {
      logger_.log("ACK received");
      acked = true;
      break;
    }
    logger_.log("No ACK");
    if (attempt < config_.maxAttempts) {      // random back-off before retry
      clock_.sleepMs(random_.between(config_.backoffMinMs, config_.backoffMaxMs));
    }
  }
  if (!acked) logger_.log("Message was not acknowledged");

  state_.msgId++;
  state_.nextBatteryReportS =
      nextReportAfterSend(type, acked, clock_.rtcSeconds(), state_.nextBatteryReportS);
  return acked;
}

bool MailboxSender::waitForAck(uint32_t id) {
  radio_.startReceive();                      // continuous receive – no gaps
  const uint32_t t0 = clock_.nowMs();
  while ((uint32_t) (clock_.nowMs() - t0) < config_.ackTimeoutMs) {
    RxPacket rx;
    RxStatus status = radio_.poll(rx);
    if (status == RxStatus::Packet) {
      char line[160];
      snprintf(line, sizeof(line), "Received: %s (RSSI %d, SNR %.1f)", rx.text.c_str(), rx.rssi,
               (double) rx.snr);
      logger_.log(line);
      uint32_t ackId;
      if (parseAck(rx.text, ackId) && ackId == id) return true;
    } else if (status == RxStatus::CrcError) {
      logger_.log("!! Packet dropped: CRC error");
    }
    if (status != RxStatus::None) radio_.startReceive();   // foreign or corrupted – keep listening
    clock_.sleepMs(config_.ackPollMs);
  }
  return false;
}

SleepPlan MailboxSender::planSleep(bool flapStillOpen) {
  return mailbox::planSleep(flapStillOpen, clock_.rtcSeconds(), state_.nextBatteryReportS);
}

}  // namespace mailbox
