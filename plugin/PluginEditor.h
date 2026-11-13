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

    bool isInterestedInFileDrag(const juce::StringArray&) override { return true; }
    void fileDragEnter(const juce::StringArray&, int, int) override { dragging = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragging = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override;

    // Shown in the footer by the standalone app ("listening to Speakers ...").
    std::function<juce::String()> sourceName;

    static constexpr int kDefaultWidth = 940, kDefaultHeight = 290, kMinWidth = 560, kMinHeight = 190,
                         kMaxWidth = 1800, kMaxHeight = 700;

private:
    void timerCallback() override;
    void showMenu();

    SnitchProcessor& plugin;
    ScopeView scope;
    SpectrumView spectrum;
    StatStrip stats;
    IssueList issueList;
    snitch::IssueDetector detector;
    juce::String footerLeft, footerRight;
    juce::Rectangle<int> footer;
    std::unique_ptr<juce::FileChooser> chooser;
    double lastTick = 0.0;
    bool dragging = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SnitchEditor)
};
