#pragma once
#include <cmath>
#include <utility>
#include <vector>

namespace snitch {

// In-place radix-2 complex FFT.
class Fft {
public:
    explicit Fft(int order) : n(1 << order), rev((size_t) n), cosT((size_t) n / 2), sinT((size_t) n / 2) {
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < order; ++b)
                if (i & (1 << b)) r |= 1 << (order - 1 - b);
            rev[(size_t) i] = r;
        }
        const double twoPi = 6.283185307179586;
        for (int k = 0; k < n / 2; ++k) {
            cosT[(size_t) k] = (float) std::cos(twoPi * k / n);
            sinT[(size_t) k] = (float) std::sin(twoPi * k / n);
        }
    }

    int size() const { return n; }

    void forward(float* re, float* im) const {
        for (int i = 0; i < n; ++i) {
            const int j = rev[(size_t) i];
            if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len / 2, step = n / len;
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < half; ++j) {
                    const float wr = cosT[(size_t) (j * step)], wi = -sinT[(size_t) (j * step)];
                    const float xr = re[i + j + half], xi = im[i + j + half];
                    const float vr = xr * wr - xi * wi, vi = xr * wi + xi * wr;
                    re[i + j + half] = re[i + j] - vr;  im[i + j + half] = im[i + j] - vi;
                    re[i + j] += vr;                    im[i + j] += vi;
                }
            }
        }
    }

    static void hann(float* w, int count) {
        const double twoPi = 6.283185307179586;
        for (int i = 0; i < count; ++i)
            w[i] = (float) (0.5 - 0.5 * std::cos(twoPi * i / count));
    }

private:
    int n;
    std::vector<int> rev;
    std::vector<float> cosT, sinT;
};

} // namespace snitch
