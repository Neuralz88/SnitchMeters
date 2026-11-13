// render_check <in.f32> <sampleRate> [--timeline]
// Plays a file through the live analyser + issue detector the way the meter
// would and prints which issues came up and for how long.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include "RawAudio.h"
#include "dsp/Analyser.h"
#include "dsp/ConcussionProfile.h"
#include "dsp/Issues.h"

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: render_check <in.f32> <sampleRate>\n"); return 2; }
    std::vector<float> L, R;
    if (! loadRawStereo(argv[1], L, R)) { std::fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    const double sr = std::atof(argv[2]);
    const snitch::RefProfile& ref = snitch::concussionProfile();

    snitch::Analyser analyser;
    analyser.prepare(sr);
    analyser.setMinLowShareDb(snitch::minLowShareFor(ref));
    snitch::IssueDetector detector;

    struct Seen { double seconds = 0.0; double first = -1.0; std::string text; int severity = 0; };
    std::map<int, Seen> seen;
    const int block = 512, blocksPerTick = 3;
    const double dt = block * blocksPerTick / sr;
    snitch::Snapshot s {};
    int tick = 0;
    double maxDc = 0.0;
    const bool timeline = argc > 3 && std::strcmp(argv[3], "--timeline") == 0;
    for (size_t pos = 0; pos + block <= L.size(); pos += block) {
        analyser.process(L.data() + pos, R.data() + pos, block);
        if (++tick % blocksPerTick != 0) continue;
        s = analyser.snapshot();
        snitch::Issue issues[32];
        const int n = detector.update(s, ref, dt, issues, 32);
        maxDc = std::max(maxDc, (double) s.dc);
        if (timeline && tick % (blocksPerTick * 144) == 0) {   // every ~5 s
            std::printf("%5.0fs M%6.1f S%6.1f I%6.1f psr%5.1f cl%5.2f dc%.3f clip%3d |", (double) pos / sr, s.lufsM, s.lufsS, s.lufsI, s.psr, s.corrLow, s.dc, s.clipRuns);
            for (int i = 0; i < n; ++i) std::printf(" %s;", issues[i].text);
            std::printf("\n");
        }
        for (int i = 0; i < n; ++i) {
            Seen& e = seen[issues[i].id];
            if (e.first < 0.0) e.first = (double) pos / sr;
            e.seconds += dt; e.text = issues[i].text; e.severity = std::max(e.severity, issues[i].severity);
        }
    }

    const double total = (double) L.size() / sr;
    std::printf("reference: %s%s\n", ref.name, ref.valid ? "" : " (INVALID)");
    std::printf("%.0f s played: integrated %.1f LUFS, true peak max %+.2f dBTP, PSR %.1f, corr %.2f, low corr %.2f\n",
                total, s.lufsI, s.truePeakMaxDb, s.psr, s.corr, s.corrLow);
    std::printf("largest DC reading %.4f\n", maxDc);
    if (seen.empty()) std::printf("no issues\n");
    static const char* level[] = { "info", "warn", "BAD " };
    for (const auto& kv : seen)
        std::printf("  [%s] id %2d  %5.1f s (%4.1f%%)  first at %5.1f s  last: %s\n", level[kv.second.severity], kv.first,
                    kv.second.seconds, 100.0 * kv.second.seconds / total, kv.second.first, kv.second.text.c_str());
    return 0;
}
