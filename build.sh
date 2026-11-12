#!/bin/sh
# Builds FastChat.app (Release, for Apple silicon and Intel Macs) into this folder.
#   ./build.sh           FastChat
#   ./build.sh tests     FastChat and the core tests (build/fastchat_tests)
#   ./build.sh no-prism  FastChat without Prism (no speech)
#   ./build.sh native    only for this Mac's processor, in build-native/ (a faster build)
#   ./build.sh dist      FastChat, then FastChat-mac.zip (dist.sh)
#   ./build.sh notarize  FastChat, then a notarized FastChat-mac.dmg and .zip (dist.sh notarize)
#
# Needs the Xcode command line tools (xcode-select --install), CMake 3.24+ and
# Ninja (brew install cmake ninja).
set -e
cd "$(dirname "$0")"

PRISM=ON
DIST=
DIST_ARGS=
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
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

for tool in cmake ninja; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Error: $tool is required (brew install cmake ninja)."
        exit 1
    fi
done

# The first run fetches and builds wxWidgets, Prism and the rest into build/
# (build-native/ for native),
# which takes a while; later runs reuse them. A running FastChat keeps running
# from the old copy; quit and reopen it to use the new one.
cmake -S . -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTCHAT_PRISM=$PRISM "-DCMAKE_OSX_ARCHITECTURES=$ARCHS"
cmake --build $BUILD $TARGETS

if [ -n "$DIST" ]; then
    ./dist.sh $DIST_ARGS
fi

echo
echo "Build successful. Open FastChat.app to start."
