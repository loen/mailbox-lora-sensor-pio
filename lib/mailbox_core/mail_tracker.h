// Receiver side: turns incoming messages into the "is there mail?" state.
#pragma once

#include <stdint.h>

#include "protocol.h"

namespace mailbox {

// The sender retransmits an unacknowledged message for at most ~15 s (3 attempts). The same
// id seen later than this is a new event: the sender restarted and its ids started over.
constexpr uint32_t kDuplicateWindowMs = 60000;

struct MailState {
  bool mail = false;
  uint32_t count = 0;           // how many times the flap was opened
  int rssi = 0;                 // signal of the last packet
  float snr = 0;
  bool hasPacket = false;       // false until the first packet arrives
  int batteryMv = 0;            // sender battery, 0 = not reported yet
};

class MailTracker {
 public:
  // Records a message received at `nowMs` (a millis()-style clock that may wrap around).
  // Returns true when it is a new flap opening; a retransmission of the last MAILBOX_OPEN
  // (the sender missed our ACK) within kDuplicateWindowMs is not.
  bool onMessage(const Message &msg, int rssi, float snr, uint32_t nowMs);

  // "Mail collected" – keeps the counter and the last signal / battery readings.
  void clearMail();

  const MailState &state() const { return state_; }

 private:
  MailState state_;
  bool hasLastId_ = false;
  uint32_t lastId_ = 0;         // id of the last counted MAILBOX_OPEN
  uint32_t lastIdMs_ = 0;       // when it was counted
};

}  // namespace mailbox
