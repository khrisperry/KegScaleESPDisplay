#pragma once
#include "vessel_artwork.h"
#include <cmath>

namespace serving_vessel {
enum class Kind { Pint, Solo, Can };
inline Kind for_serving(float ounces) {
  if (std::fabs(ounces - 12.0f) <= 0.25f) return Kind::Can;
  if (std::fabs(ounces - 20.0f) <= 0.25f) return Kind::Solo;
  // No crowler/growler artwork on the tap handle.
  return Kind::Pint;
}
inline vessel_artwork::Kind artwork_kind(Kind kind) {
  return kind == Kind::Can ? vessel_artwork::Kind::Can :
         kind == Kind::Solo ? vessel_artwork::Kind::Cup : vessel_artwork::Kind::Pint;
}
} // namespace serving_vessel
