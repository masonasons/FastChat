#!/bin/sh
# Builds FastChat (Release) into this folder: FastChat.app on the Mac (for Apple
# silicon and Intel Macs), the FastChat program on Linux.
#   ./build.sh           FastChat
#   ./build.sh tests     FastChat and the core tests (build/fastchat_tests)
#   ./build.sh no-prism  FastChat without Prism (no speech)
#   ./build.sh native    Mac: only for this Mac's processor, in build-native/ (a faster build)
#   ./build.sh dist      FastChat, then FastChat-mac.zip, or FastChat-linux.zip on Linux (dist.sh)
#   ./build.sh notarize  Mac: FastChat, then a notarized FastChat-mac.dmg and .zip (dist.sh notarize)
#   ./build.sh install   Linux: FastChat, installed in ~/.local/share/fastchat (where it updates
#                        itself) with an entry in the applications menu
#
# The Mac needs the Xcode command line tools (xcode-select --install), CMake
# 3.24+ and Ninja (brew install cmake ninja). Linux needs a C++20 compiler,
# CMake 3.24+, Ninja and the development files the README lists.
set -e
cd "$(dirname "$0")"

case "$(uname -s)" in
    Darwin) MAC=1 ;;
    *) MAC= ;;
esac

PRISM=ON
DIST=
DIST_ARGS=
INSTALL=
TARGETS="--target FastChat"
ARCHS="arm64;x86_64"
BUILD=build
for arg in "$@"; do
    case "$arg" in
        no-prism) PRISM=OFF ;;
        tests) TARGETS="--target FastChat --target fastchat_tests" ;;
        native) ARCHS="$(uname -m)"; BUILD=build-native ;;
        dist) DIST=1 ;;
        notarize) DIST=1; DIST_ARGS=notarize ;;
        install) INSTALL=1 ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done
if [ -z "$MAC" ] && [ -n "$DIST_ARGS" ]; then
    echo "Error: notarize is for the Mac."
    exit 1
fi
if [ -n "$MAC" ] && [ -n "$INSTALL" ]; then
    echo "Error: install is for Linux; on the Mac, drag FastChat.app to Applications."
    exit 1
fi

if [ -n "$MAC" ]; then
    HELP="brew install cmake ninja"
else
    HELP="see Building > Linux in README.md"
fi
for tool in cmake ninja; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Error: $tool is required ($HELP)."
        exit 1
    fi
done

# The first run fetches and builds wxWidgets, Prism and the rest into build/
# (build-native/ for native),
# which takes a while; later runs reuse them. A running FastChat keeps running
# from the old copy; quit and reopen it to use the new one.
# The Ninja on the PATH now, not the one a reused build folder remembers (CI
# installs it somewhere new each run).
if [ -n "$MAC" ]; then
    cmake -S . -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTCHAT_PRISM=$PRISM "-DCMAKE_OSX_ARCHITECTURES=$ARCHS" \
        "-DCMAKE_MAKE_PROGRAM=$(command -v ninja)"
else
    cmake -S . -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTCHAT_PRISM=$PRISM \
        "-DCMAKE_MAKE_PROGRAM=$(command -v ninja)"
fi
cmake --build $BUILD $TARGETS

if [ -n "$DIST" ]; then
    ./dist.sh $DIST_ARGS
fi

if [ -n "$INSTALL" ]; then
    # A copy outside this folder, which FastChat's updater may replace (it
    # leaves a FastChat running from its source folder alone). Copied as a new
    # file renamed over the old, so a FastChat running from there keeps running.
    dest="${XDG_DATA_HOME:-$HOME/.local/share}/fastchat"
    mkdir -p "$dest"
    cp FastChat "$dest/FastChat.new" && chmod 755 "$dest/FastChat.new" && mv -f "$dest/FastChat.new" "$dest/FastChat"
    cp README.md "$dest/"
    if [ -d soundpacks ]; then cp -R soundpacks "$dest/"; fi
    apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
    mkdir -p "$apps"
    cat > "$apps/fastchat.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=FastChat
Comment=Chat and code with AI models
Exec="$dest/FastChat"
Path=$dest
Icon=utilities-terminal
Terminal=false
Categories=Utility;Development;
DESKTOP
    echo "Installed FastChat in $dest, with an entry in the applications menu."
fi

echo
if [ -n "$MAC" ]; then
    echo "Build successful. Open FastChat.app to start."
else
    if [ -n "$INSTALL" ]; then
        echo "Build successful. Start FastChat from the applications menu."
    else
        echo "Build successful. Run ./FastChat to start."
    fi
fi
