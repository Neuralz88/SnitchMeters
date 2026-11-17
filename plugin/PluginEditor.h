#pragma once
#include "PluginProcessor.h"
#include "Views.h"

class SnitchEditor : public juce::AudioProcessorEditor,
                     public juce::FileDragAndDropTarget,
                     private juce::Timer {
public:
    explicit SnitchEditor(SnitchProcessor&);
    ~SnitchEditor() override;

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;

    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
    void fileDragEnter(const juce::StringArray&, int, int) override { dragging = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragging = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override;

    // Shown in the footer by the standalone app ("listening to Speakers ...").
    std::function<juce::String()> sourceName;
    // Set by the standalone app: snaps its window to a screen edge (SnitchProcessor::WindowDock).
    std::function<void(int)> dockWindow;

    static constexpr int kDefaultWidth = 940, kDefaultHeight = 460, kMinWidth = 300, kMinHeight = 190,
                         kMaxWidth = 3840, kMaxHeight = 2160;

private:
    void timerCallback() override;
    void showMenu();
    void layoutMeters(juce::Rectangle<int> area);
    void setWaveExpanded(bool shouldFill);

    SnitchProcessor& plugin;
    WaveView wave;
    SpectrumView spectrum;
    StatStrip stats;
    IssueList issueList;
    snitch::IssueDetector detector;
    juce::String footerLeft, footerRight;
    juce::Rectangle<int> footer, content, divider;   // divider: the gap you drag to resize the waveform
    bool draggingDivider = false;
    std::unique_ptr<juce::FileChooser> chooser;
    double lastTick = 0.0;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SnitchEditor)
};
