#include "protocol.h"

namespace mailbox {

namespace {

const char kTypeMailboxOpen[] = "MAILBOX_OPEN";
const char kTypeBattery[] = "BATTERY";
const char kAckPrefix[] = "ACK;";

// Decimal digits only (no sign, no spaces), value must fit in `max`.
bool parseUnsigned(const std::string &s, uint32_t max, uint32_t &out) {
  if (s.empty() || s.size() > 10) return false;
  uint64_t value = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + (uint64_t) (c - '0');
  }
  if (value > max) return false;
  out = (uint32_t) value;
  return true;
}

// Returns the field starting at `pos` and moves `pos` past the following ';'
// (or to npos when this was the last field).
std::string nextField(const std::string &text, size_t &pos) {
  size_t end = text.find(';', pos);
  std::string field = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
  pos = (end == std::string::npos) ? std::string::npos : end + 1;
  return field;
}

}  // namespace

std::string encodeMessage(const Message &msg) {
  const char *type = (msg.type == MessageType::MailboxOpen) ? kTypeMailboxOpen : kTypeBattery;
  return std::string(type) + ";" + std::to_string(msg.id) + ";" + std::to_string(msg.batteryMv);
}

bool parseMessage(const std::string &text, Message &out) {
  size_t pos = 0;
  std::string type = nextField(text, pos);
  if (pos == std::string::npos) return false;            // no id field

  Message msg;
  if (type == kTypeMailboxOpen) msg.type = MessageType::MailboxOpen;
  else if (type == kTypeBattery) msg.type = MessageType::Battery;
  else return false;

  if (!parseUnsigned(nextField(text, pos), UINT32_MAX, msg.id)) return false;

  if (pos != std::string::npos) {                        // optional battery voltage
    uint32_t mv = 0;
    if (!parseUnsigned(nextField(text, pos), UINT16_MAX, mv)) return false;
    msg.batteryMv = (uint16_t) mv;
  }

  out = msg;
  return true;
}

std::string encodeAck(uint32_t id) {
  return std::string(kAckPrefix) + std::to_string(id);
}

bool parseAck(const std::string &text, uint32_t &id) {
  const size_t prefixLen = sizeof(kAckPrefix) - 1;
  if (text.compare(0, prefixLen, kAckPrefix) != 0) return false;
  return parseUnsigned(text.substr(prefixLen), UINT32_MAX, id);
}

}  // namespace mailbox
