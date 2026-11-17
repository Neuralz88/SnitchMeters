// The desktop app: a small always-on-top window fed by system audio.
//
// Command line (for checking a build without anyone watching):
//   --play <file>          meter this audio file (in real time) instead of system audio
//   --seconds <n>          how long the two modes below run (default 5)
//   --selftest <out.txt>   no window; write capture device, frames, loudness; exit 0 if audio frames arrived
//   --screenshot <out.png> no window; save a picture of the meter, then exit
//   --wave <top|bottom|left|right|full>   where the waveform sits in that picture
//   --zoom <0..10>         waveform time zoom in that picture (0 = closest)
//   --size <width>x<height>               size of that picture
//   --channel <stereo|left|right|mid|side>  which channel that picture shows
#include "PluginEditor.h"
#include "LoopbackCapture.h"

namespace {

// Feeds a file through the meter at playback speed.
class FilePlayer : private juce::Thread {
public:
    FilePlayer(const juce::File& fileToPlay, LoopbackCapture::Callback onAudio)
        : juce::Thread("Snitch file player"), file(fileToPlay), callback(std::move(onAudio)) { startThread(); }
    ~FilePlayer() override { stopThread(3000); }

private:
    void run() override {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr) return;
        const int block = (int) (reader->sampleRate * 0.01);
        juce::AudioBuffer<float> buffer(2, block);
        const double startMs = juce::Time::getMillisecondCounterHiRes();
        for (juce::int64 pos = 0; pos < reader->lengthInSamples && ! threadShouldExit(); pos += block) {
            reader->read(&buffer, 0, block, pos, true, true);
            if (reader->numChannels < 2) buffer.copyFrom(1, 0, buffer, 0, 0, block);
            callback(buffer.getReadPointer(0), buffer.getReadPointer(1), block, reader->sampleRate);
            const double dueMs = startMs + 1000.0 * (double) (pos + block) / reader->sampleRate;
            const int waitMs = (int) (dueMs - juce::Time::getMillisecondCounterHiRes());
            if (waitMs > 0) wait(waitMs);
        }
    }

    juce::File file;
    LoopbackCapture::Callback callback;
};

class MainWindow : public juce::DocumentWindow {
public:
    explicit MainWindow(SnitchProcessor& p)
        : DocumentWindow("Snitch Meters", ui::background, DocumentWindow::closeButton | DocumentWindow::minimiseButton), processor(p) {
        setUsingNativeTitleBar(true);
        setContentOwned(processor.createEditor(), true);
        setResizable(true, false);
        setResizeLimits(SnitchEditor::kMinWidth, SnitchEditor::kMinHeight, SnitchEditor::kMaxWidth, SnitchEditor::kMaxHeight);
        if (auto* e = editor()) e->dockWindow = [this](int edge) { dock(edge); };
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    SnitchEditor* editor() { return dynamic_cast<SnitchEditor*>(getContentComponent()); }

    // Snaps the window along one edge of the screen it is on, or lets it float again.
    void dock(int edge) {
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getBounds());
        if (display == nullptr) return;
        const int was = processor.windowDock.exchange(edge);
        if (was == SnitchProcessor::kFloating) floating = getBounds();
        if (edge == SnitchProcessor::kFloating) {
            if (floating.isEmpty()) centreWithSize(SnitchEditor::kDefaultWidth, SnitchEditor::kDefaultHeight);
            else setBounds(floating);
            return;
        }
        auto area = display->userArea;   // bounds are the inside of the window: leave room for its frame
        if (auto* peer = getPeer())
            if (const auto frame = peer->getFrameSizeIfPresent()) area = frame->subtractedFrom(area);

        auto isFlat = [](int e) { return e == SnitchProcessor::kDockTop || e == SnitchProcessor::kDockBottom; };
        const bool keep = was != SnitchProcessor::kFloating && isFlat(was) == isFlat(edge);   // same way round: keep its thickness
        const int thickness = isFlat(edge) ? juce::jmin(area.getHeight() / 2, keep ? getHeight() : 300)
                                           : juce::jmin(area.getWidth() / 2, keep ? getWidth() : 380);
        switch (edge) {
            case SnitchProcessor::kDockTop:    setBounds(area.removeFromTop(thickness));    break;
            case SnitchProcessor::kDockBottom: setBounds(area.removeFromBottom(thickness)); break;
            case SnitchProcessor::kDockLeft:   setBounds(area.removeFromLeft(thickness));   break;
            default:                           setBounds(area.removeFromRight(thickness));  break;
        }
    }

private:
    SnitchProcessor& processor;
    juce::Rectangle<int> floating;
};

class SnitchApp : public juce::JUCEApplication, private juce::Timer {
public:
    const juce::String getApplicationName() override { return "Snitch Meters"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override {
        const juce::StringArray args = getCommandLineParameterArray();
        auto argAfter = [&args](const char* flag) { const int i = args.indexOf(flag); return i >= 0 && i + 1 < args.size() ? args[i + 1].unquoted() : juce::String(); };
        selftestPath = argAfter("--selftest");
        screenshotPath = argAfter("--screenshot");
        headless = selftestPath.isNotEmpty() || screenshotPath.isNotEmpty();

        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Standalone);
        processor = std::make_unique<SnitchProcessor>();
        juce::AudioProcessor::setTypeOfNextNewPlugin(juce::AudioProcessor::wrapperType_Undefined);

        juce::PropertiesFile::Options options;
        options.applicationName = "SnitchMeters";
        options.folderName = "SnitchMeters";
        options.filenameSuffix = "settings";
        options.osxLibrarySubFolder = "Application Support";
        settings = std::make_unique<juce::PropertiesFile>(options);
        if (! headless) {
            juce::MemoryBlock state;
            if (state.fromBase64Encoding(settings->getValue("state"))) processor->setStateInformation(state.getData(), (int) state.getSize());
            processor->alwaysOnTop.store(settings->getBoolValue("alwaysOnTop", true));
        }

        if (headless) {
            const juce::String where = argAfter("--wave");
            const int side = juce::StringArray({ "left", "right", "top", "bottom" }).indexOf(where);
            if (side >= 0) processor->waveDock.store(side);
            processor->waveExpanded.store(where == "full");
            if (argAfter("--zoom").isNotEmpty()) processor->waveZoom.store(argAfter("--zoom").getIntValue());
            processor->setChannel(juce::StringArray({ "stereo", "left", "right", "mid", "side" }).indexOf(argAfter("--channel")));
        }

        window = std::make_unique<MainWindow>(*processor);
        const juce::String size = argAfter("--size");
        if (headless && size.containsChar('x'))
            if (auto* editor = window->editor())
                editor->setSize(size.upToFirstOccurrenceOf("x", false, false).getIntValue(), size.fromFirstOccurrenceOf("x", false, false).getIntValue());
        auto push = [this](const float* L, const float* R, int n, double sr) { processor->pushAudio(L, R, n, sr); };
        const juce::String playPath = argAfter("--play");
        if (playPath.isNotEmpty()) {
            player = std::make_unique<FilePlayer>(juce::File::getCurrentWorkingDirectory().getChildFile(playPath), push);
            if (auto* editor = window->editor()) editor->sourceName = [playPath] { return "file: " + juce::File(playPath).getFileName(); };
        } else {
            capture = std::make_unique<LoopbackCapture>(push);
            capture->start();
            if (auto* editor = window->editor())
                editor->sourceName = [this] {
                    const juce::String device = capture != nullptr ? capture->deviceName() : juce::String();
                    return device.isEmpty() ? juce::String("no output device") : "listening to " + device;
                };
        }

        if (headless) {
            startTimer(juce::jmax(1, argAfter("--seconds").getIntValue() > 0 ? argAfter("--seconds").getIntValue() : 5) * 1000);
            return;
        }
        if (! window->restoreWindowStateFromString(settings->getValue("window")))
            window->centreWithSize(window->getWidth(), window->getHeight());
        window->setAlwaysOnTop(processor->alwaysOnTop.load());
        window->setVisible(true);
    }

    void shutdown() override {
        stopTimer();
        capture.reset();
        player.reset();
        if (! headless && settings != nullptr && window != nullptr && processor != nullptr) {
            juce::MemoryBlock state;
            processor->getStateInformation(state);
            settings->setValue("state", state.toBase64Encoding());
            settings->setValue("alwaysOnTop", processor->alwaysOnTop.load());
            settings->setValue("window", window->getWindowStateAsString());
            settings->saveIfNeeded();
        }
        window.reset();
        processor.reset();
        settings.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    void timerCallback() override {
        stopTimer();
        const snitch::Snapshot s = processor->analyser().snapshot();
        if (screenshotPath.isNotEmpty()) {
            if (auto* editor = window->editor()) {
                const juce::Image image = editor->createComponentSnapshot(editor->getLocalBounds(), true, 2.0f);
                juce::File out(juce::File::getCurrentWorkingDirectory().getChildFile(screenshotPath));
                out.deleteFile();
                juce::FileOutputStream stream(out);
                if (stream.openedOk()) juce::PNGImageFormat().writeImageToStream(image, stream);
            }
        }
        if (selftestPath.isNotEmpty()) {
            const juce::int64 frames = capture != nullptr ? capture->framesCaptured() : 0;
            juce::String report;
            report << "device: " << (capture != nullptr ? capture->deviceName() : juce::String("(file)")) << "\n"
                   << "frames: " << frames << "\n"
                   << "sampleRate: " << processor->analyser().sampleRate() << "\n"
                   << "active: " << (s.active ? 1 : 0) << "\n"
                   << "lufsS: " << s.lufsS << "\n"
                   << "truePeakDb: " << s.truePeakDb << "\n";
            juce::File::getCurrentWorkingDirectory().getChildFile(selftestPath).replaceWithText(report);
            setApplicationReturnValue(frames > 0 ? 0 : 1);
        }
        quit();
    }

    std::unique_ptr<SnitchProcessor> processor;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<LoopbackCapture> capture;
    std::unique_ptr<FilePlayer> player;
    juce::String selftestPath, screenshotPath;
    bool headless = false;
};

} // namespace

// JUCE's standalone wrapper calls this because JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1.
juce::JUCEApplicationBase* juce_CreateApplication() { return new SnitchApp(); }
