# Snitch Meters

A small desktop meter that listens to whatever is playing and says what is
wrong with it compared with a reference track. Waveform, spectrum, loudness,
true peak, phase, and a list of plain-word issues.

Default reference: **Noisia - Concussion (Mefjus Remix)** (measured profile only;
the audio is not in this repo or in the binary).

## Two forms

- **Snitch Meters.exe** - always-on-top window fed by system audio (WASAPI
  loopback of the default output device).
- **Snitch Meters.vst3 / .clap** - the same meter for a DAW master. Needed when
  the DAW plays through an ASIO driver, which system-audio capture cannot hear.
  On a Mac it is the plugin only (VST3, Audio Unit, CLAP).

`tools\Install Snitch Meters.cmd` makes a desktop shortcut and copies the VST3.

## Reading it

- **Waveform** - the last half minute of the mix, scrolling, at a fixed level
  scale (outer lines = full scale). Coloured by content: red lows (under 200 Hz),
  green mids, blue highs (over 2 kHz); mixtures blend. Red caps top and bottom
  mark samples at full scale.
  - wheel: zoom time (about 30 ms to 30 s across); shift+wheel: zoom level
  - click: freeze / go live; drag: scroll back through the history
  - hover: time and peak level under the cursor
  - double-click: expand to fill the window, again to put it back
  - drag the gap between the waveform and the meters to resize it
- **Spectrum** - blue is live (pink noise reads flat). The shaded violet band is
  where the reference's own loud sections sit, level-matched to what is playing.
  The white line is your long-term balance; verdicts come from that line.
- **Issues** - red = bad, amber = worth a look, grey = information.

| Flag | Means |
|---|---|
| `Sub +4.2 dB vs ref` (Sub / Bass / Low-mid / Mid / Presence / Air) | that region is louder or quieter than the reference, after matching overall level |
| `Over 0 dBFS - will clip` | samples beyond full scale |
| `Clipping harder than ref` | more flat-topped peaks than the reference has |
| `True peak +x dBTP` | inter-sample peaks beyond the reference's |
| `Bass out of phase - dies in mono` | left/right disagree below 150 Hz |
| `Mix out of phase` | whole mix negatively correlated |
| `Squashed: x dB less punch than ref` | peak-to-loudness ratio well under the reference |
| `DC offset`, `Leaning left x dB` | what they say |
| `x LU quieter than ref` | plugin only (system audio level is arbitrary) |

Balance verdicts only use loud sections that have low end, so intros and
breakdowns are not compared with drops, and they need about 12 s of that.

Right-click: load another reference, reset, always on top. Or drop a track on
the window. Also there: **Waveform** (dock it to the top, bottom, left or right
of the meters, or expanded) and, in the desktop app, **Dock window** (snap the
whole window along the top, bottom, left or right edge of the screen; down a
side the meters stack one above the other).

## Channel

Right-click > **Channel**: Stereo, Left, Right, Mid or Side. The waveform, the
live spectrum and the level numbers (LUFS, true peak, punch) switch to that
channel; the whole mix stays behind the spectrum as a grey line. One channel
reads as it would on both speakers, so a mono mix gives the same numbers in
every view but Side. Mid = (L+R)/2, Side = (L-R)/2.

Every channel keeps its own half minute of waveform, so you can freeze a moment
and flip through the channels. The issue list, the reference comparison and the
phase bar always judge the whole mix.

## Build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target SnitchMeters_Standalone SnitchMeters_VST3 SnitchMeters_CLAP
```

Mac: `.github/workflows/mac.yml` builds universal VST3 / AU / CLAP on a GitHub
Mac runner and uploads `Snitch Meters Mac.zip` (unsigned; `tools/mac/Install.command`
inside it installs them).

Tests and tools without JUCE:

```
cmake -S . -B build-tests -G "Visual Studio 17 2022" -A x64 -DSNITCH_BUILD_PLUGIN=OFF
cmake --build build-tests --config Release
build-tests\Release\dsp_tests.exe
tools\make_profile.ps1 -Audio "track.wav"      # re-bake the default reference
build-tests\Release\render_check.exe x.f32 44100 --timeline
```
