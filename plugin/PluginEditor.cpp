#include "PluginEditor.h"

namespace {
const int kGap = 6;
enum MenuId { kMenuExpand = 10, kMenuFreeze = 11, kMenuWave = 20, kMenuDock = 30, kMenuChannel = 40 };
}

SnitchEditor::SnitchEditor(SnitchProcessor& p) : AudioProcessorEditor(&p), plugin(p) {
    addAndMakeVisible(wave);
    wave.setView(plugin.waveZoom.load(), plugin.waveGain.load());
    wave.onMenu = [this] { showMenu(); };
    wave.onToggleExpand = [this] { setWaveExpanded(! plugin.waveExpanded.load()); };
    wave.onViewChanged = [this] { plugin.waveZoom.store(wave.zoom()); plugin.waveGain.store(wave.gain()); };
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

    wave.update(plugin.analyser(), plugin.channel());
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

    if (! divider.isEmpty()) {   // grip
        g.setColour(draggingDivider ? ui::accent : ui::line);
        const bool upright = divider.getHeight() > divider.getWidth();
        g.fillRoundedRectangle(divider.toFloat().withSizeKeepingCentre(upright ? 2.0f : 36.0f, upright ? 36.0f : 2.0f), 1.0f);
    }
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

    content = area;

    // The waveform takes its share of the window on the side it is docked to; the rest is meters.
    const bool expanded = plugin.waveExpanded.load();
    divider = {};
    if (expanded) {
        wave.setBounds(area);
    } else {
        const int dock = plugin.waveDock.load();
        const bool stacked = dock == SnitchProcessor::kWaveTop || dock == SnitchProcessor::kWaveBottom;
        const int total = stacked ? area.getHeight() : area.getWidth();
        const int least = juce::jmin(70, total / 3);
        const int size = juce::jlimit(least, juce::jmax(least, total - kGap - juce::jmin(stacked ? 150 : 240, total / 2)),
                                      juce::roundToInt((float) total * plugin.waveShare.load()));
        switch (dock) {
            case SnitchProcessor::kWaveLeft:   wave.setBounds(area.removeFromLeft(size));   divider = area.removeFromLeft(kGap);   break;
            case SnitchProcessor::kWaveRight:  wave.setBounds(area.removeFromRight(size));  divider = area.removeFromRight(kGap);  break;
            case SnitchProcessor::kWaveBottom: wave.setBounds(area.removeFromBottom(size)); divider = area.removeFromBottom(kGap); break;
            default:                           wave.setBounds(area.removeFromTop(size));    divider = area.removeFromTop(kGap);    break;
        }
    }
    spectrum.setVisible(! expanded);
    stats.setVisible(! expanded);
    issueList.setVisible(! expanded);
    if (! expanded) layoutMeters(area);
}

void SnitchEditor::layoutMeters(juce::Rectangle<int> area) {
    if (area.getWidth() >= 430) {
        auto right = area.removeFromRight(juce::jlimit(190, 300, area.getWidth() * 28 / 100));
        area.removeFromRight(kGap);
        spectrum.setBounds(area);
        stats.setBounds(right.removeFromTop(juce::jlimit(juce::jmin(96, right.getHeight() / 2), 130, right.getHeight() * 46 / 100)));
        right.removeFromTop(kGap);
        issueList.setBounds(right);
        return;
    }
    // Narrow (a window docked down the side of the screen): one above the other.
    issueList.setBounds(area.removeFromBottom(juce::jlimit(60, 150, area.getHeight() * 28 / 100)));
    area.removeFromBottom(kGap);
    stats.setBounds(area.removeFromBottom(juce::jmin(110, area.getHeight() / 2)));
    area.removeFromBottom(kGap);
    spectrum.setVisible(area.getHeight() >= 70);
    spectrum.setBounds(area);
}

void SnitchEditor::setWaveExpanded(bool shouldFill) {
    plugin.waveExpanded.store(shouldFill);
    resized();
    repaint();
}

void SnitchEditor::filesDropped(const juce::StringArray& files, int, int) {
    dragging = false;
    repaint();
    if (files.size() > 0) plugin.loadReferenceFile(juce::File(files[0]));
}

void SnitchEditor::mouseDown(const juce::MouseEvent& e) {
    if (e.mods.isPopupMenu()) { showMenu(); return; }
    draggingDivider = divider.contains(e.getPosition());
    if (draggingDivider) repaint(divider);
}

void SnitchEditor::mouseDrag(const juce::MouseEvent& e) {
    if (! draggingDivider || content.isEmpty()) return;
    const auto p = e.getPosition();
    float share = 0.5f;
    switch (plugin.waveDock.load()) {
        case SnitchProcessor::kWaveLeft:   share = (float) (p.x - content.getX()) / (float) content.getWidth(); break;
        case SnitchProcessor::kWaveRight:  share = (float) (content.getRight() - p.x) / (float) content.getWidth(); break;
        case SnitchProcessor::kWaveBottom: share = (float) (content.getBottom() - p.y) / (float) content.getHeight(); break;
        default:                           share = (float) (p.y - content.getY()) / (float) content.getHeight(); break;
    }
    plugin.waveShare.store(juce::jlimit(0.1f, 0.9f, share));
    resized();
    repaint();
}

void SnitchEditor::mouseUp(const juce::MouseEvent&) {
    if (draggingDivider) { draggingDivider = false; repaint(divider); }
}

void SnitchEditor::mouseMove(const juce::MouseEvent& e) {
    const bool upright = divider.getHeight() > divider.getWidth();
    setMouseCursor(! divider.contains(e.getPosition()) ? juce::MouseCursor::NormalCursor
                   : upright ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
}

void SnitchEditor::showMenu() {
    juce::PopupMenu menu;
    menu.addItem(1, "Load reference track...", ! plugin.referenceBusy.load());
    menu.addItem(2, "Reset reference to Concussion (Mefjus Remix)");
    menu.addSeparator();
    menu.addItem(3, "Reset meters");
    menu.addSeparator();

    // The issues and the phase bar always judge the whole mix.
    const int channel = plugin.channel();
    juce::PopupMenu channelMenu;
    const char* channels[] = { "Stereo (whole mix)", "Left", "Right", "Mid (L+R)", "Side (L-R)" };
    for (int i = 0; i < snitch::kViews; ++i) channelMenu.addItem(kMenuChannel + i, channels[i], true, channel == i);
    menu.addSubMenu("Channel", channelMenu);

    const int waveDock = plugin.waveDock.load();
    const bool expanded = plugin.waveExpanded.load();
    juce::PopupMenu waveMenu;
    waveMenu.addItem(kMenuWave + SnitchProcessor::kWaveTop, "Top", true, ! expanded && waveDock == SnitchProcessor::kWaveTop);
    waveMenu.addItem(kMenuWave + SnitchProcessor::kWaveBottom, "Bottom", true, ! expanded && waveDock == SnitchProcessor::kWaveBottom);
    waveMenu.addItem(kMenuWave + SnitchProcessor::kWaveLeft, "Left side", true, ! expanded && waveDock == SnitchProcessor::kWaveLeft);
    waveMenu.addItem(kMenuWave + SnitchProcessor::kWaveRight, "Right side", true, ! expanded && waveDock == SnitchProcessor::kWaveRight);
    waveMenu.addSeparator();
    waveMenu.addItem(kMenuExpand, "Expanded (fills the window)", true, expanded);
    menu.addSubMenu("Waveform", waveMenu);
    menu.addItem(kMenuFreeze, "Freeze waveform", true, wave.isFrozen());

    if (plugin.isStandalone()) {
        const int windowDock = plugin.windowDock.load();
        juce::PopupMenu dockMenu;
        dockMenu.addItem(kMenuDock + SnitchProcessor::kFloating, "Floating", true, windowDock == SnitchProcessor::kFloating);
        dockMenu.addItem(kMenuDock + SnitchProcessor::kDockTop, "Top of the screen", true, windowDock == SnitchProcessor::kDockTop);
        dockMenu.addItem(kMenuDock + SnitchProcessor::kDockBottom, "Bottom of the screen", true, windowDock == SnitchProcessor::kDockBottom);
        dockMenu.addItem(kMenuDock + SnitchProcessor::kDockLeft, "Left side of the screen", true, windowDock == SnitchProcessor::kDockLeft);
        dockMenu.addItem(kMenuDock + SnitchProcessor::kDockRight, "Right side of the screen", true, windowDock == SnitchProcessor::kDockRight);
        menu.addSubMenu("Dock window", dockMenu, dockWindow != nullptr);
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
        } else if (result == kMenuExpand) {
            self.setWaveExpanded(! self.plugin.waveExpanded.load());
        } else if (result == kMenuFreeze) {
            self.wave.setFrozen(! self.wave.isFrozen());
        } else if (result >= kMenuWave && result <= kMenuWave + SnitchProcessor::kWaveBottom) {
            self.plugin.waveDock.store(result - kMenuWave);
            self.setWaveExpanded(false);
        } else if (result >= kMenuDock && result <= kMenuDock + SnitchProcessor::kDockRight) {
            if (self.dockWindow) self.dockWindow(result - kMenuDock);
        } else if (result >= kMenuChannel && result < kMenuChannel + snitch::kViews) {
            self.plugin.setChannel(result - kMenuChannel);
        }
    });
}
