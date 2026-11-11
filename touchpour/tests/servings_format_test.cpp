#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>

#include "../main/servings_format.h"

int main() {
  // 3.07 gallons / 16 oz is approximately 24.56 servings, NOT 25 full beers.
  assert(touchpour_complete_servings(3.07f * 128.0f / 16.0f) == 24U);
  assert(touchpour_complete_servings(24.0f) == 24U);
  assert(touchpour_complete_servings(24.56f) == 24U);
  assert(touchpour_complete_servings(24.999f) == 24U);
  assert(touchpour_complete_servings(25.0f) == 25U);
  assert(touchpour_complete_servings(25.99f) == 25U);
  assert(touchpour_complete_servings(40.0f) == 40U);
  assert(touchpour_complete_servings(0.0f) == 0U);
  assert(touchpour_complete_servings(-0.01f) == 0U);
  assert(touchpour_complete_servings(std::numeric_limits<float>::quiet_NaN()) == 0U);
  assert(touchpour_complete_servings(std::numeric_limits<float>::infinity()) == 0U);
  assert(touchpour_complete_servings(std::numeric_limits<float>::max()) ==
         std::numeric_limits<unsigned>::max());

  puts("PASS: TouchPour complete servings matches Scale website floor behavior");
}
