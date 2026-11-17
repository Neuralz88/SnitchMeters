#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "dsp/Analyser.h"
#include "dsp/Issues.h"

namespace ui {
const juce::Colour background { 0xff0c0e11 }, panel { 0xff13161b }, line { 0xff232830 }, dim { 0xff6b7482 },
                   text { 0xffd7dde6 }, accent { 0xff35d0ff }, good { 0xff3ddc84 }, warn { 0xffffb020 }, bad { 0xffff4d5e },
                   info { 0xff8fa3b8 }, reference { 0xffb9a4ff };

inline juce::Font font(float height, bool bold = false) {
    return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
}
inline juce::Colour severityColour(int severity) { return severity >= 2 ? bad : severity == 1 ? warn : info; }

inline void drawPanel(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title) {
    g.setColour(panel);
    g.fillRoundedRectangle(area.toFloat(), 5.0f);
    g.setColour(dim);
    g.setFont(font(10.0f, true));
    g.drawText(title, area.reduced(8, 5), juce::Justification::topLeft);
}
} // namespace ui

// The mix as a scrolling waveform at a fixed level scale, coloured by what is in it:
// red lows, green mids, blue highs. Wheel zooms time, shift+wheel zooms level, a click
// freezes it, a drag scrolls back through the last half minute, a double-click expands it.
class WaveView : public juce::Component {
public:
    static constexpr int kZoomSteps = 11, kUnityZoom = 3, kDefaultZoom = 7, kGainSteps = 6;

    std::function<void()> onMenu, onToggleExpand, onViewChanged;

    int zoom() const { return zoomStep; }
    int gain() const { return gainStep; }
    bool isFrozen() const { return frozen; }
    void setView(int zoom, int gain) {
        zoomStep = juce::jlimit(0, kZoomSteps - 1, zoom);
        gainStep = juce::jlimit(0, kGainSteps - 1, gain);
        cacheValid = false;
        clampEnd();
        repaint();
    }

    void update(const snitch::Analyser& analyser, int channel = snitch::kViewStereo) {
        source = &analyser;
        rate = analyser.waveBucketsPerSecond();
        if (channel != view) {   // every channel has its own history of the same moments
            view = channel;
            cacheValid = false;
            if (frozen) source->copyWave(held.data(), heldFirst, (int) held.size(), view);
            repaint();
        }
        if (frozen) return;
        end = analyser.waveCount();
        repaint();
    }

    void setFrozen(bool shouldFreeze) {
        if (frozen == shouldFreeze || source == nullptr) return;
        if (shouldFreeze) {   // keep our own copy: the live history keeps moving on
            heldTotal = source->waveCount();
            heldFirst = juce::jmax((long long) 0, heldTotal - snitch::Analyser::kWave);
            held.resize((size_t) (heldTotal - heldFirst));
            source->copyWave(held.data(), heldFirst, (int) held.size(), view);
            end = heldTotal;
        }
        frozen = shouldFreeze;
        cacheValid = false;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        const auto area = getLocalBounds();
        ui::drawPanel(g, area, "WAVEFORM");
        const auto plot = plotArea().toFloat();
        if (plot.getWidth() < 8.0f || plot.getHeight() < 8.0f) return;

        const int cols = (int) plot.getWidth(), perPx = bucketsPerPixel(), pxPer = pixelsPerBucket();
        const float mid = plot.getCentreY(), half = plot.getHeight() * 0.5f * kFill, level = (float) (1 << gainStep);
        const double pxPerSecond = rate * pxPer / perPx;
        auto yOf = [mid, half, level](float v) { return mid - juce::jlimit(-1.0f, 1.0f, v * level) * half; };

        // Level grid: the outer lines are the loudest level that fits (full scale until zoomed in).
        g.setFont(ui::font(9.5f));
        for (int i = 0; i < 2; ++i) {
            const float offset = half * (i == 0 ? 1.0f : 0.5f);
            g.setColour(ui::line.withAlpha(i == 0 ? 1.0f : 0.55f));
            g.drawHorizontalLine((int) (mid - offset), plot.getX(), plot.getRight());
            g.drawHorizontalLine((int) (mid + offset), plot.getX(), plot.getRight());
            if (offset < 14.0f) continue;
            g.setColour(ui::dim.withAlpha(0.8f));
            g.drawText(juce::String(-6 * (gainStep + i)) + " dB", juce::Rectangle<float>(plot.getX() + 2.0f, mid - offset + 1.0f, 50.0f, 11.0f),
                       juce::Justification::centredLeft);
        }
        g.setColour(ui::line);
        g.drawHorizontalLine((int) mid, plot.getX(), plot.getRight());

        // Time grid, counted back from the right-hand edge.
        double interval = 20.0;
        for (double candidate : { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0 })
            if (candidate * pxPerSecond >= 90.0) { interval = candidate; break; }
        for (int k = 1; plot.getRight() - (float) (k * interval * pxPerSecond) > plot.getX() + 16.0f; ++k) {
            const float x = plot.getRight() - (float) (k * interval * pxPerSecond);
            g.setColour(ui::line.withAlpha(0.55f));
            g.drawVerticalLine((int) x, plot.getY(), plot.getBottom());
            g.setColour(ui::dim);
            g.drawText("-" + timeText(k * interval), juce::Rectangle<float>(x - 30.0f, plot.getBottom() + 2.0f, 60.0f, 11.0f), juce::Justification::centred);
        }

        if (source != nullptr) {
            const int count = pxPer == 1 ? cols : cols / pxPer + 2;
            prepareColumns(end / perPx - count, count, perPx);
            auto xOf = [&](int i) { return pxPer == 1 ? plot.getX() + (float) i : plot.getRight() - (float) ((count - 1 - i) * pxPer); };

            g.saveState();
            g.reduceClipRegion(plot.toNearestInt());
            if (pxPer == 1) {
                // One column a pixel, stretched to meet the one before so fast swings leave no gaps.
                float previousLo = 0.0f, previousHi = 0.0f;
                for (int i = 0; i < count; ++i) {
                    const Column& c = columns[(size_t) i];
                    const float lo = i > 0 ? juce::jmin(c.lo, previousHi) : c.lo, hi = i > 0 ? juce::jmax(c.hi, previousLo) : c.hi;
                    previousLo = c.lo; previousHi = c.hi;
                    const float top = yOf(hi), bottom = juce::jmax(top + 1.0f, yOf(lo));
                    g.setColour(c.colour);
                    g.fillRect(juce::Rectangle<float>(xOf(i), top, 1.0f, bottom - top));
                }
            } else {
                // Zoomed past one slice a pixel: join the slices into a ribbon.
                for (int i = 0; i + 1 < count; ++i) {
                    const Column& a = columns[(size_t) i]; const Column& b = columns[(size_t) i + 1];
                    juce::Path quad;
                    quad.startNewSubPath(xOf(i), yOf(a.hi) - 0.7f);
                    quad.lineTo(xOf(i + 1), yOf(b.hi) - 0.7f);
                    quad.lineTo(xOf(i + 1), yOf(b.lo) + 0.7f);
                    quad.lineTo(xOf(i), yOf(a.lo) + 0.7f);
                    quad.closeSubPath();
                    g.setColour(b.colour);
                    g.fillPath(quad);
                }
            }
            // Flat-topped at full scale: a red cap, whatever the level zoom.
            g.setColour(ui::bad);
            for (int i = 0; i < count; ++i) {
                if (columns[(size_t) i].peak < snitch::Analyser::kClipLevel) continue;
                const float x = xOf(i) - (pxPer == 1 ? 0.0f : (float) pxPer * 0.5f);
                g.fillRect(juce::Rectangle<float>(x, plot.getY(), (float) pxPer, 3.0f));
                g.fillRect(juce::Rectangle<float>(x, plot.getBottom() - 3.0f, (float) pxPer, 3.0f));
            }

            if (hoverX >= 0 && plot.contains((float) hoverX, plot.getCentreY())) {
                const int fromRight = (int) plot.getRight() - hoverX;
                const int index = juce::jlimit(0, count - 1, pxPer == 1 ? hoverX - (int) plot.getX() : count - 1 - (fromRight + pxPer / 2) / pxPer);
                const float peak = columns[(size_t) index].peak;
                const juce::String label = "-" + timeText(fromRight / pxPerSecond) + "   peak "
                                         + (peak > 1e-5f ? juce::String(20.0f * std::log10(peak), 1) + " dB" : juce::String("--"));
                g.setColour(ui::text.withAlpha(0.6f));
                g.drawVerticalLine(hoverX, plot.getY(), plot.getBottom());
                g.setFont(ui::font(11.0f, true));
                const float width = 150.0f;
                auto box = juce::Rectangle<float>((float) hoverX + 6.0f, plot.getY() + 14.0f, width, 15.0f);
                if (box.getRight() > plot.getRight()) box.setX((float) hoverX - 6.0f - width);
                g.setColour(ui::panel.withAlpha(0.85f));
                g.fillRoundedRectangle(box, 3.0f);
                g.setColour(ui::text);
                g.drawText(label, box.reduced(4.0f, 0.0f), juce::Justification::centredLeft);
            }
            g.restoreState();
        }

        // Header: what the colours mean, how much time is on screen
        g.setFont(ui::font(10.0f, true));
        auto legend = area.reduced(8, 5).withTrimmedLeft(72).withHeight(12);
        const char* names[] = { "LOW", "MID", "HIGH" };
        for (int i = 0; i < 3 && area.getWidth() > 330; ++i) {
            g.setColour(colourOf(i == 0 ? 1.0f : 0.0f, i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f));
            g.drawText(names[i], legend.removeFromLeft(30), juce::Justification::centredLeft);
        }
        juce::String right = timeText(cols / pxPerSecond);
        if (view != snitch::kViewStereo) right = juce::String(snitch::viewName(view)) + " ONLY   " + right;
        if (gainStep > 0) right = "level +" + juce::String(6 * gainStep) + " dB   " + right;
        if (frozen) right = (area.getWidth() > 420 ? "FROZEN - click to go live   " : "FROZEN   ") + right;
        g.setFont(ui::font(10.0f, frozen));
        g.setColour(frozen ? ui::warn : view != snitch::kViewStereo ? ui::accent : ui::dim);
        g.drawText(right, area.reduced(8, 5), juce::Justification::topRight);

        if (isMouseOver() && plot.getWidth() > 520.0f && plot.getHeight() > 60.0f) {
            g.setFont(ui::font(10.0f));
            g.setColour(ui::dim.withAlpha(0.9f));
            g.drawText("wheel: zoom time   shift+wheel: zoom level   click: freeze   drag: scroll back   double-click: expand",
                       plot.reduced(4.0f, 2.0f), juce::Justification::bottomRight);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override {
        dragged = false;
        dragStartEnd = end;
        if (e.mods.isPopupMenu() && onMenu) onMenu();
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (e.mods.isPopupMenu() || (! dragged && std::abs(e.getDistanceFromDragStartX()) < 4)) return;
        if (! dragged) { setFrozen(true); dragStartEnd = end; dragged = true; }
        end = dragStartEnd - (long long) std::llround(e.getDistanceFromDragStartX() * (double) bucketsPerPixel() / pixelsPerBucket());
        clampEnd();
        hoverX = e.x;
        repaint();
    }
    void mouseUp(const juce::MouseEvent& e) override {
        if (! dragged && ! e.mods.isPopupMenu() && e.getNumberOfClicks() == 1) setFrozen(! frozen);
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override {
        if (e.mods.isPopupMenu()) return;
        setFrozen(! frozen);   // put back what the first click of the pair changed
        if (onToggleExpand) onToggleExpand();
    }
    void mouseMove(const juce::MouseEvent& e) override { hoverX = e.x; repaint(); }
    void mouseEnter(const juce::MouseEvent& e) override { hoverX = e.x; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hoverX = -1; repaint(); }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
        const float delta = wheel.deltaY != 0.0f ? wheel.deltaY : wheel.deltaX;   // shift turns the wheel sideways
        if (delta == 0.0f) return;
        const int step = delta > 0.0f ? 1 : -1;
        if (e.mods.isShiftDown() || e.mods.isCtrlDown()) setView(zoomStep, gainStep + step);
        else setView(zoomStep - step, gainStep);
        if (onViewChanged) onViewChanged();
    }
    void resized() override { clampEnd(); }

private:
    struct Column { float lo, hi, peak; juce::Colour colour; };
    static constexpr float kFill = 0.94f;

    juce::Rectangle<int> plotArea() const { return getLocalBounds().reduced(8, 20); }
    int bucketsPerPixel() const { return zoomStep > kUnityZoom ? 1 << (zoomStep - kUnityZoom) : 1; }
    int pixelsPerBucket() const { return zoomStep < kUnityZoom ? 1 << (kUnityZoom - zoomStep) : 1; }

    static juce::String timeText(double seconds) {
        if (seconds < 0.9995) return juce::String(juce::roundToInt(seconds * 1000.0)) + " ms";
        return juce::String(seconds, std::abs(seconds - std::round(seconds)) < 0.05 ? 0 : 1) + " s";
    }

    // Highs carry far less energy than lows, so they are weighted up before the three are compared.
    static juce::Colour colourOf(float low, float mid, float high) {
        float a = low, b = mid * 2.0f, c = high * 5.0f;
        const float most = juce::jmax(a, b, c);
        if (most < 1e-6f) return ui::dim;
        a /= most; b /= most; c /= most;
        a *= a; b *= b; c *= c;
        const float red = a + 0.20f * b + 0.15f * c, green = 0.24f * a + 0.92f * b + 0.55f * c, blue = 0.22f * a + 0.35f * b + c;
        const float scale = 1.0f / juce::jmax(1.0f, red, green, blue);
        return juce::Colour::fromFloatRGBA(red * scale, green * scale, blue * scale, 1.0f);
    }

    void fetch(snitch::WaveBucket* dst, long long first, int n) const {
        if (! frozen) { source->copyWave(dst, first, n, view); return; }
        for (int i = 0; i < n; ++i) {
            const long long k = first + i - heldFirst;
            dst[i] = k >= 0 && k < (long long) held.size() ? held[(size_t) k] : snitch::WaveBucket {};
        }
    }

    Column makeColumn(long long firstBucket, int n) const {
        snitch::WaveBucket buckets[1 << (kZoomSteps - 1 - kUnityZoom)];
        fetch(buckets, firstBucket, n);
        Column c { 1e9f, -1e9f, 0.0f, {} };
        double low = 0.0, mid = 0.0, high = 0.0;
        for (int i = 0; i < n; ++i) {
            const snitch::WaveBucket& b = buckets[i];
            c.lo = juce::jmin(c.lo, b.lo); c.hi = juce::jmax(c.hi, b.hi); c.peak = juce::jmax(c.peak, b.peak);
            low += (double) b.low * b.low; mid += (double) b.mid * b.mid; high += (double) b.high * b.high;
        }
        c.colour = colourOf((float) std::sqrt(low / n), (float) std::sqrt(mid / n), (float) std::sqrt(high / n));
        return c;
    }

    // Columns sit on a fixed grid of the history, so one already worked out is reused as it scrolls.
    void prepareColumns(long long first, int count, int perPx) {
        spare.resize((size_t) count);
        for (int i = 0; i < count; ++i) {
            const long long k = first + i - cacheFirst;
            spare[(size_t) i] = cacheValid && cachePerPx == perPx && k >= 0 && k < (long long) columns.size()
                                    ? columns[(size_t) k] : makeColumn((first + i) * perPx, perPx);
        }
        columns.swap(spare);
        cacheFirst = first; cachePerPx = perPx; cacheValid = true;
    }

    void clampEnd() {
        if (! frozen) return;
        const long long visible = (long long) plotArea().getWidth() * bucketsPerPixel() / pixelsPerBucket();
        end = juce::jlimit(juce::jmin(heldTotal, heldFirst + visible), heldTotal, end);
    }

    const snitch::Analyser* source = nullptr;
    std::vector<snitch::WaveBucket> held;
    std::vector<Column> columns, spare;
    long long end = 0, heldFirst = 0, heldTotal = 0, cacheFirst = 0, dragStartEnd = 0;
    double rate = 4000.0;
    int zoomStep = kDefaultZoom, gainStep = 0, cachePerPx = 0, hoverX = -1, view = snitch::kViewStereo;
    bool frozen = false, cacheValid = false, dragged = false;
};

// Live spectrum (pink noise reads flat) over the reference's own range per band.
class SpectrumView : public juce::Component {
public:
    SpectrumView() { setInterceptsMouseClicks(false, false); }

    void update(const snitch::Snapshot& snapshot, const snitch::RefProfile& profile, const snitch::Issue* list, int count) {
        s = snapshot; ref = profile;
        numIssues = juce::jmin(count, (int) juce::numElementsInArray(issues));
        for (int i = 0; i < numIssues; ++i) issues[i] = list[i];

        float peak = -200.0f;
        for (int j = 0; j < snitch::kSpecPoints; ++j) {
            shown[j] = s.spectrum[j] + tilt(pointFreq(j));
            peak = juce::jmax(peak, shown[j]);
        }
        hasSignal = s.active && peak > -150.0f;
        if (hasSignal) {
            // Third-octave levels and the fine spectrum are different units; line them up by the median gap.
            float gaps[snitch::kBands]; int n = 0;
            for (int i = 0; i < snitch::kBands; ++i)
                if (snitch::bandIsJudged(i)) gaps[n++] = shownAt(snitch::bandCentre(i)) - s.bandShort[i];
            std::sort(gaps, gaps + n);
            follow(bandOffset, gaps[n / 2], 0.12f);
            follow(top, peak + 8.0f, peak + 8.0f > top ? 0.3f : 0.03f);
            if (ref.valid)
                follow(refOffset, snitch::matchOffset(s.longValid ? s.bandLong : s.bandShort, ref.band), 0.12f);
            primed = true;
        }
        repaint();
    }

    void paint(juce::Graphics& g) override {
        const auto area = getLocalBounds();
        ui::drawPanel(g, area, "SPECTRUM");
        plot = area.reduced(8, 20).withTrimmedBottom(6).toFloat();
        if (plot.getWidth() < 20.0f || plot.getHeight() < 20.0f) return;

        g.setFont(ui::font(10.0f));
        for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f }) {
            const float x = xOf(f);
            g.setColour(ui::line);
            g.drawVerticalLine((int) x, plot.getY(), plot.getBottom());
            g.setColour(ui::dim);
            g.drawText(f >= 1000.0f ? juce::String((int) (f / 1000.0f)) + "k" : juce::String((int) f),
                       juce::Rectangle<float>(x - 20.0f, plot.getBottom() + 1.0f, 40.0f, 12.0f), juce::Justification::centred);
        }
        g.setColour(ui::line);
        for (float db = top - 12.0f; db > top - kRange; db -= 12.0f) g.drawHorizontalLine((int) yOf(db), plot.getX(), plot.getRight());

        if (! primed) return;
        g.saveState();
        g.reduceClipRegion(plot.toNearestInt());

        // Regions with something wrong
        for (int i = 0; i < numIssues; ++i) {
            if (issues[i].freqHi <= issues[i].freqLo) continue;
            const float x0 = xOf(issues[i].freqLo), x1 = xOf(issues[i].freqHi);
            const auto colour = ui::severityColour(issues[i].severity);
            g.setColour(colour.withAlpha(0.13f));
            g.fillRect(juce::Rectangle<float>(x0, plot.getY(), x1 - x0, plot.getHeight()));
            g.setColour(colour.withAlpha(0.7f));
            g.fillRect(juce::Rectangle<float>(x0, plot.getY(), x1 - x0, 2.0f));
        }

        if (ref.valid) {
            juce::Path corridor;
            for (int i = 0; i < snitch::kBands; ++i) {
                const float x = xOf(snitch::bandCentre(i)), y = yOf(ref.bandHi[i] + refOffset + bandOffset);
                if (i == 0) corridor.startNewSubPath(x, y); else corridor.lineTo(x, y);
            }
            for (int i = snitch::kBands - 1; i >= 0; --i)
                corridor.lineTo(xOf(snitch::bandCentre(i)), yOf(ref.bandLo[i] + refOffset + bandOffset));
            corridor.closeSubPath();
            g.setColour(ui::reference.withAlpha(0.16f));
            g.fillPath(corridor);
            juce::Path mean;
            for (int i = 0; i < snitch::kBands; ++i) {
                const float x = xOf(snitch::bandCentre(i)), y = yOf(ref.band[i] + refOffset + bandOffset);
                if (i == 0) mean.startNewSubPath(x, y); else mean.lineTo(x, y);
            }
            g.setColour(ui::reference.withAlpha(0.55f));
            const float dashes[] = { 4.0f, 3.0f };
            juce::Path dashed;
            juce::PathStrokeType(1.0f).createDashedStroke(dashed, mean, dashes, 2);
            g.fillPath(dashed);
        }

        // One channel on its own is drawn over a faint outline of the whole mix.
        const bool oneChannel = s.view != snitch::kViewStereo;
        juce::Path curve, whole;
        for (int j = 0; j < snitch::kSpecPoints; ++j) {
            const float x = xOf(pointFreq(j)), y = yOf(oneChannel ? s.viewSpectrum[j] + tilt(pointFreq(j)) : shown[j]);
            if (j == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
            if (! oneChannel) continue;
            if (j == 0) whole.startNewSubPath(x, yOf(shown[j])); else whole.lineTo(x, yOf(shown[j]));
        }
        if (oneChannel && hasSignal) {
            g.setColour(ui::dim.withAlpha(0.8f));
            g.strokePath(whole, juce::PathStrokeType(1.0f));
        }
        juce::Path filled = curve;
        filled.lineTo(plot.getRight(), plot.getBottom());
        filled.lineTo(plot.getX(), plot.getBottom());
        filled.closeSubPath();
        const auto colour = hasSignal ? ui::accent : ui::dim;
        g.setGradientFill(juce::ColourGradient(colour.withAlpha(0.38f), 0.0f, plot.getY(), colour.withAlpha(0.03f), 0.0f, plot.getBottom(), false));
        g.fillPath(filled);
        g.setColour(colour);
        g.strokePath(curve, juce::PathStrokeType(1.2f));

        // Long-term balance: the line the verdicts are made from
        if (hasSignal && s.longValid) {
            juce::Path balance;
            for (int i = 0; i < snitch::kBands; ++i) {
                const float x = xOf(snitch::bandCentre(i)), y = yOf(s.bandLong[i] + bandOffset);
                if (i == 0) balance.startNewSubPath(x, y); else balance.lineTo(x, y);
            }
            g.setColour(ui::text.withAlpha(0.85f));
            g.strokePath(balance, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved));
        }
        g.restoreState();

        g.setFont(ui::font(11.0f, true));
        for (int i = 0; i < numIssues; ++i) {
            if (issues[i].freqHi <= issues[i].freqLo) continue;
            const float x0 = xOf(issues[i].freqLo), x1 = xOf(issues[i].freqHi);
            juce::String label(issues[i].text);
            label = issues[i].id == snitch::IssueDetector::kBassPhase ? juce::String("PHASE")
                                                                      : label.upToFirstOccurrenceOf(" dB", false, false).toUpperCase();
            g.setColour(ui::severityColour(issues[i].severity));
            const float y = issues[i].id == snitch::IssueDetector::kBassPhase ? plot.getY() + 18.0f : plot.getY() + 4.0f;
            g.drawFittedText(label, juce::Rectangle<float>(x0, y, x1 - x0, 13.0f).expanded(14.0f, 0.0f).toNearestInt(),
                             juce::Justification::centred, 1, 0.8f);
        }

        g.setFont(ui::font(10.0f));
        g.setColour(ui::reference.withAlpha(0.9f));
        g.drawText("shaded = reference", area.reduced(8, 5), juce::Justification::topRight);
        if (oneChannel) {
            g.setFont(ui::font(10.0f, true));
            g.setColour(ui::accent);
            g.drawText(juce::String(snitch::viewName(s.view)) + " ONLY   grey = whole mix", area.reduced(8, 5).withTrimmedLeft(72).withTrimmedRight(100),
                       juce::Justification::topLeft);
        }
    }

private:
    static constexpr float kRange = 54.0f;
    static float pointFreq(int j) { return 20.0f * std::pow(1000.0f, (float) j / (float) (snitch::kSpecPoints - 1)); }
    static float tilt(float f) { return 10.0f * std::log10(f / 1000.0f); }  // +3 dB/oct
    float xOf(float f) const { return plot.getX() + plot.getWidth() * std::log10(juce::jmax(f, 20.0f) / 20.0f) / 3.0f; }
    float yOf(float db) const { return plot.getY() + plot.getHeight() * (top - db) / kRange; }
    float shownAt(float f) const {
        const float pos = juce::jlimit(0.0f, (float) (snitch::kSpecPoints - 1), std::log10(f / 20.0f) / 3.0f * (float) (snitch::kSpecPoints - 1));
        const int j = juce::jmin((int) pos, snitch::kSpecPoints - 2);
        return shown[j] + (shown[j + 1] - shown[j]) * (pos - (float) j);
    }
    void follow(float& value, float target, float rate) const { value = primed ? value + rate * (target - value) : target; }

    snitch::Snapshot s {};
    snitch::RefProfile ref {};
    snitch::Issue issues[16] {};
    int numIssues = 0;
    float shown[snitch::kSpecPoints] = {};
    float top = -10.0f, bandOffset = 0.0f, refOffset = 0.0f;
    bool primed = false, hasSignal = false;
    juce::Rectangle<float> plot;
};

// Numbers: loudness, peak, punch, phase.
class StatStrip : public juce::Component {
public:
    StatStrip() { setInterceptsMouseClicks(false, false); }

    void update(const snitch::Snapshot& snapshot, const snitch::RefProfile& profile) { s = snapshot; ref = profile; repaint(); }

    void paint(juce::Graphics& g) override {
        const auto area = getLocalBounds();
        g.setColour(ui::panel);
        g.fillRoundedRectangle(area.toFloat(), 5.0f);
        auto inner = area.reduced(10, 7);
        auto phase = inner.removeFromBottom(22);
        const int cellW = inner.getWidth() / 2, cellH = inner.getHeight() / 2;

        // Level numbers follow the channel being viewed; the phase bar is always the whole mix.
        const bool on = s.active, oneChannel = s.view != snitch::kViewStereo;
        const juce::String of = oneChannel ? juce::String(" - ") + snitch::viewName(s.view) : juce::String();
        const float tpLimit = juce::jmax(0.0f, ref.valid ? ref.truePeakDb : 0.0f) + 0.3f;
        cell(g, { inner.getX(), inner.getY(), cellW, cellH }, "LUFS SHORT" + of, on ? juce::String(s.viewLufsS, 1) : dash(), ui::text, oneChannel);
        cell(g, { inner.getX() + cellW, inner.getY(), cellW, cellH }, "INTEGRATED" + of, s.viewLufsI > -100.0f ? juce::String(s.viewLufsI, 1) : dash(), ui::text, oneChannel);
        cell(g, { inner.getX(), inner.getY() + cellH, cellW, cellH }, "TRUE PEAK" + of,
             on ? signedText(s.viewTruePeakDb) : dash(), on && (s.viewTruePeakDb > tpLimit || (! oneChannel && s.overs > 0)) ? ui::bad : ui::text, oneChannel);
        const bool squashed = on && ! oneChannel && ref.valid && ref.psr - s.psr > 3.0f;
        cell(g, { inner.getX() + cellW, inner.getY() + cellH, cellW, cellH },
             oneChannel ? "PUNCH" + of : ref.valid ? "PUNCH (ref " + juce::String(ref.psr, 1) + ")" : juce::String("PUNCH"),
             on ? juce::String(s.viewPsr, 1) : dash(), squashed ? ui::warn : ui::text, oneChannel);

        // Phase: -1 .. +1, the mix as a bar and the low end as a tick
        g.setColour(ui::dim);
        g.setFont(ui::font(10.0f, true));
        g.drawText("PHASE", phase.removeFromLeft(42), juce::Justification::centredLeft);
        const auto bar = phase.reduced(0, 7).toFloat();
        g.setColour(ui::line);
        g.fillRoundedRectangle(bar, 2.0f);
        const float centre = bar.getCentreX();
        if (on) {
            const float x = centre + juce::jlimit(-1.0f, 1.0f, s.corr) * bar.getWidth() * 0.5f;
            g.setColour(s.corr < 0.0f ? ui::bad : ui::accent);
            g.fillRoundedRectangle(juce::Rectangle<float>(juce::jmin(centre, x), bar.getY(), std::abs(x - centre), bar.getHeight()), 2.0f);
            if (s.judging) {
                const float xl = centre + juce::jlimit(-1.0f, 1.0f, s.corrLow) * bar.getWidth() * 0.5f;
                g.setColour(s.corrLow < 0.4f ? ui::bad : ui::text);
                g.fillRect(juce::Rectangle<float>(xl - 1.0f, bar.getY() - 4.0f, 2.0f, bar.getHeight() + 8.0f));
            }
        }
        g.setColour(ui::dim);
        g.fillRect(juce::Rectangle<float>(centre - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f));
    }

private:
    static juce::String dash() { return "--"; }
    static juce::String signedText(float v) { return (v >= 0.0f ? "+" : "") + juce::String(v, 1); }
    static void cell(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label, const juce::String& value, juce::Colour colour,
                     bool marked = false) {
        g.setColour(marked ? ui::accent : ui::dim);
        g.setFont(ui::font(9.5f, true));
        g.drawText(label, r.removeFromTop(12), juce::Justification::centredLeft);
        g.setColour(colour);
        g.setFont(ui::font(juce::jlimit(14.0f, 24.0f, (float) r.getHeight() * 0.95f), true));
        g.drawText(value, r, juce::Justification::centredLeft);
    }

    snitch::Snapshot s {};
    snitch::RefProfile ref {};
};

// The verdicts, worst first.
class IssueList : public juce::Component {
public:
    IssueList() { setInterceptsMouseClicks(false, false); }

    void update(const snitch::Snapshot& snapshot, const snitch::Issue* list, int count) {
        s = snapshot;
        numIssues = juce::jmin(count, (int) juce::numElementsInArray(issues));
        for (int i = 0; i < numIssues; ++i) issues[i] = list[i];
        repaint();
    }

    void paint(juce::Graphics& g) override {
        const auto area = getLocalBounds();
        g.setColour(ui::panel);
        g.fillRoundedRectangle(area.toFloat(), 5.0f);
        auto inner = area.reduced(10, 7);
        const int rowH = juce::jlimit(15, 20, inner.getHeight() / 5);

        if (numIssues == 0) {
            juce::String message; juce::Colour colour = ui::dim;
            if (! s.active)          message = "No signal";
            else if (! s.longValid)  message = s.judging ? "Listening... balance verdict in a few seconds"
                                                         : "Waiting for a loud section with low end to judge the balance";
            else { message = "Nothing to snitch on"; colour = ui::good; }
            g.setColour(colour);
            g.setFont(ui::font(13.0f, colour == ui::good));
            g.drawFittedText(message, inner, juce::Justification::centredLeft, 3);
            return;
        }
        for (int i = 0; i < numIssues && inner.getHeight() >= rowH; ++i) {
            auto row = inner.removeFromTop(rowH);
            const auto colour = ui::severityColour(issues[i].severity);
            g.setColour(colour);
            g.fillRoundedRectangle(row.removeFromLeft(4).reduced(0, 3).toFloat(), 2.0f);
            row.removeFromLeft(7);
            g.setColour(issues[i].severity == 0 ? ui::info : ui::text);
            g.setFont(ui::font(13.0f, issues[i].severity >= 2));
            g.drawFittedText(issues[i].text, row, juce::Justification::centredLeft, 1, 0.75f);
        }
        if (s.active && ! s.longValid && inner.getHeight() >= rowH) {
            g.setColour(ui::dim);
            g.setFont(ui::font(11.0f));
            g.drawFittedText("balance verdict still warming up", inner.removeFromTop(rowH), juce::Justification::centredLeft, 1);
        }
    }

private:
    snitch::Snapshot s {};
    snitch::Issue issues[16] {};
    int numIssues = 0;
};
