#pragma once
#include <cmath>
#include <vector>

namespace snitch {

// ITU-R BS.1770 loudness: K-weighting, momentary (400 ms), short-term (3 s),
// gated integrated. Stereo; a null right channel is treated as dual mono.
class Loudness {
public:
    void prepare(double sampleRate) {
        const double pi = 3.141592653589793;
        // High shelf, then high-pass, from the BS.1770 analogue prototypes so any rate works.
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan(pi * f0 / sampleRate);
            const double Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            Biquad b;
            b.b0 = (Vh + Vb * K / Q + K * K) / a0;
            b.b1 = 2.0 * (K * K - Vh) / a0;
            b.b2 = (Vh - Vb * K / Q + K * K) / a0;
            b.a1 = 2.0 * (K * K - 1.0) / a0;
            b.a2 = (1.0 - K / Q + K * K) / a0;
            shelf[0] = shelf[1] = b;
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan(pi * f0 / sampleRate);
            const double a0 = 1.0 + K / Q + K * K;
            Biquad b;
            b.b0 = 1.0; b.b1 = -2.0; b.b2 = 1.0;
            b.a1 = 2.0 * (K * K - 1.0) / a0;
            b.a2 = (1.0 - K / Q + K * K) / a0;
            highpass[0] = highpass[1] = b;
        }
        slotLen = (int) std::lround(sampleRate * 0.1);
        if (slotLen < 1) slotLen = 1;
        gateBlocks.reserve(36000);   // an hour, so the audio thread rarely allocates
        reset();
    }

    void reset() {
        for (int c = 0; c < 2; ++c) { shelf[c].clear(); highpass[c].clear(); }
        slotPos = 0; slotAcc = 0.0; slotCount = 0; slotIdx = 0;
        for (double& s : slots) s = 0.0;
        resetIntegrated();
    }

    void resetIntegrated() { gateBlocks.clear(); integratedDirty = true; integratedCache = kSilence; }

    void process(const float* L, const float* R, int n) {
        for (int i = 0; i < n; ++i) {
            const double l = highpass[0].tick(shelf[0].tick(L[i]));
            const double r = R ? highpass[1].tick(shelf[1].tick(R[i])) : l;
            slotAcc += l * l + r * r;
            if (++slotPos >= slotLen) {
                slots[slotIdx] = slotAcc / slotLen;
                slotIdx = (slotIdx + 1) % kSlots;
                ++slotCount;
                slotPos = 0; slotAcc = 0.0;
                if (slotCount >= 4) {
                    const double e = meanOfLast(4);
                    if (toLufs(e) > -70.0f) { gateBlocks.push_back(e); integratedDirty = true; }
                }
            }
        }
    }

    float momentary() const { return toLufs(meanOfLast(4)); }
    float shortTerm() const { return toLufs(meanOfLast(kSlots)); }

    float integrated() const {
        if (integratedDirty) {
            integratedDirty = false;
            integratedCache = kSilence;
            if (! gateBlocks.empty()) {
                double sum = 0.0;
                for (double e : gateBlocks) sum += e;
                const double relGate = sum / (double) gateBlocks.size() * 0.1; // -10 LU
                double gated = 0.0; size_t count = 0;
                for (double e : gateBlocks) if (e >= relGate) { gated += e; ++count; }
                if (count > 0) integratedCache = toLufs(gated / (double) count);
            }
        }
        return integratedCache;
    }

    int slotLength() const { return slotLen; }

    static constexpr float kSilence = -120.0f;

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double tick(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void clear() { z1 = z2 = 0; }
    };

    static float toLufs(double energy) {
        if (! (energy > 1e-12)) return kSilence;
        return (float) (-0.691 + 10.0 * std::log10(energy));
    }

    double meanOfLast(int count) const {
        if (count > slotCount) count = slotCount;
        if (count <= 0) return 0.0;
        double sum = 0.0;
        for (int i = 1; i <= count; ++i) sum += slots[(slotIdx - i + kSlots * 2) % kSlots];
        return sum / count;
    }

    static constexpr int kSlots = 30; // 3 s of 100 ms slots
    Biquad shelf[2], highpass[2];
    int slotLen = 4800, slotPos = 0, slotCount = 0, slotIdx = 0;
    double slotAcc = 0.0;
    double slots[kSlots] = {};
    std::vector<double> gateBlocks;
    mutable bool integratedDirty = true;
    mutable float integratedCache = kSilence;
};

} // namespace snitch
