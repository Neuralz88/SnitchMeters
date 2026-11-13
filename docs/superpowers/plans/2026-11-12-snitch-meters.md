# Snitch Meters Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A desktop meter (standalone + VST3) with oscilloscope, spectrum and plain-word issue callouts measured against Noisia - Concussion (Mefjus Remix).

**Architecture:** Header-only, JUCE-free analysis code in `dsp/` shared by three consumers: a console test exe, a console profiling tool, and the JUCE plugin. The standalone is a custom JUCE app (no audio device) that feeds the processor's analyser from a WASAPI loopback thread; the VST3 feeds it from `processBlock`.

**Tech Stack:** C++17, CMake, JUCE 8.0.4 (FetchContent), VS 2022, WASAPI, ffmpeg (profiling script only).

**Spec:** `docs/superpowers/specs/2026-11-12-snitch-meters-design.md`

## Global Constraints

- Windows only. `dsp/` includes no JUCE and no Windows headers.
- The reference audio is never copied into the repo or binary; only its profile.
- VST3 passes audio through untouched.
- Every issue flag: raise after 1.0 s continuously true, clear after 2.0 s continuously false at a relaxed threshold.
- Balance comparison is level-independent (band dB relative to total band power).
- No six-pointed / six-way-symmetric visuals (user hard rule); plain rectangular meter UI.

## Review Focus

1. Silence, NaN or inf input → no issues, no NaN in snapshot, "no signal" state. (Task 2 test `silence_and_nan`)
2. Sample-rate change (44.1 → 48 → 96 k) → `prepare()` again, bands still land on the right frequencies. (Task 2 test `sine_band_at_rates`)
3. Blocks of 1 sample and of 100 000 samples → same result as 512. (Task 2 test `block_size_invariance`)
4. Default output device changes or disappears while capturing → capture thread reopens the new default, never crashes. (Task 5, manual check + code path)
5. Dropped file is not audio / is 2 s long → reference unchanged, message shown. (Task 6, `buildProfile` returns `valid=false` under 10 s of gated audio; Task 3 test `short_input_invalid`)

---

## File map

| File | Responsibility |
|---|---|
| `dsp/Fft.h` | radix-2 complex FFT, Hann window |
| `dsp/Loudness.h` | BS.1770 K-weighting, momentary / short-term / gated integrated |
| `dsp/TruePeak.h` | 4x polyphase FIR peak |
| `dsp/Bands.h` | 31 third-octave centres 20 Hz–20 kHz, bin→band mapping, region table |
| `dsp/Profile.h` | `RefProfile`, `Snapshot`, `Issue` structs |
| `dsp/Analyser.h` | everything live: feeds the above, scope ring, publishes `Snapshot` |
| `dsp/ProfileBuilder.h` | offline: whole track → `RefProfile` (uses `Analyser`) |
| `dsp/Issues.h` | `IssueDetector` with latching |
| `dsp/ConcussionProfile.h` | generated default profile |
| `tests/dsp_tests.cpp` | console tests |
| `tools/profile_ref.cpp`, `tools/make_profile.ps1` | raw f32 → profile header |
| `plugin/PluginProcessor.*` | owns analyser, detector, reference, state |
| `plugin/PluginEditor.*` | layout, timer, drag-drop, menu |
| `plugin/Views.h` | `ScopeView`, `SpectrumView`, `StatStrip`, `IssueList` |
| `plugin/LoopbackCapture.*` | WASAPI loopback thread |
| `plugin/StandaloneApp.cpp` | custom always-on-top standalone window |

## Shared types (`dsp/Profile.h`) — every task uses these names

```cpp
namespace snitch {
constexpr int kBands = 31;        // third octaves, 20 Hz .. 20 kHz
constexpr int kSpecPoints = 240;  // log-spaced display points, 20 Hz .. 20 kHz
constexpr int kRegions = 6;       // Sub 20-60, Bass 60-150, LowMid 150-400, Mid 400-2k, Presence 2k-6k, Air 6k-20k

struct RefProfile {
    char  name[96];
    bool  valid;
    float band[kBands];     // dB, relative to total band power, mean of loud sections
    float bandLo[kBands];   // 10th percentile of 3 s windows
    float bandHi[kBands];   // 90th percentile
    float lufs;             // integrated
    float truePeakDb;
    float psr;              // median (peak - short-term LUFS) over loud sections
    float corrLow;          // median correlation < 150 Hz
};

struct Snapshot {
    bool  active;           // momentary > -50 LUFS
    bool  longValid;        // >= 5 s of active audio in the long average
    float lufsM, lufsS, lufsI;
    float truePeakDb;       // 3 s hold
    float truePeakMaxDb;    // since reset
    int   clipRuns;         // runs of >= 3 samples at |x| >= 0.999, last 3 s
    float corr, corrLow;    // -1..1
    float dc;               // max |mean| of L, R
    float psr;              // 3 s peak - short-term LUFS
    float balanceDb;        // L - R, long-term; + = left
    float bandShort[kBands], bandLong[kBands];   // dB relative to total
    float spectrum[kSpecPoints];                 // dBFS, ~300 ms average
};

struct Issue { int id; int severity; /*0 info 1 warn 2 bad*/ char text[96]; float freqLo, freqHi; /*0,0 if not spectral*/ };
}
```

---

### Task 1: Scaffold + primitives (FFT, loudness, true peak)

**Files:** Create `CMakeLists.txt`, `.gitignore`, `dsp/Fft.h`, `dsp/Loudness.h`, `dsp/TruePeak.h`, `dsp/Profile.h`, `tests/dsp_tests.cpp`

**Interfaces — Produces:**
- `snitch::Fft(int order)`; `void forward(float* re, float* im)`; `static void hann(float* w, int n)`
- `snitch::Loudness`: `void prepare(double sr)`, `void process(const float* L, const float* R, int n)`, `float momentary()`, `float shortTerm()`, `float integrated()`, `void reset()`; all return LUFS, `-120` when silent
- `snitch::TruePeak`: `void prepare()`, `float process(const float* x, int n)` → linear max of this block (4x oversampled, 48-tap polyphase)

- [ ] Write tests: `loudness_997hz` (stereo 997 Hz sine at −23 dBFS peak → momentary, short-term and integrated all −23.0 ± 0.3 LUFS at 48 k and 44.1 k); `loudness_silence` (→ ≤ −100, no NaN); `truepeak_intersample` (fs/4 sine, 45° phase, sample peak −3.01 dB → true peak 0.0 ± 0.3 dB); `fft_bin` (bin 64 sine in 1024 → magnitude peak at bin 64).
- [ ] CMake target `dsp_tests` (plain exe, `/W4`), configured with `-DSNITCH_BUILD_PLUGIN=OFF` so it builds without fetching JUCE. Run: fails to compile.
- [ ] Implement the three headers. K-weighting coefficients are computed per sample rate from the BS.1770 analogue prototypes (shelf 1681.97 Hz, +3.9998 dB, Q 0.7072; high-pass 38.135 Hz, Q 0.5003), not hard-coded for 48 k.
- [ ] Run `build-tests/Release/dsp_tests.exe` → `ALL PASS`. Commit.

### Task 2: Live analyser

**Files:** Create `dsp/Bands.h`, `dsp/Analyser.h`; extend `tests/dsp_tests.cpp`

**Interfaces — Produces:**
- `snitch::bandCentre(int i)`, `snitch::regionName(int r)`, `snitch::regionRange(int r, float& lo, float& hi)`, `snitch::regionOfBand(int i)`
- `snitch::Analyser`: `void prepare(double sr)`, `void reset()`, `void process(const float* L, const float* R /*may be null = mono*/, int n)`, `Snapshot snapshot() const`, `int copyScope(float* dstL, float* dstR, int n) const` (most recent n samples), `double sampleRate() const`
- `process` is called on one thread; `snapshot`/`copyScope` from another. Publishing uses a try-lock on the audio side so `process` never blocks.

Values: FFT 4096 (8192 above 50 kHz), hop = size/4, Hann. Short average 300 ms, long average 8 s time constant, long average frozen while not `active`. Low band for `corrLow`: 2nd-order low-pass at 150 Hz on each channel. Correlation and DC integrate over 1 s. Scope ring 32768 samples. Non-finite input samples are replaced with 0.

- [ ] Write tests: `sine_band_at_rates` (1 kHz sine at 44.1 / 48 / 96 k → loudest `bandShort` is the 1 kHz band); `pink_is_flat` (30 s pink noise → `bandLong` 50 Hz–10 kHz within ± 1.5 dB of each other); `correlation` (L=R → corr > 0.98; L=−R → < −0.98; 50 Hz antiphase + 3 kHz in-phase at equal level → corrLow < −0.8); `dc_offset` (0.02 offset → dc 0.02 ± 0.002); `clip_runs` (square at ±1.0 → clipRuns > 0; −6 dB sine → 0); `silence_and_nan` (zeros then NaN block → `active` false, every float field finite); `block_size_invariance` (same 10 s signal in blocks of 1, 512, 100 000 → lufsS within 0.05, bandLong within 0.1).
- [ ] Run → fail. Implement. Run → `ALL PASS`. Commit.

### Task 3: Profile builder, issue detector, tool, baked profile

**Files:** Create `dsp/ProfileBuilder.h`, `dsp/Issues.h`, `tools/profile_ref.cpp`, `tools/make_profile.ps1`, `dsp/ConcussionProfile.h` (generated); extend tests

**Interfaces — Produces:**
- `RefProfile snitch::buildProfile(const float* L, const float* R, size_t n, double sr, const char* name)` — runs an `Analyser` over the track in 3 s windows; keeps windows whose short-term loudness ≥ integrated − 6 LU ("loud sections"); `valid=false` if fewer than 10 s kept.
- `snitch::IssueDetector`: `void reset()`, `int update(const Snapshot&, const RefProfile&, double dt, Issue* out, int maxOut)` → count, sorted severity-descending
- `const snitch::RefProfile& snitch::concussionProfile()`
- `profile_ref <in.f32> <sampleRate> <name> <out.h>`; `make_profile.ps1 -Audio <file>` decodes with ffmpeg to the scratch dir, runs the tool, writes `dsp/ConcussionProfile.h`.

Issue table (raise threshold / clear threshold):

| id | condition | severity | text |
|---|---|---|---|
| 0–5 region hot | region mean(bandLong − ref.band) > +3 / +2 dB; > +6 = bad | warn/bad | `Sub +4.2 dB vs ref` (region name) |
| 10–15 region thin | < −3 / −2 dB; < −6 = bad | warn/bad | `Air -3.8 dB vs ref` |
| 20 clipping | clipRuns > 0 | bad | `Clipping` |
| 21 true peak | truePeakDb > max(0, ref.truePeakDb) + 0.3 / +0.1 | warn | `True peak +1.2 dBTP` |
| 22 bass phase | corrLow < 0.4 / 0.55; < 0 = bad | warn/bad | `Bass out of phase - dies in mono` |
| 23 mix phase | corr < −0.1 / 0 | bad | `Mix out of phase` |
| 24 squashed | psr < ref.psr − 2.5 / −2 | warn | `Squashed: 3.1 dB less punch than ref` |
| 25 DC | dc > 0.005 / 0.003 | warn | `DC offset` |
| 26 lean | abs(balanceDb) > 1.5 / 1.0 | warn | `Leaning left 2.0 dB` |
| 27 loudness | abs(lufsI − ref.lufs) > 3 / 2, only after 30 s active | info | `4.0 LU quieter than ref` |

Region issues need `longValid`; nothing is raised while `!active` (existing flags run their clear timers).

- [ ] Write tests: `matching_ref_no_issues`; `sub_hot_latches` (sub region +6 dB: no issue at 0.9 s, issue id 0 severity bad by 1.1 s, still present 1.9 s after the condition ends, gone by 2.1 s); `hot_and_thin_text` (text contains region name and signed dB); `inactive_raises_nothing`; `profile_of_pink` (60 s pink → valid, bands flat ± 1.5 dB, bandLo ≤ band ≤ bandHi); `short_input_invalid` (2 s → `valid == false`).
- [ ] Run → fail. Implement. Run → `ALL PASS`.
- [ ] Run `tools/make_profile.ps1 -Audio "C:\ShannanAIProducer\dataset\01_FULL_TRACKS\Neurofunk\Noisia - Concussion (Mefjus Remix).wav"`. Check the header: `valid`, lufs between −12 and −4, bands finite. Add test `baked_profile_sane`. Commit (header yes, audio no).

### Task 4: Plugin processor + editor (VST3)

**Files:** Create `plugin/PluginProcessor.h/.cpp`, `plugin/PluginEditor.h/.cpp`, `plugin/Views.h`; extend `CMakeLists.txt` (`juce_add_plugin(SnitchMeters … FORMATS VST3 Standalone)`, `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1`, company "Monkey Hammer")

**Interfaces — Produces:**
- `SnitchProcessor`: `snitch::Analyser& analyser()`, `void pushAudio(const float* L, const float* R, int n, double sr)` (re-prepares on rate change; used by loopback), `snitch::RefProfile reference() const`, `void setReference(const snitch::RefProfile&)`, `void resetReference()`, `void loadReferenceFile(const juce::File&)` (background thread, `buildProfile`, callback message on fail). State = the reference profile.
- Editor: 30 Hz timer pulls `snapshot()`, runs `IssueDetector`, repaints. Default 920×280, resizable 560×180 … 1800×700.

Layout left→right: scope (30 %), spectrum (50 %), right column (stat strip over issue list). Spectrum: log x 20 Hz–20 kHz, y −90…0 dB; reference corridor (bandLo…bandHi) drawn level-matched to the live signal; regions with an active issue tinted (amber warn, red bad) with the label at the top of the region. Scope: 40 ms window, trigger = last rising zero crossing of a 150 Hz low-passed copy. Right-click menu: Load reference…, Reset to Concussion (Mefjus Remix), Reset meters, Always on top (standalone only). Drag-drop audio file = load reference.

- [ ] Build `SnitchMeters_VST3` Release. Run `~/tools/pluginval/pluginval.exe --strictness-level 5 --validate <vst3>` → `SUCCESS`. Commit.

### Task 5: Standalone app + loopback capture

**Files:** Create `plugin/LoopbackCapture.h/.cpp`, `plugin/StandaloneApp.cpp`

**Interfaces — Produces:** `LoopbackCapture(std::function<void(const float* L, const float* R, int n, double sr)>)`, `start()`, `stop()`, `juce::String deviceName() const`. Thread: default render endpoint, shared-mode loopback, event-less 10 ms polling; converts the mix format (float32 or PCM16/24/32, any channel count → first two) to planar float; on `AUDCLNT_E_DEVICE_INVALIDATED` or any failed call, releases and retries the current default endpoint every 500 ms. Also registers no notification client — polling `GetDefaultAudioEndpoint` id every 2 s is enough to follow a device switch.

Standalone: `DocumentWindow`, native title bar, always on top (persisted), position/size and state persisted in `%APPDATA%\SnitchMeters\settings.xml`.

- [ ] Build `SnitchMeters_Standalone` Release. Launch, play audio through the default device, confirm via a `--selftest` flag: runs 5 s headless, prints device name, frames captured, lufsS; exits 0 if frames > 0. Commit.

### Task 6: End-to-end check + install script

**Files:** Create `tools/Install Snitch Meters.cmd`, `tools/render_check.cpp` (console: runs a raw f32 file through `Analyser` + `IssueDetector` in real-time-sized blocks and prints the issues raised and for how long), `README.md`

- [ ] `render_check` on Concussion itself → no region issues over the drops (the reference must not snitch on itself); any that appear mean thresholds or gating are wrong — fix those, not the test.
- [ ] `render_check` on a deliberately broken copy (ffmpeg: +8 dB low-shelf at 60 Hz, hard clip, one channel inverted below 120 Hz) → Sub hot, Clipping, Bass out of phase all reported.
- [ ] Screenshot the standalone while that broken file plays; look at it; fix what reads badly.
- [ ] Install script copies the VST3 to `%CommonProgramFiles%\VST3` (needs admin; not run automatically) and makes a desktop shortcut to the standalone. Commit. Open the output folder.
