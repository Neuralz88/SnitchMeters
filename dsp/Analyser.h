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

// One slice (about a quarter of a millisecond) of the waveform history: the range the
// mono sum covered, the highest sample on either side, and how loud its lows, mids and highs were.
struct WaveBucket { float lo, hi, peak, low, mid, high; };

// What the waveform, the live spectrum and the level numbers show: the whole mix, or one
// channel of it as it would sound on both speakers. Mid = (L+R)/2, side = (L-R)/2.
enum View { kViewStereo, kViewLeft, kViewRight, kViewMid, kViewSide, kViews };

inline const char* viewName(int view) {
    static const char* names[kViews] = { "STEREO", "LEFT", "RIGHT", "MID", "SIDE" };
    return names[view >= 0 && view < kViews ? view : 0];
}

// Everything the meter measures live. process() runs on one thread (audio or
// capture); snapshot(), copyScope() and copyWave() may be called from another.
class Analyser {
public:
    static constexpr int kScope = 32768;
    static constexpr int kWave = 131072;   // buckets of history: about 32 s
    static constexpr float kWaveLowHz = 200.0f, kWaveHighHz = 2000.0f;
    static constexpr float kClipLevel = 0.9999f, kOverLevel = 1.0005f;
    static constexpr double kLongSeconds = 30.0;

    // Start the long-term numbers again after 2 s of silence (a new track). Off for offline use.
    void setAutoReset(bool shouldAutoReset) { autoReset = shouldAutoReset; }

    // Moments with less low end than this are treated as intro/breakdown and kept out of the
    // long average. Safe from any thread.
    void setMinLowShareDb(float db) { minLowShareDb.store(db); }

    // Which channel the view* numbers and viewSpectrum follow. Safe from any thread. The
    // waveform history and the spectrum are kept for every view, so those switch at once;
    // the level numbers start again.
    void setView(int newView) { view.store(newView >= 0 && newView < kViews ? newView : kViewStereo); }
    int currentView() const { return view.load(); }

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
        re2.assign((size_t) N, 0.0f);   im2.assign((size_t) N, 0.0f);
        framePow.assign((size_t) N / 2 + 1, 0.0f);
        binSmooth.assign((size_t) N / 2 + 1, 0.0f);
        for (auto& smooth : viewSmooth) smooth.assign((size_t) N / 2 + 1, 0.0f);
        scopeL.assign(kScope, 0.0f);    scopeR.assign(kScope, 0.0f);
        for (auto& history : wave) history.assign(kWave, WaveBucket {});
        waveLen = std::max(1, (int) std::lround(fs / 4000.0));
        waveLowCoef  = (float) (1.0 - std::exp(-2.0 * 3.141592653589793 * kWaveLowHz / fs));
        waveHighCoef = (float) (1.0 - std::exp(-2.0 * 3.141592653589793 * kWaveHighHz / fs));
        buildBandMap();
        buildPointMap();

        loud.prepare(fs);
        tp[0].prepare(); tp[1].prepare();
        soloLoud.prepare(fs);
        soloTp.prepare();
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
        for (auto& smooth : viewSmooth) std::fill(smooth.begin(), smooth.end(), 0.0f);
        std::fill(scopeL.begin(), scopeL.end(), 0.0f); std::fill(scopeR.begin(), scopeR.end(), 0.0f);
        fifoPos = hopCount = slotPos = ringIdx = 0;
        startWaveBucket();
        for (WaveAcc& acc : waveAcc) for (float& z : acc.z) z = 0.0f;
        resetSolo();
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
        if (const int v = view.load(); v != soloView) { soloView = v; resetSolo(); }
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

    // Waveform history. Buckets are numbered from 0 and never renumbered; waveCount() is how many
    // exist so far. copyWave() fills dst with buckets first .. first+n-1, zeroed where a bucket is
    // not there yet or is older than the history goes back.
    long long waveCount() const { return waveTotal.load(std::memory_order_relaxed); }
    double waveBucketsPerSecond() const { return fs / waveLen; }
    void copyWave(WaveBucket* dst, long long first, int n, int ofView = kViewStereo) const {
        const std::vector<WaveBucket>& history = wave[ofView >= 0 && ofView < kViews ? ofView : kViewStereo];
        const long long total = waveCount();
        for (int i = 0; i < n; ++i) {
            const long long index = first + i;
            dst[i] = (index >= 0 && index < total && index >= total - kWave && ! history.empty())
                         ? history[(size_t) (index % kWave)] : WaveBucket {};
        }
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
    // One waveform slice being filled: filter states, range, peak, band energies.
    struct WaveAcc { float z[8], lo, hi, peak, e[3]; };
    enum { kAccMono, kAccLeft, kAccRight, kAccSide, kAccs };

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
        soloLoud.resetIntegrated();
        dirty = true;
    }

    void resetSolo() {
        soloLoud.reset(); soloTp.reset();
        for (float& p : soloRing) p = 0.0f;
        soloSlotPeak = 0.0f;
        dirty = true;
    }

    // The channel the level numbers follow, as it would sound on both speakers.
    void processSolo(const float* l, const float* r, int c) {
        if (soloView == kViewStereo) return;
        float x[kChunk];
        for (int i = 0; i < c; ++i)
            x[i] = soloView == kViewLeft ? l[i] : soloView == kViewRight ? r[i]
                 : soloView == kViewMid ? 0.5f * (l[i] + r[i]) : 0.5f * (l[i] - r[i]);
        soloLoud.process(x, x, c);
        soloSlotPeak = std::max(soloSlotPeak, soloTp.process(x, c));
    }

    void processChunk(const float* l, const float* r, int c) {
        loud.process(l, r, c);
        slotPeak = std::max(slotPeak, std::max(tp[0].process(l, c), tp[1].process(r, c)));
        processSolo(l, r, c);

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
            waveSample(l[i], r[i]);

            fifoL[(size_t) fifoPos] = l[i]; fifoR[(size_t) fifoPos] = r[i];
            fifoPos = (fifoPos + 1) % N;
            if (++hopCount >= hop) { hopCount = 0; frame(); }
        }
        scopeWrite.store(w, std::memory_order_relaxed);

        slotPos += c;
        if (slotPos >= slotLen) endSlot();
    }

    void startWaveBucket() {
        waveFill = 0;
        for (WaveAcc& acc : waveAcc) { acc.lo = 1e9f; acc.hi = -1e9f; acc.peak = 0.0f; acc.e[0] = acc.e[1] = acc.e[2] = 0.0f; }
        waveMidPeak = 0.0f;
    }

    void waveSample(float l, float r) {
        const float m = 0.5f * (l + r);
        const float x[kAccs] = { m, l, r, 0.5f * (l - r) };
        // The whole mix is drawn as its mono sum, but peaks at the highest sample on either side.
        const float peaks[kAccs] = { std::max(std::fabs(l), std::fabs(r)), std::fabs(l), std::fabs(r), std::fabs(x[kAccSide]) };
        for (int k = 0; k < kAccs; ++k) {
            WaveAcc& acc = waveAcc[k];
            // Each band is filtered on its own (two one-pole stages a side): splitting by subtraction
            // would leak half the sub into the "mid" through the phase shift.
            float* z = acc.z;
            auto lowpass  = [&z](float v, float coef) { *z += coef * (v - *z); return *z++; };
            auto highpass = [&z](float v, float coef) { *z += coef * (v - *z); return v - *z++; };
            const float low  = lowpass(lowpass(x[k], waveLowCoef), waveLowCoef);
            const float mid  = lowpass(lowpass(highpass(highpass(x[k], waveLowCoef), waveLowCoef), waveHighCoef), waveHighCoef);
            const float high = highpass(highpass(x[k], waveHighCoef), waveHighCoef);
            acc.lo = std::min(acc.lo, x[k]); acc.hi = std::max(acc.hi, x[k]);
            acc.peak = std::max(acc.peak, peaks[k]);
            acc.e[0] += low * low; acc.e[1] += mid * mid; acc.e[2] += high * high;
        }
        waveMidPeak = std::max(waveMidPeak, std::fabs(m));
        if (++waveFill < waveLen) return;

        const long long total = waveTotal.load(std::memory_order_relaxed);
        const size_t slot = (size_t) (total % kWave);
        const float scale = 1.0f / (float) waveLen;
        static constexpr int viewOfAcc[kAccs] = { kViewStereo, kViewLeft, kViewRight, kViewSide };
        for (int k = 0; k < kAccs; ++k) {
            const WaveAcc& acc = waveAcc[k];
            wave[viewOfAcc[k]][slot] = { acc.lo, acc.hi, acc.peak, std::sqrt(acc.e[0] * scale),
                                         std::sqrt(acc.e[1] * scale), std::sqrt(acc.e[2] * scale) };
        }
        wave[kViewMid][slot] = wave[kViewStereo][slot];   // the same mono sum, with its own peak
        wave[kViewMid][slot].peak = waveMidPeak;
        waveTotal.store(total + 1, std::memory_order_relaxed);
        startWaveBucket();
    }

    void transform(const std::vector<float>& fifo, std::vector<float>& real, std::vector<float>& imag) {
        for (int i = 0; i < N; ++i) {
            real[(size_t) i] = fifo[(size_t) ((fifoPos + i) % N)] * window[(size_t) i];
            imag[(size_t) i] = 0.0f;
        }
        fft->forward(real.data(), imag.data());
    }

    void frame() {
        transform(fifoL, re, im);
        transform(fifoR, re2, im2);

        for (int k = 0; k <= N / 2; ++k) {
            const size_t b = (size_t) k;
            const float pl = (re[b] * re[b] + im[b] * im[b]) * powScale, pr = (re2[b] * re2[b] + im2[b] * im2[b]) * powScale;
            const float mr = 0.5f * (re[b] + re2[b]), mi = 0.5f * (im[b] + im2[b]);
            const float sr = 0.5f * (re[b] - re2[b]), si = 0.5f * (im[b] - im2[b]);
            // One channel counts as that channel on both speakers, so a mono mix reads the same in every view but side.
            const float p[kViews] = { 0.5f * pl + 0.5f * pr, pl, pr, (mr * mr + mi * mi) * powScale, (sr * sr + si * si) * powScale };
            framePow[b] = p[kViewStereo];
            binSmooth[b] += specCoef * (framePow[b] - binSmooth[b]);
            for (int v = 1; v < kViews; ++v) viewSmooth[v][b] += specCoef * (p[v] - viewSmooth[v][b]);
        }

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
        soloRing[ringIdx] = soloSlotPeak; soloSlotPeak = 0.0f;
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

        s.view = soloView;
        s.viewLufsS = s.lufsS; s.viewLufsI = s.lufsI; s.viewTruePeakDb = s.truePeakDb; s.viewPsr = s.psr;
        if (soloView != kViewStereo) {
            float soloHold = soloSlotPeak;
            for (float p : soloRing) soloHold = std::max(soloHold, p);
            s.viewLufsS = soloLoud.shortTerm(); s.viewLufsI = soloLoud.integrated();
            s.viewTruePeakDb = toDb(soloHold);
            s.viewPsr = active ? s.viewTruePeakDb - s.viewLufsS : 0.0f;
        }

        double totalShort = 0.0, totalLong = 0.0;
        for (int i = 0; i < kBands; ++i) { totalShort += bandShortP[i]; totalLong += bandLongP[i]; }
        for (int i = 0; i < kBands; ++i) {
            s.bandShort[i] = totalShort > 1e-14 ? powDb(bandShortP[i] / totalShort) : -120.0f;
            s.bandLong[i]  = totalLong  > 1e-14 ? powDb(bandLongP[i] / totalLong)   : -120.0f;
        }
        auto pointDb = [this](const std::vector<float>& bins, int j) {
            const Point& p = points[j];
            float v;
            if (p.interpolate) {
                const int k = (int) p.pos;
                const float t = p.pos - (float) k;
                v = bins[(size_t) k] * (1.0f - t) + bins[(size_t) k + 1] * t;
            } else {
                v = 0.0f;
                for (int k = p.lo; k <= p.hi; ++k) v = std::max(v, bins[(size_t) k]);
            }
            return powDb(v);
        };
        const std::vector<float>& viewBins = soloView == kViewStereo ? binSmooth : viewSmooth[soloView];
        for (int j = 0; j < kSpecPoints; ++j) {
            s.spectrum[j] = pointDb(binSmooth, j);
            s.viewSpectrum[j] = pointDb(viewBins, j);
        }

        // Never wait on the reader: if it holds the lock, publish on the next round.
        std::unique_lock<std::mutex> lock(snapLock, std::try_to_lock);
        if (lock.owns_lock()) { shared = s; dirty = false; }
    }

    double fs = 48000.0;
    int N = 0, hop = 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> window, fifoL, fifoR, re, im, re2, im2, framePow, binSmooth, viewSmooth[kViews], scopeL, scopeR;
    std::vector<MapEntry> bandMap;
    Point points[kSpecPoints] = {};
    float powScale = 1.0f, specCoef = 0.1f, shortCoef = 0.1f;
    double longCoef = 0.01, secondCoef = 0.0, dcCoef = 0.0;
    int fifoPos = 0, hopCount = 0;

    Loudness loud;
    TruePeak tp[2];
    // Loudness and peak of the one channel being viewed; unused while the view is the whole mix.
    Loudness soloLoud;
    TruePeak soloTp;
    std::atomic<int> view { kViewStereo };
    int soloView = kViewStereo;
    float soloRing[kRing] = {}, soloSlotPeak = 0.0f;
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

    std::vector<WaveBucket> wave[kViews];
    std::atomic<long long> waveTotal { 0 };
    int waveLen = 12, waveFill = 0;
    float waveLowCoef = 0.0f, waveHighCoef = 0.0f, waveMidPeak = 0.0f;
    WaveAcc waveAcc[kAccs] = {};
    mutable std::mutex snapLock;
    Snapshot shared {};
};

} // namespace snitch
