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

// Trigger-locked waveform: 40 ms of the mix, held steady on the bass.
class ScopeView : public juce::Component {
public:
    ScopeView() { setInterceptsMouseClicks(false, false); }

    void update(const snitch::Analyser& analyser, bool active) {
        const double sr = analyser.sampleRate();
        window = juce::jlimit(256, snitch::Analyser::kScope / 3, (int) (sr * 0.040));
        const int total = window * 3;
        left.resize((size_t) total); right.resize((size_t) total); mono.resize((size_t) total);
        analyser.copyScope(left.data(), right.data(), total);

        // Latest rising zero crossing of the low end that still leaves a full window after it.
        const float coef = 1.0f - std::exp(-juce::MathConstants<float>::twoPi * 120.0f / (float) sr);
        float lp1 = 0.0f, lp2 = 0.0f, previous = 0.0f;
        start = total - window;
        for (int i = 0; i < total; ++i) {
            mono[(size_t) i] = 0.5f * (left[(size_t) i] + right[(size_t) i]);
            lp1 += coef * (mono[(size_t) i] - lp1);
            lp2 += coef * (lp1 - lp2);
            if (i > window / 2 && i <= total - window && previous <= 0.0f && lp2 > 0.0f) start = i;
            previous = lp2;
        }

        float peak = 0.0f;
        for (int i = 0; i < window; ++i) peak = juce::jmax(peak, std::abs(mono[(size_t) (start + i)]));
        envelope = juce::jmax(peak, envelope * 0.96f, 0.02f);  // quiet playback still fills the view
        live = active;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        const auto area = getLocalBounds();
        ui::drawPanel(g, area, "SCOPE");
        const auto plot = area.reduced(8, 20).toFloat();
        g.setColour(ui::line);
        g.drawHorizontalLine((int) plot.getCentreY(), plot.getX(), plot.getRight());
        if (mono.empty() || plot.getWidth() < 8.0f) return;

        const int columns = (int) plot.getWidth();
        const float gain = 0.92f / envelope, half = plot.getHeight() * 0.5f, mid = plot.getCentreY();
        juce::Path top, fill;
        std::vector<float> lows((size_t) columns);
        for (int x = 0; x < columns; ++x) {
            const int a = start + x * window / columns, b = juce::jmax(a + 1, start + (x + 1) * window / columns);
            float lo = 1e9f, hi = -1e9f;
            for (int i = a; i < b && i < (int) mono.size(); ++i) { lo = juce::jmin(lo, mono[(size_t) i]); hi = juce::jmax(hi, mono[(size_t) i]); }
            const float yHi = mid - juce::jlimit(-1.0f, 1.0f, hi * gain) * half;
            lows[(size_t) x] = juce::jmax(yHi + 1.0f, mid - juce::jlimit(-1.0f, 1.0f, lo * gain) * half);
            if (x == 0) top.startNewSubPath(plot.getX(), yHi); else top.lineTo(plot.getX() + (float) x, yHi);
        }
        fill = top;
        for (int x = columns - 1; x >= 0; --x) fill.lineTo(plot.getX() + (float) x, lows[(size_t) x]);
        fill.closeSubPath();
        const auto colour = live ? ui::accent : ui::dim;
        g.setColour(colour.withAlpha(0.85f));
        g.fillPath(fill);
        g.setColour(colour);
        g.strokePath(top, juce::PathStrokeType(1.0f));

        g.setColour(ui::dim);
        g.setFont(ui::font(10.0f));
        g.drawText("40 ms", area.reduced(8, 5), juce::Justification::topRight);
    }

private:
    std::vector<float> left, right, mono;
    int window = 1920, start = 0;
    float envelope = 0.02f;
    bool live = false;
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

        juce::Path curve;
        for (int j = 0; j < snitch::kSpecPoints; ++j) {
            const float x = xOf(pointFreq(j)), y = yOf(shown[j]);
            if (j == 0) curve.startNewSubPath(x, y); else curve.lineTo(x, y);
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

        const bool on = s.active;
        const float tpLimit = juce::jmax(0.0f, ref.valid ? ref.truePeakDb : 0.0f) + 0.3f;
        cell(g, { inner.getX(), inner.getY(), cellW, cellH }, "LUFS SHORT", on ? juce::String(s.lufsS, 1) : dash(), ui::text);
        cell(g, { inner.getX() + cellW, inner.getY(), cellW, cellH }, "INTEGRATED", s.lufsI > -100.0f ? juce::String(s.lufsI, 1) : dash(), ui::text);
        cell(g, { inner.getX(), inner.getY() + cellH, cellW, cellH }, "TRUE PEAK",
             on ? signedText(s.truePeakDb) : dash(), on && (s.truePeakDb > tpLimit || s.overs > 0) ? ui::bad : ui::text);
        const bool squashed = on && ref.valid && ref.psr - s.psr > 3.0f;
        cell(g, { inner.getX() + cellW, inner.getY() + cellH, cellW, cellH }, ref.valid ? "PUNCH (ref " + juce::String(ref.psr, 1) + ")" : juce::String("PUNCH"),
             on ? juce::String(s.psr, 1) : dash(), squashed ? ui::warn : ui::text);

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
    static void cell(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& label, const juce::String& value, juce::Colour colour) {
        g.setColour(ui::dim);
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
