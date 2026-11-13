#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>

// Captures whatever Windows is playing on the default output device (WASAPI
// loopback) and hands it over as stereo float. Follows the default device when
// it changes and keeps retrying when there is none.
class LoopbackCapture : private juce::Thread {
public:
    using Callback = std::function<void(const float* L, const float* R, int numSamples, double sampleRate)>;

    explicit LoopbackCapture(Callback onAudio);
    ~LoopbackCapture() override;

    void start();
    void stop();

    juce::String deviceName() const;
    juce::int64 framesCaptured() const { return frames.load(); }

private:
    void run() override;
    void session();

    Callback callback;
    std::atomic<juce::int64> frames { 0 };
    juce::CriticalSection nameLock;
    juce::String name;
};
