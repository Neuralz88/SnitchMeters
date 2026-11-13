#pragma once
#include <cmath>

namespace snitch {

// 4x oversampled peak for one channel (48-tap windowed sinc, 12 taps per phase).
class TruePeak {
public:
    void prepare() {
        const double pi = 3.141592653589793;
        for (int i = 0; i < kPhases * kTaps; ++i) {
            const double t = (i - 24) / (double) kPhases;
            const double sinc = (i == 24) ? 1.0 : std::sin(pi * t) / (pi * t);
            const double window = 0.5 + 0.5 * std::cos(pi * (i - 24) / 24.0);
            h[i % kPhases][i / kPhases] = (float) (sinc * window);
        }
        for (int p = 0; p < kPhases; ++p) {
            float sum = 0.0f;
            for (int k = 0; k < kTaps; ++k) sum += h[p][k];
            for (int k = 0; k < kTaps; ++k) h[p][k] /= sum;
        }
        reset();
    }

    void reset() {
        for (float& v : hist) v = 0.0f;
        pos = 0;
    }

    // Returns the largest oversampled magnitude in this block (linear).
    float process(const float* x, int n) {
        float peak = 0.0f;
        for (int i = 0; i < n; ++i) {
            pos = (pos + kTaps - 1) % kTaps;
            hist[pos] = hist[pos + kTaps] = x[i];   // doubled so the taps read contiguously
            const float* d = hist + pos;            // d[k] = x[n - k]
            for (int p = 0; p < kPhases; ++p) {
                float y = 0.0f;
                for (int k = 0; k < kTaps; ++k) y += d[k] * h[p][k];
                const float a = std::fabs(y);
                if (a > peak) peak = a;
            }
        }
        return peak;
    }

private:
    static constexpr int kPhases = 4, kTaps = 12;
    float h[kPhases][kTaps] = {};
    float hist[kTaps * 2] = {};
    int pos = 0;
};

} // namespace snitch
