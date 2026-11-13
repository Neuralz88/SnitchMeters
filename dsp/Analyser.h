#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>
#include "Bands.h"
#include "Fft.h"
#include "Loudness.h"
#include "TruePeak.h"

namespace snitch {

// Everything the meter measures live. process() runs on one thread (audio or
// capture); snapshot() and copyScope() may be called from another.
class Analyser {
public:
    static constexpr int kScope = 32768;
    static constexpr float kClipLevel = 0.9999f, kOverLevel = 1.0005f;
    static constexpr double kLongSeconds = 30.0;

    // Start the long-term numbers again after 2 s of silence (a new track). Off for offline use.
    void setAutoReset(bool shouldAutoReset) { autoReset = shouldAutoReset; }

    // Moments with less low end than this are treated as intro/breakdown and kept out of the
    // long average. Safe from any thread.
    void setMinLowShareDb(float db) { minLowShareDb.store(db); }

    void prepare(double sampleRate) {
        fs = sampleRate > 1000.0 ? sampleRate : 48000.0;
        fft = std::make_unique<Fft>(fs > 50000.0 ? 14 : 13);
        N = fft->size();
        hop = N / 8;

        window.resize((size_t) N);
        Fft::hann(window.data(), N);
        double wsum = 0.0;
        for (float w : window) wsum += w;
        powScale = (float) (4.0 / (wsum * wsum));   // full-scale sine -> 1.0 at its peak bin

        fifoL.assign((size_t) N, 0.0f); fifoR.assign((size_t) N, 0.0f);
        re.assign((size_t) N, 0.0f);    im.assign((size_t) N, 0.0f);
        framePow.assign((size_t) N / 2 + 1, 0.0f);
        binSmooth.assign((size_t) N / 2 + 1, 0.0f);
        scopeL.assign(kScope, 0.0f);    scopeR.assign(kScope, 0.0f);
        buildBandMap();
        buildPointMap();

        loud.prepare(fs);
        tp[0].prepare(); tp[1].prepare();
        slotLen = loud.slotLength();

        const double pi = 3.141592653589793;
        const double w0 = 2.0 * pi * 150.0 / fs, alpha = std::sin(w0) / (2.0 * 0.7071), a0 = 1.0 + alpha;
        lpB0 = (1.0 - std::cos(w0)) / 2.0 / a0; lpB1 = (1.0 - std::cos(w0)) / a0; lpB2 = lpB0;
        lpA1 = -2.0 * std::cos(w0) / a0;         lpA2 = (1.0 - alpha) / a0;

        secondCoef = 1.0 - std::exp(-1.0 / fs);
        dcCoef = 1.0 - std::exp(-1.0 / (fs * 4.0));   // slow: infrasonic content is not an offset
        specCoef  = (float) (1.0 - std::exp(-hop / (fs * 0.15)));
        shortCoef = (float) (1.0 - std::exp(-hop / (fs * 0.3)));
        longCoef  = 1.0 - std::exp(-hop / (fs * kLongSeconds));
        reset();
    }

    void reset() {
        loud.reset(); tp[0].reset(); tp[1].reset();
        std::fill(fifoL.begin(), fifoL.end(), 0.0f); std::fill(fifoR.begin(), fifoR.end(), 0.0f);
        std::fill(binSmooth.begin(), binSmooth.end(), 0.0f);
        std::fill(scopeL.begin(), scopeL.end(), 0.0f); std::fill(scopeR.begin(), scopeR.end(), 0.0f);
        fifoPos = hopCount = slotPos = ringIdx = 0;
        for (int i = 0; i < kRing; ++i) { tpRing[i] = 0.0f; clipRing[i] = 0; overRing[i] = 0; }
        for (int i = 0; i < kBands; ++i) bandShortP[i] = 0.0;
        for (int c = 0; c < 2; ++c) { lpZ1[c] = lpZ2[c] = 0.0; clipRun[c] = 0; dcMean[c] = 0.0; }
        cLL = cRR = cLR = lLL = lRR = lLR = 0.0;
        slotPeak = 0.0f; slotClips = slotOvers = 0; slotEL = slotER = 0.0; lowShareDb = -120.0f;
        active = gateOpen = false; silentSlots = 0;
        resetLong();
        resetAccum();
        dirty = true;
        publish();
    }

    // Safe from any thread: applied at the start of the next process().
    void requestReset() { resetRequested.store(true); }

    void process(const float* L, const float* R, int n) {
        if (fft == nullptr || L == nullptr) return;
        if (resetRequested.exchange(false)) resetLong();
        float bl[kChunk], br[kChunk];
        while (n > 0) {
            const int c = std::min(std::min(n, (int) kChunk), slotLen - slotPos);
            for (int i = 0; i < c; ++i) {
                const float l = L[i], r = R ? R[i] : l;
                bl[i] = std::isfinite(l) ? l : 0.0f;
                br[i] = std::isfinite(r) ? r : 0.0f;
            }
            processChunk(bl, br, c);
            L += c; if (R) R += c; n -= c;
        }
        if (dirty) publish();
    }

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lock(snapLock);
        return shared;
    }

    // Most recent n samples, oldest first. Returns n.
    int copyScope(float* dstL, float* dstR, int n) const {
        n = std::min(n, (int) kScope);
        if (scopeL.empty()) { std::fill(dstL, dstL + n, 0.0f); std::fill(dstR, dstR + n, 0.0f); return n; }
        const int w = scopeWrite.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            const int idx = (w - n + i + kScope * 2) % kScope;
            dstL[i] = scopeL[(size_t) idx]; dstR[i] = scopeR[(size_t) idx];
        }
        return n;
    }

    double sampleRate() const { return fs; }
    int slotLength() const { return slotLen; }

    // Mean band powers (linear) of the frames since resetAccum(). For the offline profile builder.
    void resetAccum() { for (double& a : accumP) a = 0.0; accumFrames = 0; }
    int readAccum(double* out) const {
        for (int i = 0; i < kBands; ++i) out[i] = accumFrames > 0 ? accumP[i] / accumFrames : 0.0;
        return accumFrames;
    }

private:
    static constexpr int kChunk = 512, kRing = 30;
    struct MapEntry { int bin, band; float weight; };
    struct Point { int lo, hi; float pos; bool interpolate; };

    static float toDb(double linear) { return linear > 1e-6 ? (float) (20.0 * std::log10(linear)) : -120.0f; }
    static float powDb(double power)  { return power > 1e-12 ? (float) (10.0 * std::log10(power)) : -120.0f; }

    void buildBandMap() {
        bandMap.clear();
        const double df = fs / N;
        for (int k = 1; k <= N / 2; ++k) {
            const double lo = (k - 0.5) * df, hi = (k + 0.5) * df;
            for (int b = 0; b < kBands; ++b) {
                const double overlap = std::min(hi, (double) bandHighEdge(b)) - std::max(lo, (double) bandLowEdge(b));
                if (overlap > 0.0) bandMap.push_back({ k, b, (float) (overlap / df) });
            }
        }
    }

    void buildPointMap() {
        const double df = fs / N;
        auto binOf = [df](int j) { return 20.0 * std::pow(1000.0, j / (double) (kSpecPoints - 1)) / df; };
        for (int j = 0; j < kSpecPoints; ++j) {
            const double b = binOf(j), lo = 0.5 * (binOf(j - 1) + b), hi = 0.5 * (b + binOf(j + 1));
            Point p;
            p.pos = (float) std::min(b, (double) (N / 2 - 1));
            p.interpolate = (hi - lo) < 1.0;
            p.lo = std::min((int) std::ceil(lo), N / 2);
            p.hi = std::min((int) std::floor(hi), N / 2);
            if (p.hi < p.lo) p.interpolate = true;
            points[j] = p;
        }
    }

    void resetLong() {
        loud.resetIntegrated();
        for (double& p : bandLongP) p = 0.0;
        longFrames = 0; longSeconds = 0.0; activeSeconds = 0.0;
        balL = balR = 0.0; balCount = 0;
        tpMax = 0.0f;
        dirty = true;
    }

    void processChunk(const float* l, const float* r, int c) {
        loud.process(l, r, c);
        slotPeak = std::max(slotPeak, std::max(tp[0].process(l, c), tp[1].process(r, c)));

        int w = scopeWrite.load(std::memory_order_relaxed);
        for (int i = 0; i < c; ++i) {
            const double x[2] = { l[i], r[i] };
            double low[2];
            for (int ch = 0; ch < 2; ++ch) {
                if (std::fabs(x[ch]) >= kClipLevel) { if (++clipRun[ch] == 3) ++slotClips; }
                else clipRun[ch] = 0;
                if (std::fabs(x[ch]) > kOverLevel) ++slotOvers;
                low[ch] = lpB0 * x[ch] + lpZ1[ch];
                lpZ1[ch] = lpB1 * x[ch] - lpA1 * low[ch] + lpZ2[ch];
                lpZ2[ch] = lpB2 * x[ch] - lpA2 * low[ch];
                dcMean[ch] += dcCoef * (x[ch] - dcMean[ch]);
            }
            cLL += secondCoef * (x[0] * x[0] - cLL);
            cRR += secondCoef * (x[1] * x[1] - cRR);
            cLR += secondCoef * (x[0] * x[1] - cLR);
            lLL += secondCoef * (low[0] * low[0] - lLL);
            lRR += secondCoef * (low[1] * low[1] - lRR);
            lLR += secondCoef * (low[0] * low[1] - lLR);
            slotEL += x[0] * x[0]; slotER += x[1] * x[1];

            scopeL[(size_t) w] = l[i]; scopeR[(size_t) w] = r[i];
            w = (w + 1) % kScope;

            fifoL[(size_t) fifoPos] = l[i]; fifoR[(size_t) fifoPos] = r[i];
            fifoPos = (fifoPos + 1) % N;
            if (++hopCount >= hop) { hopCount = 0; frame(); }
        }
        scopeWrite.store(w, std::memory_order_relaxed);

        slotPos += c;
        if (slotPos >= slotLen) endSlot();
    }

    void transform(const std::vector<float>& fifo, bool accumulate) {
        for (int i = 0; i < N; ++i) {
            re[(size_t) i] = fifo[(size_t) ((fifoPos + i) % N)] * window[(size_t) i];
            im[(size_t) i] = 0.0f;
        }
        fft->forward(re.data(), im.data());
        for (int k = 0; k <= N / 2; ++k) {
            const float p = (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]) * powScale * 0.5f;
            framePow[(size_t) k] = accumulate ? framePow[(size_t) k] + p : p;
        }
    }

    void frame() {
        transform(fifoL, false);
        transform(fifoR, true);

        for (int k = 0; k <= N / 2; ++k)
            binSmooth[(size_t) k] += specCoef * (framePow[(size_t) k] - binSmooth[(size_t) k]);

        double bp[kBands] = {};
        for (const MapEntry& e : bandMap) bp[e.band] += (double) framePow[(size_t) e.bin] * e.weight;

        for (int i = 0; i < kBands; ++i) {
            bandShortP[i] += shortCoef * (bp[i] - bandShortP[i]);
            accumP[i] += bp[i];
        }
        ++accumFrames;

        if (gateOpen) {
            // Plain running mean until it has as much history as the smoother wants.
            ++longFrames;
            const double a = std::max(1.0 / (double) longFrames, longCoef);
            for (int i = 0; i < kBands; ++i) bandLongP[i] += a * (bp[i] - bandLongP[i]);
            longSeconds += hop / fs;
        }
        dirty = true;
    }

    void endSlot() {
        slotPos = 0;
        tpRing[ringIdx] = slotPeak; clipRing[ringIdx] = slotClips; overRing[ringIdx] = slotOvers;
        ringIdx = (ringIdx + 1) % kRing;
        tpMax = std::max(tpMax, slotPeak);
        slotPeak = 0.0f; slotClips = slotOvers = 0;

        double low = 0.0, total = 0.0;
        for (int i = 0; i < kBands; ++i) { total += bandShortP[i]; if (regionOfBand(i) <= 1) low += bandShortP[i]; }
        lowShareDb = total > 1e-14 ? powDb(low / total) : -120.0f;

        const float m = loud.momentary();
        active = m > -50.0f;
        if (active) {
            if (autoReset && silentSlots >= 20) resetLong();
            silentSlots = 0;
            activeSeconds += 0.1;
            ++balCount;
            const double a = std::max(1.0 / (double) balCount, 1.0 - std::exp(-0.1 / 8.0));
            balL += a * (slotEL - balL); balR += a * (slotER - balR);
        } else if (silentSlots < 1000000) {
            ++silentSlots;
        }
        // Only loud parts that have low end feed the long-term balance, so an intro or a
        // breakdown does not read as a thin mix.
        gateOpen = active && m >= loud.integrated() - 6.0f && lowShareDb >= minLowShareDb.load();
        slotEL = slotER = 0.0;
        dirty = true;
    }

    void publish() {
        Snapshot s {};
        s.active = active;
        s.longValid = longSeconds >= 12.0;
        s.judging = gateOpen;
        s.lowShareDb = lowShareDb;
        s.lufsM = loud.momentary(); s.lufsS = loud.shortTerm(); s.lufsI = loud.integrated();
        float hold = 0.0f; int clips = 0, overs = 0;
        for (int i = 0; i < kRing; ++i) { hold = std::max(hold, tpRing[i]); clips += clipRing[i]; overs += overRing[i]; }
        s.overs = overs + slotOvers;
        hold = std::max(hold, slotPeak);
        s.truePeakDb = toDb(hold);
        s.truePeakMaxDb = toDb(std::max(tpMax, hold));
        s.clipRuns = clips + slotClips;
        s.corr    = cLL * cRR > 1e-18 ? (float) (cLR / std::sqrt(cLL * cRR)) : 0.0f;
        s.corrLow = lLL * lRR > 1e-18 ? (float) (lLR / std::sqrt(lLL * lRR)) : 0.0f;
        s.dc = (float) std::max(std::fabs(dcMean[0]), std::fabs(dcMean[1]));
        s.psr = active ? s.truePeakDb - s.lufsS : 0.0f;
        s.balanceDb = (balCount > 0 && balL > 1e-12 && balR > 1e-12) ? (float) (10.0 * std::log10(balL / balR)) : 0.0f;
        s.activeSeconds = (float) activeSeconds;

        double totalShort = 0.0, totalLong = 0.0;
        for (int i = 0; i < kBands; ++i) { totalShort += bandShortP[i]; totalLong += bandLongP[i]; }
        for (int i = 0; i < kBands; ++i) {
            s.bandShort[i] = totalShort > 1e-14 ? powDb(bandShortP[i] / totalShort) : -120.0f;
            s.bandLong[i]  = totalLong  > 1e-14 ? powDb(bandLongP[i] / totalLong)   : -120.0f;
        }
        for (int j = 0; j < kSpecPoints; ++j) {
            const Point& p = points[j];
            float v;
            if (p.interpolate) {
                const int k = (int) p.pos;
                const float t = p.pos - (float) k;
                v = binSmooth[(size_t) k] * (1.0f - t) + binSmooth[(size_t) k + 1] * t;
            } else {
                v = 0.0f;
                for (int k = p.lo; k <= p.hi; ++k) v = std::max(v, binSmooth[(size_t) k]);
            }
            s.spectrum[j] = powDb(v);
        }

        // Never wait on the reader: if it holds the lock, publish on the next round.
        std::unique_lock<std::mutex> lock(snapLock, std::try_to_lock);
        if (lock.owns_lock()) { shared = s; dirty = false; }
    }

    double fs = 48000.0;
    int N = 0, hop = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> window, fifoL, fifoR, re, im, framePow, binSmooth, scopeL, scopeR;
    std::vector<MapEntry> bandMap;
    Point points[kSpecPoints] = {};
    float powScale = 1.0f, specCoef = 0.1f, shortCoef = 0.1f;
    double longCoef = 0.01, secondCoef = 0.0, dcCoef = 0.0;
    int fifoPos = 0, hopCount = 0;

    Loudness loud;
    TruePeak tp[2];
    int slotLen = 4800, slotPos = 0, ringIdx = 0;
    float tpRing[kRing] = {}, slotPeak = 0.0f, tpMax = 0.0f;
    int clipRing[kRing] = {}, overRing[kRing] = {}, slotClips = 0, slotOvers = 0, clipRun[2] = {};
    float lowShareDb = -120.0f;
    std::atomic<float> minLowShareDb { -200.0f };
    double slotEL = 0.0, slotER = 0.0, balL = 0.0, balR = 0.0;
    long long balCount = 0, longFrames = 0;

    double lpB0 = 0, lpB1 = 0, lpB2 = 0, lpA1 = 0, lpA2 = 0, lpZ1[2] = {}, lpZ2[2] = {};
    double cLL = 0, cRR = 0, cLR = 0, lLL = 0, lRR = 0, lLR = 0, dcMean[2] = {};

    double bandShortP[kBands] = {}, bandLongP[kBands] = {}, accumP[kBands] = {};
    int accumFrames = 0;
    double longSeconds = 0.0, activeSeconds = 0.0;
    bool active = false, gateOpen = false, autoReset = true, dirty = true;
    int silentSlots = 0;

    std::atomic<bool> resetRequested { false };
    std::atomic<int> scopeWrite { 0 };
    mutable std::mutex snapLock;
    Snapshot shared {};
};

} // namespace snitch
