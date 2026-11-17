#!/bin/bash
# Builds Snitch Meters on this Mac and installs the plugins for this user.
# Run it from Terminal with:
#   curl -fsSL https://raw.githubusercontent.com/Neuralz88/SnitchMeters/master/tools/mac/build.sh | bash
set -e
WORK="$HOME/SnitchMetersBuild"
CMAKE_VERSION=3.31.6

if ! xcode-select -p >/dev/null 2>&1; then
    echo "Apple's developer tools are needed first. A window is opening to install them."
    echo "When that has finished, run this same line again."
    xcode-select --install || true
    exit 1
fi

mkdir -p "$WORK"
cd "$WORK"

# CMake: use the one on this Mac, or fetch a copy into the build folder (nothing is installed system-wide).
CMAKE="$(command -v cmake || true)"
if [ -z "$CMAKE" ]; then
    CMAKE="$WORK/cmake-$CMAKE_VERSION-macos-universal/CMake.app/Contents/bin/cmake"
    if [ ! -x "$CMAKE" ]; then
        echo "Fetching CMake $CMAKE_VERSION..."
        curl -fL "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-macos-universal.tar.gz" | tar xz
    fi
fi

if [ -d SnitchMeters/.git ]; then
    git -C SnitchMeters pull --ff-only
else
    git clone --depth 1 https://github.com/Neuralz88/SnitchMeters.git
fi

echo "Building (the first time takes several minutes)..."
"$CMAKE" -S SnitchMeters -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15
"$CMAKE" --build build --config Release --parallel "$(sysctl -n hw.ncpu)" --target SnitchMeters_VST3 SnitchMeters_AU SnitchMeters_CLAP

install() {   # bundle name, plug-in folder
    found="$(find build/SnitchMeters_artefacts -maxdepth 3 -name "$1" | head -1)"
    if [ -z "$found" ]; then echo "Could not find $1 after the build"; exit 1; fi
    mkdir -p "$2"
    rm -rf "$2/$1"
    cp -R "$found" "$2/"
    echo "Installed $2/$1"
}
install "Snitch Meters.vst3"      "$HOME/Library/Audio/Plug-Ins/VST3"
install "Snitch Meters.component" "$HOME/Library/Audio/Plug-Ins/Components"
install "Snitch Meters.clap"      "$HOME/Library/Audio/Plug-Ins/CLAP"
killall -9 AudioComponentRegistrar 2>/dev/null || true

echo
echo "Done. Rescan plug-ins in your DAW and put Snitch Meters last on the master."
echo "The build folder $WORK can be deleted."
