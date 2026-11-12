#pragma once

#include <stdint.h>
#include <string>

namespace mailbox {

// Elapsed time for the display: "42 s", "5 min", "2 h 7 min".
std::string formatAgo(uint32_t ms);

}  // namespace mailbox
