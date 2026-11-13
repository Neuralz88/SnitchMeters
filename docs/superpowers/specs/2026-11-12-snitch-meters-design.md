# Snitch Meters — design (approved 2026-11-12)

## Intent
A MiniMeters-style desktop meter that listens to whatever is playing, shows an
oscilloscope and a spectrum, and says in plain words what is wrong with the mix
compared with a reference track. The user produces neurofunk; the default
reference is **Noisia - Concussion (Mefjus Remix)** (user's choice).

## Goals
- Small always-on-top desktop window; captures system audio (WASAPI loopback).
- Same meter as a VST3 for the Bitwig master (ASIO output is invisible to loopback).
- Oscilloscope (stable, trigger-locked), spectrum with the reference drawn as a
  target band, strip with LUFS (momentary / short-term / integrated), true peak,
  stereo correlation, dynamic range (PLR).
- Issue callouts, live, on the display and in a short list:
  - band balance vs reference ("sub 4 dB hot", "harsh 3–5 kHz", "dull top end")
  - clipping / true-peak overs
  - low end out of phase or too wide
  - over-compression (PLR well under reference)
  - DC offset
  - louder / quieter than reference
- Reference profile for Concussion baked in; drop any audio file on the window
  to swap the reference.

## Non-goals
- No audio processing (pass-through only in the VST3).
- No installer; a build output folder and an install .cmd for the VST3.
- macOS / Linux.

## Anti-goals
- Flags that flicker or nag: every issue needs hold time + hysteresis, and the
  balance comparison is loudness-normalised so a quiet playback is not "wrong".
- The reference audio itself is never shipped or embedded — only its measured
  profile (band levels, loudness, PLR, width numbers).

## Approaches
1. **JUCE, one codebase → Standalone + VST3 (picked).** Matches Mentalist /
   MasteringSuite builds; the plugin form is needed for ASIO Bitwig. Standalone
   uses its own WASAPI loopback capture thread instead of JUCE's device manager.
2. .NET + NAudio — quickest loopback app, but no plugin form.
3. Python + Qt — fastest to write, heaviest to run, no plugin form.

## Shape
- `dsp/` — header-only, no JUCE: K-weighted loudness (BS.1770), 4x true peak,
  FFT band analyser (1/3-octave, long + short averages), correlation (full band
  and <150 Hz), DC, PLR, reference profile + issue detector with hysteresis.
  Unit-tested with a plain console test exe.
- `tools/profile_ref` — console tool: audio file → profile (JSON + C++ header).
  Runs the same `dsp/` code as the live meter so numbers are comparable.
- `plugin/` — JUCE processor (feeds analyser), editor (scope, spectrum, strip,
  issue list), WASAPI loopback source for the standalone, drag-drop reference.

## Builder's calls
Thresholds, colours, layout details, window size (default ~900x260, resizable),
band edges.
