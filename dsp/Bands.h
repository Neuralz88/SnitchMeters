#pragma once
#include <cmath>
#include "Profile.h"

namespace snitch {

// Third-octave centre: band 17 = 1 kHz, band 0 = 20 Hz, band 30 = 20 kHz.
inline float bandCentre(int i) { return 1000.0f * std::pow(2.0f, (float) (i - 17) / 3.0f); }
inline float bandLowEdge(int i)  { return bandCentre(i) * 0.8908987f; }  // 2^(-1/6)
inline float bandHighEdge(int i) { return bandCentre(i) * 1.1224620f; }  // 2^(+1/6)

inline const char* regionName(int r) {
    static const char* names[kRegions] = { "Sub", "Bass", "Low-mid", "Mid", "Presence", "Air" };
    return names[r];
}

inline void regionRange(int r, float& lo, float& hi) {
    static const float edges[kRegions + 1] = { 20.0f, 60.0f, 150.0f, 400.0f, 2000.0f, 6000.0f, 20000.0f };
    lo = edges[r]; hi = edges[r + 1];
}

inline int regionOfBand(int i) {
    const float f = bandCentre(i);
    if (f < 56.0f)   return 0;
    if (f < 141.0f)  return 1;
    if (f < 355.0f)  return 2;
    if (f < 1780.0f) return 3;
    if (f < 5600.0f) return 4;
    return 5;
}

// Bands that count towards a region verdict. The outermost ones are left out:
// 20 Hz is mostly rumble/FFT skirt and 16-20 kHz is where lossy streams cut off.
inline bool bandIsJudged(int i) { return i >= 1 && i <= 28; }

} // namespace snitch
