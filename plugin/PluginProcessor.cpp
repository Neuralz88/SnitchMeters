#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/ConcussionProfile.h"
#include "dsp/ProfileBuilder.h"

namespace {
const juce::uint32 kStateMagic = 0x31544e53; // "SNT1"
const double kMaxReferenceSeconds = 15.0 * 60.0;
}

SnitchProcessor::SnitchProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    meter.prepare(48000.0);
    setReference(snitch::concussionProfile());
}

SnitchProcessor::~SnitchProcessor() {
    loader.removeAllJobs(true, 20000);
}

void SnitchProcessor::prepareToPlay(double sampleRate, int) {
    if (! isStandalone()) meter.prepare(sampleRate);
}

bool SnitchProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto out = layouts.getMainOutputChannelSet();
    return out == layouts.getMainInputChannelSet()
        && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void SnitchProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    if (isStandalone() || buffer.getNumChannels() == 0) return;
    meter.process(buffer.getReadPointer(0), buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : nullptr,
                  buffer.getNumSamples());
}

void SnitchProcessor::pushAudio(const float* L, const float* R, int numSamples, double sampleRate) {
    if (sampleRate != pushedRate) {
        pushedRate = sampleRate;
        meter.prepare(sampleRate);
    }
    meter.process(L, R, numSamples);
}

snitch::RefProfile SnitchProcessor::reference() const {
    const juce::ScopedLock lock(refLock);
    return ref;
}

void SnitchProcessor::setReference(const snitch::RefProfile& profile) {
    {
        const juce::ScopedLock lock(refLock);
        ref = profile;
        ref.name[sizeof(ref.name) - 1] = 0;
    }
    meter.setMinLowShareDb(snitch::minLowShareFor(profile));
}

void SnitchProcessor::resetReference() {
    setReference(snitch::concussionProfile());
    setStatus({});
}

juce::String SnitchProcessor::status() const {
    const juce::ScopedLock lock(refLock);
    return statusText;
}

void SnitchProcessor::setStatus(const juce::String& text) {
    const juce::ScopedLock lock(refLock);
    statusText = text;
}

void SnitchProcessor::loadReferenceFile(const juce::File& file) {
    if (referenceBusy.exchange(true)) return;
    setStatus("Measuring " + file.getFileNameWithoutExtension() + "...");
    loader.addJob([this, file] {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0) {
            setStatus("Can't read " + file.getFileName() + " as audio - reference unchanged");
        } else {
            const auto length = (int) juce::jmin(reader->lengthInSamples, (juce::int64) (reader->sampleRate * kMaxReferenceSeconds));
            juce::AudioBuffer<float> audio(2, length);
            reader->read(&audio, 0, length, 0, true, true);
            if (reader->numChannels < 2) audio.copyFrom(1, 0, audio, 0, 0, length);
            const auto name = file.getFileNameWithoutExtension();
            const snitch::RefProfile profile = snitch::buildProfile(audio.getReadPointer(0), audio.getReadPointer(1),
                                                                    (size_t) length, reader->sampleRate, name.toRawUTF8());
            if (profile.valid) { setReference(profile); setStatus({}); }
            else setStatus(file.getFileName() + " is too short or too quiet - reference unchanged");
        }
        referenceBusy.store(false);
    });
}

void SnitchProcessor::getStateInformation(juce::MemoryBlock& destData) {
    const snitch::RefProfile profile = reference();
    juce::MemoryOutputStream out(destData, false);
    out.writeInt((int) kStateMagic);
    out.writeInt((int) sizeof(profile));
    out.write(&profile, sizeof(profile));
}

void SnitchProcessor::setStateInformation(const void* data, int sizeInBytes) {
    juce::MemoryInputStream in(data, (size_t) juce::jmax(0, sizeInBytes), false);
    snitch::RefProfile profile {};
    if ((juce::uint32) in.readInt() != kStateMagic || in.readInt() != (int) sizeof(profile)) return;
    if (in.read(&profile, (int) sizeof(profile)) != (int) sizeof(profile)) return;
    profile.name[sizeof(profile.name) - 1] = 0;
    for (int i = 0; i < snitch::kBands; ++i)
        if (! (std::isfinite(profile.band[i]) && std::isfinite(profile.bandLo[i]) && std::isfinite(profile.bandHi[i]))) return;
    if (profile.valid) setReference(profile);
}

juce::AudioProcessorEditor* SnitchProcessor::createEditor() { return new SnitchEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SnitchProcessor(); }
