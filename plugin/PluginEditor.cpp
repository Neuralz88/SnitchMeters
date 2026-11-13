#include "PluginEditor.h"

SnitchEditor::SnitchEditor(SnitchProcessor& p) : AudioProcessorEditor(&p), plugin(p) {
    addAndMakeVisible(scope);
    addAndMakeVisible(spectrum);
    addAndMakeVisible(stats);
    addAndMakeVisible(issueList);

    // System audio is at whatever the volume happens to be, so only the plugin compares loudness.
    detector.setCompareLoudness(! plugin.isStandalone());

    if (! plugin.isStandalone()) {   // the standalone window does its own resizing
        setResizable(true, true);
        setResizeLimits(kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    }
    setSize(kDefaultWidth, kDefaultHeight);
    lastTick = juce::Time::getMillisecondCounterHiRes();
    startTimerHz(30);
}

SnitchEditor::~SnitchEditor() { stopTimer(); }

void SnitchEditor::timerCallback() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = juce::jlimit(0.001, 0.25, (now - lastTick) / 1000.0);
    lastTick = now;

    const snitch::Snapshot s = plugin.analyser().snapshot();
    const snitch::RefProfile ref = plugin.reference();
    snitch::Issue issues[16];
    const int n = detector.update(s, ref, dt, issues, 16);

    scope.update(plugin.analyser(), s.active);
    spectrum.update(s, ref, issues, n);
    stats.update(s, ref);
    issueList.update(s, issues, n);

    const juce::String left = "ref: " + juce::String(juce::CharPointer_UTF8(ref.name));
    juce::String right = plugin.status();
    if (right.isEmpty() && sourceName != nullptr) right = sourceName();
    if (left != footerLeft || right != footerRight) {
        footerLeft = left; footerRight = right;
        repaint(footer);
    }
}

void SnitchEditor::paint(juce::Graphics& g) {
    g.fillAll(ui::background);
    g.setFont(ui::font(11.0f));
    g.setColour(ui::reference);
    g.drawText(footerLeft, footer.withTrimmedRight(footer.getWidth() / 2), juce::Justification::centredLeft);
    g.setColour(ui::dim);
    g.drawText(footerRight, footer.withTrimmedLeft(footer.getWidth() / 2), juce::Justification::centredRight);
}

void SnitchEditor::paintOverChildren(juce::Graphics& g) {
    if (! dragging) return;
    g.fillAll(ui::background.withAlpha(0.85f));
    g.setColour(ui::reference);
    g.drawRoundedRectangle(getLocalBounds().reduced(8).toFloat(), 8.0f, 2.0f);
    g.setFont(ui::font(18.0f, true));
    g.drawText("Drop a track to use it as the reference", getLocalBounds(), juce::Justification::centred);
}

void SnitchEditor::resized() {
    auto area = getLocalBounds().reduced(6);
    footer = area.removeFromBottom(16).reduced(4, 0);
    area.removeFromBottom(2);

    const int gap = 6;
    auto right = area.removeFromRight(juce::jlimit(190, 300, area.getWidth() * 28 / 100));
    area.removeFromRight(gap);
    // The scope gives way first when the window gets narrow.
    const int scopeWidth = area.getWidth() < 420 ? 0 : area.getWidth() * 36 / 100;
    scope.setVisible(scopeWidth > 0);
    if (scopeWidth > 0) { scope.setBounds(area.removeFromLeft(scopeWidth)); area.removeFromLeft(gap); }
    spectrum.setBounds(area);

    stats.setBounds(right.removeFromTop(juce::jlimit(96, 130, right.getHeight() * 46 / 100)));
    right.removeFromTop(gap);
    issueList.setBounds(right);
}

void SnitchEditor::filesDropped(const juce::StringArray& files, int, int) {
    dragging = false;
    repaint();
    if (files.size() > 0) plugin.loadReferenceFile(juce::File(files[0]));
}

void SnitchEditor::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) showMenu();
}

void SnitchEditor::showMenu() {
    juce::PopupMenu menu;
    menu.addItem(1, "Load reference track...", ! plugin.referenceBusy.load());
    menu.addItem(2, "Reset reference to Concussion (Mefjus Remix)");
    menu.addSeparator();
    menu.addItem(3, "Reset meters");
    if (plugin.isStandalone()) {
        menu.addSeparator();
        menu.addItem(4, "Always on top", true, plugin.alwaysOnTop.load());
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                       [safe = juce::Component::SafePointer<SnitchEditor>(this)](int result) {
        if (safe == nullptr) return;
        auto& self = *safe;
        if (result == 1) {
            self.chooser = std::make_unique<juce::FileChooser>("Reference track", juce::File(), "*.wav;*.flac;*.mp3;*.aif;*.aiff;*.ogg;*.m4a;*.wma");
            self.chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                      [safe](const juce::FileChooser& fc) {
                if (safe != nullptr && fc.getResult().existsAsFile()) safe->plugin.loadReferenceFile(fc.getResult());
            });
        } else if (result == 2) {
            self.plugin.resetReference();
        } else if (result == 3) {
            self.plugin.analyser().requestReset();
            self.detector.reset();
        } else if (result == 4) {
            const bool onTop = ! self.plugin.alwaysOnTop.load();
            self.plugin.alwaysOnTop.store(onTop);
            if (auto* window = self.getTopLevelComponent()) window->setAlwaysOnTop(onTop);
        }
    });
}
