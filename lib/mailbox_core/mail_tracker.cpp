#include "mail_tracker.h"

namespace mailbox {

bool MailTracker::onMessage(const Message &msg, int rssi, float snr, uint32_t nowMs) {
  state_.rssi = rssi;
  state_.snr = snr;
  state_.hasPacket = true;
  if (msg.batteryMv > 0) state_.batteryMv = msg.batteryMv;

  if (msg.type != MessageType::MailboxOpen) return false;
  // unsigned subtraction stays correct when the clock wraps around
  if (hasLastId_ && msg.id == lastId_ && (uint32_t) (nowMs - lastIdMs_) < kDuplicateWindowMs) {
    return false;                               // retransmission – already counted
  }

  hasLastId_ = true;
  lastId_ = msg.id;
  lastIdMs_ = nowMs;
  state_.mail = true;
  state_.count++;
  return true;
}

void MailTracker::clearMail() {
  state_.mail = false;
}

}  // namespace mailbox
