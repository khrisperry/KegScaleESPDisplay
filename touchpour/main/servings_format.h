#pragma once

#include <cmath>
#include <limits>

// Count only complete servings, matching the Scale website's Math.floor().
// The controller intentionally sends fractional servings over Wi-Fi.
inline unsigned touchpour_complete_servings(float servings) {
  if (!std::isfinite(servings) || servings <= 0.0f)
    return 0U;
  constexpr unsigned max_servings = std::numeric_limits<unsigned>::max();
  if (servings >= static_cast<float>(max_servings))
    return max_servings;
  return static_cast<unsigned>(std::floor(servings));
}
