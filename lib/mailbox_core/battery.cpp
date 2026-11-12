#include "battery.h"

#include <stdint.h>

namespace mailbox {

int batteryPercent(int mv) {
  static const int curve[][2] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3900, 65}, {3800, 45},
    {3750, 30},  {3700, 20}, {3650, 12}, {3600, 7},  {3500, 3}, {3300, 0}
  };
  const int n = sizeof(curve) / sizeof(curve[0]);
  if (mv >= curve[0][0]) return 100;
  for (int i = 1; i < n; i++) {
    if (mv >= curve[i][0]) {                    // linear interpolation between two points
      return curve[i][1] + (int32_t) (mv - curve[i][0]) * (curve[i - 1][1] - curve[i][1]) /
                           (curve[i - 1][0] - curve[i][0]);
    }
  }
  return 0;
}

}  // namespace mailbox
