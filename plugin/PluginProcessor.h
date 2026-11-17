#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "dsp/Analyser.h"
#include "dsp/Issues.h"

// Passes audio through untouched and measures it. In the standalone app the
// audio arrives through pushAudio() from the system-audio capture instead.
class SnitchProcessor : public juce::AudioProcessor {
public:
    SnitchProcessor();
    ~SnitchProcessor() override;

    snitch::Analyser& analyser() { return meter; }
    bool isStandalone() const { return wrapperType == wrapperType_Standalone; }

    // For the standalone capture thread. Follows sample-rate changes.
    void pushAudio(const float* L, const float* R, int numSamples, double sampleRate);

    snitch::RefProfile reference() const;
    void setReference(const snitch::RefProfile& profile);
    void resetReference();
    // Measures the file on a background thread; status() reports how it went.
    void loadReferenceFile(const juce::File& file);
    juce::String status() const;

    // Where things sit. Kept here so it is saved with the rest of the state.
    enum WaveDock { kWaveLeft, kWaveRight, kWaveTop, kWaveBottom };
    enum WindowDock { kFloating, kDockTop, kDockBottom, kDockLeft, kDockRight };
    std::atomic<int> waveDock { kWaveTop }, waveZoom { 7 }, waveGain { 0 };
    std::atomic<float> waveShare { 0.48f };      // of the window, along the side it is docked to
    std::atomic<bool> waveExpanded { false };    // waveform fills the window
    std::atomic<int> windowDock { kFloating };   // standalone: which screen edge the window is snapped to

    // Which channel the waveform, spectrum and level numbers show (snitch::View). Saved with the state.
    int channel() const { return meter.currentView(); }
    void setChannel(int view) { meter.setView(view); }

    std::atomic<bool> alwaysOnTop { true };
    std::atomic<bool> referenceBusy { false };

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Snitch Meters"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

private:
    void setStatus(const juce::String& text);

    snitch::Analyser meter;
    double pushedRate = 0.0;

    juce::CriticalSection refLock;
    snitch::RefProfile ref;
    juce::String statusText;
    juce::ThreadPool loader { 1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SnitchProcessor)
};
