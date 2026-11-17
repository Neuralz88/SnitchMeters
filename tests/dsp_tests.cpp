// Console tests for dsp/. No framework: each CHECK prints on failure, main returns the failure count.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include "dsp/Analyser.h"
#include "dsp/ConcussionProfile.h"
#include "dsp/Issues.h"
#include "dsp/ProfileBuilder.h"

using namespace snitch;

static int failures = 0;
static const char* current = "";
#define CHECK(cond) do { if (! (cond)) { ++failures; std::printf("FAIL %s: %s (line %d)\n", current, #cond, __LINE__); } } while (0)
#define CHECK_NEAR(a, b, tol) do { const double va = (a), vb = (b); if (! (std::fabs(va - vb) <= (tol))) { ++failures; \
    std::printf("FAIL %s: %s = %g, expected %g +- %g (line %d)\n", current, #a, va, vb, (double) (tol), __LINE__); } } while (0)
#define TEST(name) static void name(); struct name##_reg { name##_reg() { tests().push_back({ #name, name }); } } name##_instance; static void name()

struct TestCase { const char* name; void (*fn)(); };
static std::vector<TestCase>& tests() { static std::vector<TestCase> t; return t; }

static const double kPi = 3.141592653589793;

static std::vector<float> sine(double freq, double amp, double sr, double seconds, double phase = 0.0) {
    std::vector<float> v((size_t) (sr * seconds));
    for (size_t i = 0; i < v.size(); ++i) v[i] = (float) (amp * std::sin(2.0 * kPi * freq * (double) i / sr + phase));
    return v;
}

static std::vector<float> pinkNoise(double seconds, double sr, unsigned seed, float gain = 0.1f) {
    // Paul Kellet's pink filter over an LCG white source.
    std::vector<float> v((size_t) (sr * seconds));
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, hp = 0, last = 0;
    unsigned state = seed;
    for (float& out : v) {
        state = state * 1664525u + 1013904223u;
        const double white = (double) (state >> 8) / 8388608.0 - 1.0;
        b0 = 0.99886 * b0 + white * 0.0555179; b1 = 0.99332 * b1 + white * 0.0750759;
        b2 = 0.96900 * b2 + white * 0.1538520; b3 = 0.86650 * b3 + white * 0.3104856;
        b4 = 0.55000 * b4 + white * 0.5329522; b5 = -0.7616 * b5 - white * 0.0168980;
        const double pink = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362) * 0.11;
        b6 = white * 0.115926;
        // 7 Hz DC blocker, like any real master: raw 1/f noise wanders like an offset.
        hp = pink - last + 0.999 * hp; last = pink;
        out = (float) hp * gain * 10.0f;
    }
    return v;
}

static void feed(Analyser& a, const std::vector<float>& L, const std::vector<float>& R, int block = 512) {
    for (size_t pos = 0; pos < L.size(); pos += (size_t) block) {
        const int n = (int) std::min((size_t) block, L.size() - pos);
        a.process(L.data() + pos, R.data() + pos, n);
    }
}

static int loudestBand(const float* bands) {
    int best = 0;
    for (int i = 1; i < kBands; ++i) if (bands[i] > bands[best]) best = i;
    return best;
}

static bool allFinite(const Snapshot& s) {
    bool ok = std::isfinite(s.lufsM) && std::isfinite(s.lufsS) && std::isfinite(s.lufsI) && std::isfinite(s.truePeakDb)
           && std::isfinite(s.truePeakMaxDb) && std::isfinite(s.corr) && std::isfinite(s.corrLow) && std::isfinite(s.dc)
           && std::isfinite(s.psr) && std::isfinite(s.balanceDb);
    for (float v : s.bandShort) ok = ok && std::isfinite(v);
    for (float v : s.bandLong)  ok = ok && std::isfinite(v);
    for (float v : s.spectrum)  ok = ok && std::isfinite(v);
    return ok;
}

// ---------------------------------------------------------------- primitives

TEST(fft_bin) {
    Fft fft(10);
    std::vector<float> re(1024), im(1024, 0.0f);
    for (int i = 0; i < 1024; ++i) re[(size_t) i] = (float) std::sin(2.0 * kPi * 64.0 * i / 1024.0);
    fft.forward(re.data(), im.data());
    int best = 0; float bestMag = 0.0f;
    for (int k = 0; k < 512; ++k) {
        const float m = re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k];
        if (m > bestMag) { bestMag = m; best = k; }
    }
    CHECK(best == 64);
    CHECK_NEAR(std::sqrt(bestMag), 512.0, 0.5);
}

TEST(loudness_997hz) {
    for (double sr : { 48000.0, 44100.0 }) {
        const auto x = sine(997.0, std::pow(10.0, -23.0 / 20.0), sr, 6.0);
        Loudness l; l.prepare(sr);
        l.process(x.data(), x.data(), (int) x.size());
        CHECK_NEAR(l.momentary(), -23.0, 0.3);
        CHECK_NEAR(l.shortTerm(), -23.0, 0.3);
        CHECK_NEAR(l.integrated(), -23.0, 0.3);
    }
}

TEST(loudness_silence) {
    std::vector<float> zero(48000, 0.0f);
    Loudness l; l.prepare(48000.0);
    l.process(zero.data(), zero.data(), (int) zero.size());
    CHECK(l.momentary() <= -100.0f);
    CHECK(l.integrated() <= -100.0f);
    CHECK(std::isfinite(l.shortTerm()));
}

TEST(truepeak_intersample) {
    // fs/4 at 45 degrees: every sample is 0.707 but the waveform reaches 1.0 between them.
    const auto x = sine(12000.0, 1.0, 48000.0, 0.5, kPi / 4.0);
    float samplePeak = 0.0f;
    for (float v : x) samplePeak = std::max(samplePeak, std::fabs(v));
    CHECK_NEAR(20.0 * std::log10(samplePeak), -3.01, 0.05);
    TruePeak tp; tp.prepare();
    CHECK_NEAR(20.0 * std::log10(tp.process(x.data(), (int) x.size())), 0.0, 0.3);
}

// ------------------------------------------------------------------ analyser

TEST(sine_band_at_rates) {
    for (double sr : { 44100.0, 48000.0, 96000.0 }) {
        const auto x = sine(1000.0, 0.25, sr, 3.0);
        Analyser a; a.prepare(sr);
        feed(a, x, x);
        CHECK(loudestBand(a.snapshot().bandShort) == 17);
        // Full-scale-referenced display: the 1 kHz peak reads about -12 dBFS.
        float top = -200.0f;
        for (float v : a.snapshot().spectrum) top = std::max(top, v);
        CHECK_NEAR(top, -12.04, 1.0);
    }
}

TEST(pink_is_flat) {
    const auto l = pinkNoise(30.0, 44100.0, 1u), r = pinkNoise(30.0, 44100.0, 2u);
    Analyser a; a.prepare(44100.0);
    feed(a, l, r);
    const Snapshot s = a.snapshot();
    CHECK(s.longValid);
    float lo = 1e9f, hi = -1e9f;
    for (int i = 4; i <= 27; ++i) { lo = std::min(lo, s.bandLong[i]); hi = std::max(hi, s.bandLong[i]); }  // 50 Hz .. 10 kHz
    CHECK(hi - lo < 3.0f);
}

TEST(correlation) {
    const double sr = 48000.0;
    const auto x = sine(440.0, 0.3, sr, 4.0);
    std::vector<float> inv(x.size());
    for (size_t i = 0; i < x.size(); ++i) inv[i] = -x[i];
    { Analyser a; a.prepare(sr); feed(a, x, x);   CHECK(a.snapshot().corr > 0.98f); }
    { Analyser a; a.prepare(sr); feed(a, x, inv); CHECK(a.snapshot().corr < -0.98f); }

    // Sub out of phase, tops in phase: only the low-band reading should go negative.
    const auto sub = sine(50.0, 0.3, sr, 4.0), top = sine(3000.0, 0.3, sr, 4.0);
    std::vector<float> l(sub.size()), r(sub.size());
    for (size_t i = 0; i < sub.size(); ++i) { l[i] = sub[i] + top[i]; r[i] = -sub[i] + top[i]; }
    Analyser a; a.prepare(sr); feed(a, l, r);
    CHECK(a.snapshot().corrLow < -0.8f);
    CHECK(std::fabs(a.snapshot().corr) < 0.2f);
}

TEST(dc_offset) {
    auto x = sine(200.0, 0.2, 48000.0, 25.0);
    for (float& v : x) v += 0.02f;
    Analyser a; a.prepare(48000.0); feed(a, x, x);
    CHECK_NEAR(a.snapshot().dc, 0.02, 0.002);
}

TEST(clip_runs) {
    const double sr = 48000.0;
    std::vector<float> square((size_t) sr * 2);
    for (size_t i = 0; i < square.size(); ++i) square[i] = (i / 240) % 2 ? 1.0f : -1.0f;
    { Analyser a; a.prepare(sr); feed(a, square, square); CHECK(a.snapshot().clipRuns > 0); }
    const auto quiet = sine(100.0, 0.5, sr, 2.0);
    { Analyser a; a.prepare(sr); feed(a, quiet, quiet); CHECK(a.snapshot().clipRuns == 0); }
}

TEST(silence_and_nan) {
    std::vector<float> zero(48000, 0.0f), bad(4800, std::numeric_limits<float>::quiet_NaN());
    bad[10] = std::numeric_limits<float>::infinity();
    Analyser a; a.prepare(48000.0);
    feed(a, zero, zero);
    feed(a, bad, bad);
    feed(a, zero, zero);
    const Snapshot s = a.snapshot();
    CHECK(! s.active);
    CHECK(allFinite(s));

    IssueDetector d; Issue issues[32];
    RefProfile ref {}; ref.valid = true;
    int n = 0;
    for (int i = 0; i < 100; ++i) n = d.update(s, ref, 0.05, issues, 32);
    CHECK(n == 0);
}

TEST(mono_input) {
    const auto x = sine(440.0, 0.3, 48000.0, 2.0);
    Analyser a; a.prepare(48000.0);
    a.process(x.data(), nullptr, (int) x.size());
    CHECK(a.snapshot().corr > 0.98f);
    CHECK(a.snapshot().active);
}

TEST(block_size_invariance) {
    const auto l = pinkNoise(10.0, 44100.0, 7u), r = pinkNoise(10.0, 44100.0, 8u);
    Snapshot ref {};
    bool first = true;
    for (int block : { 512, 1, 100000 }) {
        Analyser a; a.prepare(44100.0);
        feed(a, l, r, block);
        const Snapshot s = a.snapshot();
        if (first) { ref = s; first = false; continue; }
        CHECK_NEAR(s.lufsS, ref.lufsS, 0.05);
        CHECK_NEAR(s.truePeakMaxDb, ref.truePeakMaxDb, 0.05);
        for (int i = 0; i < kBands; ++i) CHECK_NEAR(s.bandLong[i], ref.bandLong[i], 0.1);
    }
}

TEST(reprepare_changes_rate) {
    Analyser a; a.prepare(44100.0);
    const auto x44 = sine(1000.0, 0.25, 44100.0, 2.0);
    feed(a, x44, x44);
    a.prepare(96000.0);
    const auto x96 = sine(4000.0, 0.25, 96000.0, 2.0);
    feed(a, x96, x96);
    CHECK(loudestBand(a.snapshot().bandShort) == 23);  // 4 kHz
}

// -------------------------------------------------------------------- issues

static RefProfile flatRef() {
    RefProfile ref {};
    std::strcpy(ref.name, "flat");
    ref.valid = true;
    for (int i = 0; i < kBands; ++i) { ref.band[i] = -15.0f; ref.bandLo[i] = -17.0f; ref.bandHi[i] = -13.0f; }
    ref.lufs = -8.0f; ref.truePeakDb = 0.0f; ref.psr = 7.0f; ref.corrLow = 0.95f;
    return ref;
}

static Snapshot matchingSnapshot(const RefProfile& ref) {
    Snapshot s {};
    s.active = true; s.longValid = true; s.judging = true;
    s.lufsM = s.lufsS = s.lufsI = ref.lufs;
    s.truePeakDb = s.truePeakMaxDb = -0.5f;
    s.corr = 0.6f; s.corrLow = 0.95f; s.psr = ref.psr; s.activeSeconds = 60.0f;
    for (int i = 0; i < kBands; ++i) s.bandShort[i] = s.bandLong[i] = ref.band[i] - 9.0f;  // quieter overall: must not matter
    return s;
}

static bool has(const Issue* issues, int n, int id) {
    for (int i = 0; i < n; ++i) if (issues[i].id == id) return true;
    return false;
}

static int run(IssueDetector& d, const Snapshot& s, const RefProfile& ref, double seconds, Issue* out) {
    int n = 0;
    for (double t = 0.0; t < seconds - 1e-9; t += 0.05) n = d.update(s, ref, 0.05, out, 32);
    return n;
}

TEST(matching_ref_no_issues) {
    const RefProfile ref = flatRef();
    IssueDetector d; Issue issues[32];
    CHECK(run(d, matchingSnapshot(ref), ref, 10.0, issues) == 0);
}

TEST(sub_hot_latches) {
    const RefProfile ref = flatRef();
    const Snapshot good = matchingSnapshot(ref);
    Snapshot hot = good;
    for (int i = 0; i < kBands; ++i) if (regionOfBand(i) == 0) hot.bandLong[i] += 7.0f;

    IssueDetector d; Issue issues[32];
    int n = run(d, hot, ref, 0.9, issues);
    CHECK(n == 0);                                   // not yet: needs a full second
    n = run(d, hot, ref, 0.2, issues);
    CHECK(n == 1);                                   // only the sub, nothing reads "thin" because of it
    CHECK(has(issues, n, IssueDetector::kHot + 0));
    CHECK(issues[0].severity == 2);
    CHECK(std::strstr(issues[0].text, "Sub") != nullptr);
    CHECK(std::strstr(issues[0].text, "+") != nullptr);
    CHECK(issues[0].freqLo == 20.0f && issues[0].freqHi == 60.0f);

    n = run(d, good, ref, 1.9, issues);
    CHECK(has(issues, n, IssueDetector::kHot + 0));  // lingers for 2 s
    n = run(d, good, ref, 0.2, issues);
    CHECK(n == 0);
}

TEST(thin_region_text) {
    const RefProfile ref = flatRef();
    Snapshot s = matchingSnapshot(ref);
    for (int i = 0; i < kBands; ++i) if (regionOfBand(i) == 5) s.bandLong[i] -= 4.0f;
    IssueDetector d; Issue issues[32];
    const int n = run(d, s, ref, 1.5, issues);
    CHECK(n == 1);
    CHECK(has(issues, n, IssueDetector::kThin + 5));
    CHECK(issues[0].severity == 1);
    CHECK(std::strstr(issues[0].text, "Air -") != nullptr);
}

TEST(inactive_raises_nothing) {
    const RefProfile ref = flatRef();
    Snapshot s = matchingSnapshot(ref);
    s.active = false;
    s.clipRuns = 50; s.corrLow = -1.0f; s.corr = -1.0f; s.dc = 0.5f; s.truePeakDb = 3.0f; s.balanceDb = 9.0f; s.psr = 0.0f;
    for (int i = 0; i < 6; ++i) s.bandLong[i] += 12.0f;
    IssueDetector d; Issue issues[32];
    CHECK(run(d, s, ref, 5.0, issues) == 0);
}

TEST(each_plain_issue) {
    const RefProfile ref = flatRef();
    const Snapshot good = matchingSnapshot(ref);
    struct Case { int id; void (*breakIt)(Snapshot&); };
    const Case cases[] = {
        { IssueDetector::kClipping,  [](Snapshot& s) { s.clipRuns = 30; } },
        { IssueDetector::kOver,      [](Snapshot& s) { s.overs = 2; } },
        { IssueDetector::kTruePeak,  [](Snapshot& s) { s.truePeakDb = 1.2f; } },
        { IssueDetector::kBassPhase, [](Snapshot& s) { s.corrLow = -0.3f; } },
        { IssueDetector::kMixPhase,  [](Snapshot& s) { s.corr = -0.5f; } },
        { IssueDetector::kSquashed,  [](Snapshot& s) { s.psr = 3.0f; } },
        { IssueDetector::kDc,        [](Snapshot& s) { s.dc = 0.05f; } },
        { IssueDetector::kLean,      [](Snapshot& s) { s.balanceDb = -2.5f; } },
        { IssueDetector::kLoudness,  [](Snapshot& s) { s.lufsI = -14.0f; s.lufsM = -14.0f; } },
    };
    for (const Case& c : cases) {
        Snapshot s = good; c.breakIt(s);
        IssueDetector d; Issue issues[32];
        const int n = run(d, s, ref, 1.5, issues);
        CHECK(n == 1);
        CHECK(has(issues, n, c.id));
    }
}

TEST(invalid_ref_skips_comparisons) {
    RefProfile none {};
    Snapshot s = matchingSnapshot(flatRef());
    s.psr = 1.0f; s.lufsI = -30.0f; s.lufsM = -30.0f;
    for (int i = 0; i < 6; ++i) s.bandLong[i] += 12.0f;
    IssueDetector d; Issue issues[32];
    CHECK(run(d, s, none, 3.0, issues) == 0);
}

// ------------------------------------------------------------------- profile

TEST(profile_of_pink) {
    const auto l = pinkNoise(60.0, 44100.0, 3u), r = l;  // mono: independent noise would rightly read as phasey bass
    const RefProfile p = buildProfile(l.data(), r.data(), l.size(), 44100.0, "pink");
    CHECK(p.valid);
    CHECK(std::strcmp(p.name, "pink") == 0);
    float lo = 1e9f, hi = -1e9f;
    for (int i = 4; i <= 27; ++i) { lo = std::min(lo, p.band[i]); hi = std::max(hi, p.band[i]); }
    CHECK(hi - lo < 3.0f);
    for (int i = 0; i < kBands; ++i) CHECK(p.bandLo[i] <= p.band[i] && p.band[i] <= p.bandHi[i]);
    CHECK(std::isfinite(p.lufs) && p.lufs > -60.0f && p.lufs < 0.0f);
    CHECK(p.psr > 3.0f && p.psr < 25.0f);

    // The meter must not flag the track it was profiled from.
    Analyser a; a.prepare(44100.0);
    IssueDetector d; Issue issues[32];
    int worst = 0;
    for (size_t pos = 0; pos + 2048 <= l.size(); pos += 2048) {
        a.process(l.data() + pos, r.data() + pos, 2048);
        const int n = d.update(a.snapshot(), p, 2048.0 / 44100.0, issues, 32);
        if (n > worst) std::printf("     at %.1f s: %s\n", (double) pos / 44100.0, issues[0].text);
        worst = std::max(worst, n);
    }
    CHECK(worst == 0);
}

TEST(short_input_invalid) {
    const auto x = pinkNoise(2.0, 44100.0, 5u);
    CHECK(! buildProfile(x.data(), x.data(), x.size(), 44100.0, "short").valid);
    CHECK(! buildProfile(x.data(), x.data(), 0, 44100.0, "empty").valid);
}

TEST(baked_profile_sane) {
    const RefProfile& p = concussionProfile();
    CHECK(p.valid);
    CHECK(std::strstr(p.name, "Concussion") != nullptr);
    CHECK(p.lufs > -14.0f && p.lufs < -3.0f);
    CHECK(p.psr > 2.0f && p.psr < 15.0f);
    for (int i = 0; i < kBands; ++i) {
        CHECK(std::isfinite(p.band[i]) && p.band[i] <= 0.0f);
        CHECK(p.bandLo[i] <= p.band[i] && p.band[i] <= p.bandHi[i]);
    }
}

TEST(wave_history_shape_and_colour) {
    Analyser a; a.prepare(48000.0);
    const auto sub = sine(50.0, 0.5, 48000.0, 1.0), hat = sine(8000.0, 0.25, 48000.0, 1.0);
    feed(a, sub, sub);
    const long long afterSub = a.waveCount();
    CHECK_NEAR((double) afterSub, a.waveBucketsPerSecond(), 2.0);
    feed(a, hat, hat);
    const long long total = a.waveCount();

    std::vector<WaveBucket> b((size_t) total);
    a.copyWave(b.data(), 0, (int) total);
    float lo = 0.0f, hi = 0.0f, peak = 0.0f;
    for (long long i = afterSub / 2; i < afterSub; ++i) {
        lo = std::min(lo, b[(size_t) i].lo); hi = std::max(hi, b[(size_t) i].hi); peak = std::max(peak, b[(size_t) i].peak);
        CHECK(b[(size_t) i].low > 4.0f * b[(size_t) i].mid && b[(size_t) i].low > 4.0f * b[(size_t) i].high);
    }
    CHECK_NEAR(lo, -0.5, 0.01); CHECK_NEAR(hi, 0.5, 0.01); CHECK_NEAR(peak, 0.5, 0.01);
    for (long long i = afterSub + (total - afterSub) / 2; i < total; ++i)
        CHECK(b[(size_t) i].high > 4.0f * b[(size_t) i].mid && b[(size_t) i].high > 4.0f * b[(size_t) i].low);

    // Outside what exists: zeroed, not garbage.
    WaveBucket edge[4];
    a.copyWave(edge, total - 2, 4);
    CHECK(edge[1].peak > 0.2f && edge[2].peak == 0.0f && edge[3].peak == 0.0f);
    a.copyWave(edge, -2, 4);
    CHECK(edge[0].peak == 0.0f && edge[1].peak == 0.0f && edge[2].peak > 0.0f);
}

TEST(wave_history_wraps) {
    Analyser a; a.prepare(48000.0);
    const auto x = sine(100.0, 0.3, 48000.0, 40.0);   // longer than the history
    feed(a, x, x);
    const long long total = a.waveCount();
    CHECK(total > Analyser::kWave);
    WaveBucket old[2], recent[2];
    a.copyWave(old, total - Analyser::kWave - 2, 2);
    a.copyWave(recent, total - Analyser::kWave, 2);
    CHECK(old[0].peak == 0.0f && old[1].peak == 0.0f);
    CHECK(recent[0].peak > 0.1f && recent[1].peak > 0.1f);
}

// ---------------------------------------------------------------- channel views

TEST(view_waveforms) {
    Analyser a; a.prepare(48000.0);
    const auto x = sine(100.0, 0.5, 48000.0, 1.0), none = std::vector<float>(x.size(), 0.0f);
    feed(a, x, none);   // left only
    const long long total = a.waveCount();
    const float expected[kViews] = { 0.25f, 0.5f, 0.0f, 0.25f, 0.25f };   // the whole mix is drawn as its mono sum
    const float peaks[kViews]    = { 0.5f,  0.5f, 0.0f, 0.25f, 0.25f };
    for (int v = 0; v < kViews; ++v) {
        std::vector<WaveBucket> b((size_t) total / 2);
        a.copyWave(b.data(), total / 2, (int) b.size(), v);
        float hi = 0.0f, peak = 0.0f;
        for (const WaveBucket& bucket : b) { hi = std::max(hi, bucket.hi); peak = std::max(peak, bucket.peak); }
        CHECK_NEAR(hi, expected[v], 0.01);
        CHECK_NEAR(peak, peaks[v], 0.01);
    }
}

TEST(view_spectrum_mid_side) {
    const auto x = sine(1000.0, 0.5, 48000.0, 2.0);
    std::vector<float> flipped(x.size());
    for (size_t i = 0; i < x.size(); ++i) flipped[i] = -x[i];
    auto peakOf = [](const float* spectrum) { float top = -200.0f; for (int j = 0; j < kSpecPoints; ++j) top = std::max(top, spectrum[j]); return top; };

    for (int v = 0; v < kViews; ++v) {
        Analyser mono; mono.prepare(48000.0); mono.setView(v);
        feed(mono, x, x);
        const Snapshot s = mono.snapshot();
        CHECK(s.view == v);
        if (v == kViewSide) CHECK(peakOf(s.viewSpectrum) < peakOf(s.spectrum) - 60.0f);
        else CHECK_NEAR(peakOf(s.viewSpectrum), peakOf(s.spectrum), 0.01);

        Analyser wide; wide.prepare(48000.0); wide.setView(v);
        feed(wide, x, flipped);
        const Snapshot w = wide.snapshot();
        if (v == kViewMid) CHECK(peakOf(w.viewSpectrum) < peakOf(w.spectrum) - 60.0f);
        else CHECK_NEAR(peakOf(w.viewSpectrum), peakOf(w.spectrum), 0.01);
    }
}

TEST(view_levels) {
    const auto loudSide = sine(997.0, 0.5, 48000.0, 5.0), quietSide = sine(997.0, 0.125, 48000.0, 5.0);
    Analyser both; both.prepare(48000.0);
    feed(both, loudSide, loudSide);
    const Snapshot whole = both.snapshot();
    CHECK_NEAR(whole.viewLufsS, whole.lufsS, 1e-6);   // the whole mix: the same numbers
    CHECK_NEAR(whole.viewTruePeakDb, whole.truePeakDb, 1e-6);

    // One channel reads as that channel on both speakers.
    Analyser a; a.prepare(48000.0); a.setView(kViewLeft);
    feed(a, loudSide, quietSide);
    Snapshot s = a.snapshot();
    CHECK_NEAR(s.viewLufsS, whole.lufsS, 0.05);
    CHECK_NEAR(s.viewLufsI, whole.lufsI, 0.05);
    CHECK_NEAR(s.viewTruePeakDb, whole.truePeakDb, 0.05);
    CHECK_NEAR(s.viewPsr, whole.psr, 0.1);

    a.setView(kViewRight);   // 12 dB down; the numbers start again
    feed(a, loudSide, quietSide);
    s = a.snapshot();
    CHECK_NEAR(s.viewLufsS, whole.lufsS - 12.04, 0.05);
    CHECK_NEAR(s.viewTruePeakDb, whole.truePeakDb - 12.04, 0.05);

    a.setView(kViewSide);    // (0.5 - 0.125) / 2 = 0.1875: 8.52 dB down
    feed(a, loudSide, quietSide);
    s = a.snapshot();
    CHECK_NEAR(s.viewLufsS, whole.lufsS - 8.52, 0.05);
    CHECK(allFinite(s));
}

int main(int argc, char** argv) {
    for (const TestCase& t : tests()) {
        if (argc > 1 && std::strstr(t.name, argv[1]) == nullptr) continue;
        current = t.name;
        const int before = failures;
        t.fn();
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
