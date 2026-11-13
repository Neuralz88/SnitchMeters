# Snitch Meters

A small desktop meter that listens to whatever is playing and says what is
wrong with it compared with a reference track. Oscilloscope, spectrum, loudness,
true peak, phase, and a list of plain-word issues.

Default reference: **Noisia - Concussion (Mefjus Remix)** (measured profile only;
the audio is not in this repo or in the binary).

## Two forms

- **Snitch Meters.exe** - always-on-top window fed by system audio (WASAPI
  loopback of the default output device).
- **Snitch Meters.vst3** - the same meter for a DAW master. Needed when the DAW
  plays through an ASIO driver, which system-audio capture cannot hear.

`tools\Install Snitch Meters.cmd` makes a desktop shortcut and copies the VST3.

## Reading it

- **Scope** - 40 ms of the mix, locked to the bass.
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
the window.

## Build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target SnitchMeters_Standalone SnitchMeters_VST3
```

Tests and tools without JUCE:

```
cmake -S . -B build-tests -G "Visual Studio 17 2022" -A x64 -DSNITCH_BUILD_PLUGIN=OFF
cmake --build build-tests --config Release
build-tests\Release\dsp_tests.exe
tools\make_profile.ps1 -Audio "track.wav"      # re-bake the default reference
build-tests\Release\render_check.exe x.f32 44100 --timeline
```
