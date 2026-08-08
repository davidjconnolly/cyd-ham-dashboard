#pragma once

// Working out a spot's mode from its frequency.
//
// Nothing in the DX cluster world carries the mode as data. The Telnet `DX de`
// line has no mode field, and neither the DXSummit nor the IZ3MEZ JSON feed has
// one — both give a frequency and a free-text comment and nothing else. So the
// mode is either stated by the spotter in that comment or it has to be
// inferred, and most spotters state nothing.
//
// Frequency infers it well, because the band plans put CW, data and phone in
// separate parts of every band. Two steps:
//
//   1. The digital calling frequencies, matched within kToleranceMhz. These are
//      the only places a specific digital mode can be named with confidence.
//   2. The CW and phone segments. Broad, and enough to classify the great
//      majority of spots that say nothing.
//
// The segments below are deliberately only the slices where IARU Region 1 and
// Region 2 agree. They differ most on 40 m — 7.060-7.125 is phone in Region 1
// and data in the US — so that slice, and others like it, are simply left out
// and such a spot stays unknown. Measured against 500 real spots, declining
// those cost 3 classifications out of 203 and bought 99.5% accuracy on the ones
// it does commit to (see tools/test_dx_mode_plan.cpp, which re-checks this
// against a captured feed in CI).
//
// The digital segment is intentionally NOT classified wholesale. FT8, FT4,
// PSK31, RTTY and JS8 share it, so calling all of it FT8 would be a guess
// dressed as a fact; only the calling frequencies below are claimed.
//
// Deliberately free of Arduino types so the host test compiles this same
// header rather than a copy of it.

#include <stddef.h>

namespace dxmode {

// Spotters round, and a signal sits up to 3 kHz above the dial frequency.
// Measured across real labelled spots: 2 kHz catches 100 of 105 FT8 spots,
// 5 kHz catches 104 and adds no cross-mode confusion.
constexpr double kToleranceMhz = 0.005;

struct CallingFrequency {
  double mhz;
  const char* mode;
};

struct Segment {
  double lowMhz;
  double highMhz;
  const char* mode;
};

// Returns the mode name, or nullptr when the frequency says nothing reliable.
// Callers pass MHz.
inline const char* forFrequency(double mhz) {
  static const CallingFrequency kCalling[] = {
      {1.840, "FT8"},   {3.573, "FT8"},   {5.357, "FT8"},   {7.074, "FT8"},
      {10.136, "FT8"},  {14.074, "FT8"},  {18.100, "FT8"},  {21.074, "FT8"},
      {24.915, "FT8"},  {28.074, "FT8"},  {50.313, "FT8"},  {70.154, "FT8"},
      {144.174, "FT8"},
      // 21.140 and 28.180, not the 21.080/28.080 this firmware used to carry:
      // no real FT4 activity was found at those, and they sit in the RTTY/PSK
      // part of the band where they caused false positives.
      {3.575, "FT4"},   {7.0475, "FT4"},  {10.140, "FT4"},  {14.080, "FT4"},
      {18.104, "FT4"},  {21.140, "FT4"},  {24.919, "FT4"},  {28.180, "FT4"},
      {50.318, "FT4"},
  };
  static const Segment kSegments[] = {
      {1.810, 1.836, "CW"},     {1.845, 2.000, "SSB"},
      {3.500, 3.570, "CW"},     {3.600, 4.000, "SSB"},
      {7.000, 7.040, "CW"},     {7.125, 7.300, "SSB"},
      {10.100, 10.130, "CW"},   // 30 m has no phone allocation anywhere
      {14.000, 14.070, "CW"},   {14.101, 14.350, "SSB"},
      {18.068, 18.095, "CW"},   {18.111, 18.168, "SSB"},
      {21.000, 21.070, "CW"},   {21.151, 21.450, "SSB"},
      {24.890, 24.910, "CW"},   {24.931, 24.990, "SSB"},
      {28.000, 28.070, "CW"},   {28.300, 29.700, "SSB"},
      {50.000, 50.100, "CW"},   {50.100, 50.300, "SSB"},
      {144.000, 144.160, "CW"}, {144.200, 144.275, "SSB"},
  };

  // The NEAREST calling frequency, not the first one within tolerance. On 17 m
  // and 12 m the FT8 and FT4 frequencies are only 4 kHz apart — closer than the
  // tolerance — so first-match ordering reported every 18.104 and 24.919 spot,
  // both of them FT4 calling frequencies, as FT8.
  const char* best = nullptr;
  double bestDelta = 0.0;
  for (size_t i = 0; i < sizeof(kCalling) / sizeof(kCalling[0]); ++i) {
    const double delta = mhz - kCalling[i].mhz;
    const double distance = delta < 0 ? -delta : delta;
    if (distance <= kToleranceMhz && (best == nullptr || distance < bestDelta)) {
      best = kCalling[i].mode;
      bestDelta = distance;
    }
  }
  if (best != nullptr) {
    return best;
  }
  for (size_t i = 0; i < sizeof(kSegments) / sizeof(kSegments[0]); ++i) {
    if (mhz >= kSegments[i].lowMhz && mhz < kSegments[i].highMhz) {
      return kSegments[i].mode;
    }
  }
  return nullptr;
}

}  // namespace dxmode
