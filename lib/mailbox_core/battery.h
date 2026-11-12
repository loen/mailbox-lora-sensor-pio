#pragma once

namespace mailbox {

// Resting voltage of a single Li-ion / LiPo cell (mV) -> state of charge (0–100 %).
// 0 % is set at 3.3 V: below that the board's 3.3 V regulator drops out anyway.
int batteryPercent(int mv);

}  // namespace mailbox
