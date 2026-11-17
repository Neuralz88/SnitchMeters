Snitch Meters for Mac (plugin only)
===================================

Works on Apple Silicon and Intel Macs, macOS 10.15 or newer.

  Snitch Meters.vst3       - Bitwig, Ableton Live, Cubase, Reaper, Studio One ...
  Snitch Meters.component  - Audio Unit: Logic Pro, GarageBand, Ableton Live
  Snitch Meters.clap       - Bitwig, Reaper

Install
-------
1. Unzip this folder on the Mac.
2. Right-click "Install.command", choose Open, then Open again.
   (A plain double-click is refused because the build is not signed with a paid
   Apple developer certificate.)
3. Rescan plug-ins in the DAW. Put Snitch Meters last on the master.

By hand instead: copy each file into the matching folder under
~/Library/Audio/Plug-Ins (VST3, Components, CLAP), then run in Terminal:
  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins

There is no Mac desktop app: listening to whatever the Mac is playing needs
separate code that has not been written.

Right-click the meter for everything: reference track, channel
(Stereo / Left / Right / Mid / Side), waveform position.
