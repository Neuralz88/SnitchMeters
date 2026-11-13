#pragma once
#include <cstdio>
#include <vector>

// Reads interleaved stereo float32 little-endian (ffmpeg -f f32le -ac 2).
inline bool loadRawStereo(const char* path, std::vector<float>& L, std::vector<float>& R) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long bytes = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    const size_t frames = (size_t) bytes / (2 * sizeof(float));
    std::vector<float> interleaved(frames * 2);
    const size_t got = std::fread(interleaved.data(), sizeof(float), frames * 2, f);
    std::fclose(f);
    if (got != frames * 2) return false;
    L.resize(frames); R.resize(frames);
    for (size_t i = 0; i < frames; ++i) { L[i] = interleaved[2 * i]; R[i] = interleaved[2 * i + 1]; }
    return frames > 0;
}
