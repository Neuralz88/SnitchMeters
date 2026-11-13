#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>
#include "Analyser.h"

namespace snitch {

// Measures a whole track the same way the live meter does. Only the loud
// sections (3 s windows within 6 LU of the integrated loudness) are profiled,
// so the result describes the drops, not the intro. valid=false when there is
// less than 10 s of that.
inline RefProfile buildProfile(const float* L, const float* R, size_t n, double sampleRate, const char* name) {
    RefProfile p {};
    std::strncpy(p.name, name ? name : "", sizeof(p.name) - 1);

    Analyser a;
    a.setAutoReset(false);
    a.prepare(sampleRate);

    struct Window { float lufsS, psr, corrLow, clipRate, dc; double pow[kBands]; };
    std::vector<Window> windows;
    const size_t win = (size_t) a.slotLength() * 30;
    Snapshot s {};
    for (size_t pos = 0; pos + win <= n; pos += win) {
        a.resetAccum();
        a.process(L + pos, R ? R + pos : nullptr, (int) win);
        s = a.snapshot();
        Window w;
        w.lufsS = s.lufsS; w.psr = s.psr; w.corrLow = s.corrLow; w.clipRate = (float) s.clipRuns / 3.0f; w.dc = s.dc;
        a.readAccum(w.pow);
        windows.push_back(w);
    }
    p.lufs = s.lufsI;
    p.truePeakDb = s.truePeakMaxDb;

    std::vector<const Window*> loud;
    for (const Window& w : windows)
        if (w.lufsS > -60.0f && w.lufsS >= p.lufs - 6.0f) loud.push_back(&w);
    if (loud.size() * 3 < 10) return p;

    auto percentile = [](std::vector<float>& v, double q) {
        std::sort(v.begin(), v.end());
        return v[(size_t) (q * (double) (v.size() - 1) + 0.5)];
    };
    auto relDb = [](double power, double total) {
        return (power > 0.0 && total > 0.0) ? (float) std::max(-120.0, 10.0 * std::log10(power / total)) : -120.0f;
    };

    double mean[kBands] = {}, total = 0.0;
    for (const Window* w : loud)
        for (int i = 0; i < kBands; ++i) mean[i] += w->pow[i];
    for (int i = 0; i < kBands; ++i) total += mean[i];

    std::vector<float> column(loud.size());
    for (int i = 0; i < kBands; ++i) {
        p.band[i] = relDb(mean[i], total);
        for (size_t k = 0; k < loud.size(); ++k) {
            double windowTotal = 0.0;
            for (int j = 0; j < kBands; ++j) windowTotal += loud[k]->pow[j];
            column[k] = relDb(loud[k]->pow[i], windowTotal);
        }
        p.bandLo[i] = std::min(percentile(column, 0.1), p.band[i]);
        p.bandHi[i] = std::max(percentile(column, 0.9), p.band[i]);
    }

    for (size_t k = 0; k < loud.size(); ++k) column[k] = loud[k]->psr;
    p.psr = percentile(column, 0.5);
    for (size_t k = 0; k < loud.size(); ++k) column[k] = loud[k]->corrLow;
    p.corrLow = percentile(column, 0.5);
    for (size_t k = 0; k < loud.size(); ++k) column[k] = loud[k]->clipRate;
    p.clipRate = percentile(column, 0.9);
    for (size_t k = 0; k < loud.size(); ++k) column[k] = loud[k]->dc;
    p.dc = percentile(column, 0.9);

    p.valid = true;
    return p;
}

} // namespace snitch
