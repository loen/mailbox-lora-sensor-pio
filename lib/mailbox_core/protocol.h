// Wire format of the packets exchanged between the sender and the receiver.
//
//   sender   -> receiver:  "<type>;<id>[;<battery mV>]"   type = MAILBOX_OPEN or BATTERY
//   receiver -> sender:    "ACK;<id>"
//
// Numbers are 1–10 decimal digits (leading zeros allowed, no sign, no whitespace); the id must
// fit in 32 bits and the voltage in 16. Fields after the battery voltage are ignored, so new
// ones can be appended to a message later. An ACK is matched exactly – nothing may follow the id.
#pragma once

#include <stdint.h>
#include <string>

namespace mailbox {

enum class MessageType {
  MailboxOpen,   // the flap was opened
  Battery,       // heartbeat that only carries the battery voltage
};

struct Message {
  MessageType type = MessageType::MailboxOpen;
  uint32_t id = 0;
  uint16_t batteryMv = 0;   // 0 = not reported
};

std::string encodeMessage(const Message &msg);

// Returns false for anything that is not a well-formed message (e.g. a foreign packet).
bool parseMessage(const std::string &text, Message &out);

std::string encodeAck(uint32_t id);
bool parseAck(const std::string &text, uint32_t &id);

}  // namespace mailbox
