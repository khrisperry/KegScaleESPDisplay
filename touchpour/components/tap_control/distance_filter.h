#pragma once
#include <algorithm>
#include <cstdint>

// Median rejects one-sample spikes; retain fractional EMA precision.
class DistanceFilter {
  int window[3]{};
  unsigned count = 0, next = 0;
  float value = 0;
  uint32_t last_at = 0;
public:
  void reset() { count = next = 0; value = 0; }
  float update(int raw, bool valid, uint32_t at, uint32_t stale_ms) {
    if (!valid) { reset(); return 0; }
    if (count && at - last_at > stale_ms) reset();
    last_at = at;
    window[next] = raw;
    next = (next + 1) % 3;
    count = std::min(count + 1, 3u);
    int sorted[3];
    std::copy(window, window + count, sorted);
    std::sort(sorted, sorted + count);
    float median = count == 2 ? (sorted[0] + sorted[1]) * 0.5f
                              : sorted[count / 2];
    value = count == 1 ? median : value + 0.4f * (median - value);
    return value;
  }
};
