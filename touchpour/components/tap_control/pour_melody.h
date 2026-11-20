#pragma once

// Melody data: monophonic adaptation/transcription of "99 Bottles of Beer",
// sheet music by Theleekycauldron (2021), Wikimedia Commons:
// https://commons.wikimedia.org/wiki/File:99_bottles_of_beer-1.png
// This melody-data file is licensed CC BY-SA 4.0:
// https://creativecommons.org/licenses/by-sa/4.0/
// Changes: integer pitch frequencies, 300 ms quarter-note tempo, no lyrics
// or accompaniment, and a short rest at the end for looping.
namespace tap {
struct AudioNote { int hz, ms; };
inline constexpr AudioNote pour_melody[] = {
  // G major, one complete verse (instrumental).
  {392,300},{392,300},{392,300}, {294,300},{294,300},{294,300},
  {392,300},{392,300},{392,300},{392,900},
  {440,300},{440,300},{440,300}, {330,300},{330,300},{330,300},{440,1800},
  {370,600},{370,300},{370,900}, {370,300},{370,300},{370,300},{370,900},
  {294,300},{294,300},{294,300}, {294,300},{330,300},{370,300},
  {392,300},{392,300},{392,300},{392,900},{0,120}
};
inline constexpr int pour_melody_count=sizeof(pour_melody)/sizeof(pour_melody[0]);
} // namespace tap
