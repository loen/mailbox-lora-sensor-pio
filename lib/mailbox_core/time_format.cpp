#include "time_format.h"

namespace mailbox {

std::string formatAgo(uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s < 60)   return std::to_string(s) + " s";
  if (s < 3600) return std::to_string(s / 60) + " min";
  return std::to_string(s / 3600) + " h " + std::to_string((s % 3600) / 60) + " min";
}

}  // namespace mailbox
