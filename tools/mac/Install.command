#!/bin/bash
# Copies the Snitch Meters plugins into this user's plug-in folders and lets macOS open them.
cd "$(dirname "$0")" || exit 1
install() {
    mkdir -p "$2"
    rm -rf "$2/$1"
    cp -R "$1" "$2/" && xattr -dr com.apple.quarantine "$2/$1" 2>/dev/null
    echo "Installed $2/$1"
}
install "Snitch Meters.vst3"      "$HOME/Library/Audio/Plug-Ins/VST3"
install "Snitch Meters.component" "$HOME/Library/Audio/Plug-Ins/Components"
install "Snitch Meters.clap"      "$HOME/Library/Audio/Plug-Ins/CLAP"
killall -9 AudioComponentRegistrar 2>/dev/null
echo
echo "Done. Rescan plug-ins in your DAW and put Snitch Meters last on the master."
