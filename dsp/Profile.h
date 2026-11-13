#pragma once

namespace snitch {

constexpr int kBands = 31;        // third octaves, 20 Hz .. 20 kHz
constexpr int kSpecPoints = 240;  // log-spaced display points, 20 Hz .. 20 kHz
constexpr int kRegions = 6;       // Sub, Bass, Low-mid, Mid, Presence, Air

// What a reference track measures as. Only this ever ships, never the audio.
struct RefProfile {
    char  name[96];
    bool  valid;
    float band[kBands];     // dB relative to total band power, mean of the loud sections
    float bandLo[kBands];   // 10th percentile of 3 s windows
    float bandHi[kBands];   // 90th percentile
    float lufs;             // integrated
    float truePeakDb;
    float psr;              // median (3 s true peak - short-term LUFS) over the loud sections
    float corrLow;          // median correlation below 150 Hz
    float clipRate;         // flat-topped runs per second at full scale, 90th percentile
    float dc;               // |mean|, 90th percentile
};

struct Snapshot {
    bool  active;           // momentary > -50 LUFS
    bool  longValid;        // >= 12 s of audio in the long average
    bool  judging;          // this moment counts towards the long average (loud and has low end)
    float lufsM, lufsS, lufsI;
    float truePeakDb;       // 3 s hold
    float truePeakMaxDb;    // since reset
    int   clipRuns;         // runs of >= 3 samples at full scale, last 3 s
    int   overs;            // samples beyond full scale (float streams only), last 3 s
    float corr, corrLow;    // -1..1
    float dc;               // max |mean| of L, R
    float psr;              // 3 s true peak - short-term LUFS
    float balanceDb;        // L - R, long-term; + = left
    float activeSeconds;    // since the last reset
    float lowShareDb;       // energy below 150 Hz relative to the total
    float bandShort[kBands], bandLong[kBands];  // dB relative to total band power
    float spectrum[kSpecPoints];                // dBFS (full-scale sine = 0)
};

struct Issue {
    int   id;
    int   severity;         // 0 info, 1 warn, 2 bad
    char  text[96];
    float freqLo, freqHi;   // 0,0 when the issue is not tied to a frequency range
};

} // namespace snitch
