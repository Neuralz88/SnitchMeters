#pragma once
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "Bands.h"

namespace snitch {

// How far the live spectrum sits above the reference overall (median of the
// per-band differences), so a hot sub does not make every other band read thin.
inline float matchOffset(const float* live, const float* ref) {
    float d[kBands]; int n = 0;
    for (int i = 0; i < kBands; ++i)
        if (bandIsJudged(i)) d[n++] = live[i] - ref[i];
    std::sort(d, d + n);
    return (n & 1) ? d[n / 2] : 0.5f * (d[n / 2 - 1] + d[n / 2]);
}

// Level-matched difference to the reference per region, in dB (+ = more than the reference).
inline void regionDeltas(const float* live, const float* ref, float out[kRegions]) {
    const float offset = matchOffset(live, ref);
    float sum[kRegions] = {}; int count[kRegions] = {};
    for (int i = 0; i < kBands; ++i) {
        if (! bandIsJudged(i)) continue;
        const int r = regionOfBand(i);
        sum[r] += std::max(-12.0f, std::min(12.0f, live[i] - ref[i] - offset));
        ++count[r];
    }
    for (int r = 0; r < kRegions; ++r) out[r] = count[r] > 0 ? sum[r] / (float) count[r] : 0.0f;
}

// What Analyser::setMinLowShareDb should be for a reference: 10 dB under the reference's own
// low-end share, so sections without bass (intros, breakdowns) are not compared with its drops.
inline float minLowShareFor(const RefProfile& ref) {
    if (! ref.valid) return -200.0f;
    double low = 0.0;
    for (int i = 0; i < kBands; ++i)
        if (regionOfBand(i) <= 1) low += std::pow(10.0, ref.band[i] / 10.0);
    return (float) (10.0 * std::log10(low > 1e-12 ? low : 1e-12)) - 10.0f;
}

// Turns measurements into flags. A flag is raised after its condition has held
// for 1 s and cleared after it has been false (at a relaxed threshold) for 2 s.
class IssueDetector {
public:
    enum Id { kHot = 0, kThin = 10, kClipping = 20, kTruePeak, kBassPhase, kMixPhase, kSquashed, kDc, kLean, kLoudness, kOver, kNumIds };

    static constexpr double kRaiseSeconds = 1.0, kClearSeconds = 2.0;

    void reset() { for (Latch& l : latches) l = Latch {}; }

    // Loudness against the reference only means something where the level is the real one
    // (a DAW master), not for system audio at whatever the volume happens to be.
    void setCompareLoudness(bool shouldCompare) { compareLoudness = shouldCompare; }

    // Returns the number of issues written to out, most severe first.
    int update(const Snapshot& s, const RefProfile& ref, double dt, Issue* out, int maxOut) {
        count = 0; outPtr = out; outMax = maxOut;
        const bool on = s.active;
        const bool useRef = ref.valid;
        const bool loudPart = on && s.lufsM >= s.lufsI - 6.0f;

        float delta[kRegions] = {};
        const bool regionsReady = on && useRef && s.longValid;
        if (regionsReady) regionDeltas(s.bandLong, ref.band, delta);
        for (int r = 0; r < kRegions; ++r) {
            float lo, hi; regionRange(r, lo, hi);
            const float d = delta[r];
            consider(kHot + r,  regionsReady && d >  3.0f, regionsReady && d >  2.0f, dt, d >  6.0f ? 2 : 1, lo, hi,
                     "%s %+.1f dB vs ref", regionName(r), d);
            consider(kThin + r, regionsReady && d < -3.0f, regionsReady && d < -2.0f, dt, d < -6.0f ? 2 : 1, lo, hi,
                     "%s %+.1f dB vs ref", regionName(r), d);
        }

        // A reference that is itself clipped at full scale (most loud masters) sets the bar.
        const float refClips = useRef ? ref.clipRate : 0.0f;
        const float clips = (float) s.clipRuns / 3.0f;
        consider(kClipping, on && clips > refClips * 1.5f + 3.0f, on && clips > refClips * 1.2f + 2.0f, dt, 2, 0, 0,
                 refClips < 1.0f ? "Clipping" : "Clipping harder than ref");
        consider(kOver, on && s.overs > 0, on && s.overs > 0, dt, 2, 0, 0, "Over 0 dBFS - will clip");

        const float tpLimit = std::max(0.0f, useRef ? ref.truePeakDb : 0.0f);
        consider(kTruePeak, on && s.truePeakDb > tpLimit + 0.3f, on && s.truePeakDb > tpLimit + 0.1f, dt, 1, 0, 0,
                 "True peak %+.1f dBTP", s.truePeakDb);

        consider(kBassPhase, on && s.judging && s.corrLow < 0.4f, on && s.judging && s.corrLow < 0.55f, dt, s.corrLow < 0.0f ? 2 : 1, 20.0f, 150.0f,
                 "Bass out of phase - dies in mono");
        consider(kMixPhase, on && s.corr < -0.1f, on && s.corr < 0.0f, dt, 2, 0, 0, "Mix out of phase");

        const float punch = useRef ? ref.psr - s.psr : 0.0f;
        consider(kSquashed, loudPart && useRef && punch > 3.0f, loudPart && useRef && punch > 2.5f, dt, 1, 0, 0,
                 "Squashed: %.1f dB less punch than ref", punch);

        const float dcLimit = std::max(0.02f, useRef ? ref.dc * 1.5f : 0.0f);
        consider(kDc, on && s.dc > dcLimit, on && s.dc > dcLimit * 0.7f, dt, 1, 0, 0, "DC offset");

        const bool settled = on && s.activeSeconds >= 5.0f;
        consider(kLean, settled && std::fabs(s.balanceDb) > 1.5f, settled && std::fabs(s.balanceDb) > 1.0f, dt, 1, 0, 0,
                 "Leaning %s %.1f dB", s.balanceDb > 0.0f ? "left" : "right", std::fabs(s.balanceDb));

        const bool longRun = on && useRef && compareLoudness && s.activeSeconds >= 45.0f;
        const float lu = s.lufsI - (useRef ? ref.lufs : 0.0f);
        consider(kLoudness, longRun && std::fabs(lu) > 4.0f, longRun && std::fabs(lu) > 3.0f, dt, 0, 0, 0,
                 "%.1f LU %s than ref", std::fabs(lu), lu > 0.0f ? "louder" : "quieter");

        std::stable_sort(out, out + count, [](const Issue& a, const Issue& b) { return a.severity > b.severity; });
        return count;
    }

private:
    struct Latch { bool on = false; double held = 0.0, released = 0.0; int severity = 0; char text[96] = {}; };

    void consider(int id, bool raise, bool hold, double dt, int severity, float lo, float hi, const char* fmt, ...) {
        Latch& l = latches[id];
        if (! l.on) {
            l.held = raise ? l.held + dt : 0.0;
            if (l.held >= kRaiseSeconds) { l.on = true; l.released = 0.0; }
        } else {
            l.released = hold ? 0.0 : l.released + dt;
            if (l.released >= kClearSeconds) { l.on = false; l.held = 0.0; }
        }
        // While the flag is only coasting on its clear timer, keep the last real reading.
        if (raise || hold) {
            l.severity = severity;
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(l.text, sizeof(l.text), fmt, args);
            va_end(args);
        }
        if (! l.on || count >= outMax) return;
        Issue& issue = outPtr[count++];
        issue.id = id; issue.severity = l.severity; issue.freqLo = lo; issue.freqHi = hi;
        std::memcpy(issue.text, l.text, sizeof(issue.text));
    }

    Latch latches[kNumIds];
    Issue* outPtr = nullptr;
    int count = 0, outMax = 0;
    bool compareLoudness = true;
};

} // namespace snitch
