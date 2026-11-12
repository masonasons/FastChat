#!/bin/sh
# Builds FastChat.app (Release, for Apple silicon and Intel Macs) into this folder.
#   ./build.sh           FastChat
#   ./build.sh tests     FastChat and the core tests (build/fastchat_tests)
#   ./build.sh no-prism  FastChat without Prism (no speech)
#   ./build.sh native    only for this Mac's processor (a faster build)
#   ./build.sh dist      FastChat, then FastChat-mac.zip (dist.sh)
#
# Needs the Xcode command line tools (xcode-select --install), CMake 3.24+ and
# Ninja (brew install cmake ninja).
set -e
cd "$(dirname "$0")"

PRISM=ON
DIST=
TARGETS="--target FastChat"
ARCHS="arm64;x86_64"
for arg in "$@"; do
    case "$arg" in
        no-prism) PRISM=OFF ;;
        tests) TARGETS="--target FastChat --target fastchat_tests" ;;
        native) ARCHS="$(uname -m)" ;;
        dist) DIST=1 ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

for tool in cmake ninja; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Error: $tool is required (brew install cmake ninja)."
        exit 1
    fi
done

# The first run fetches and builds wxWidgets, Prism and the rest into build/,
# which takes a while; later runs reuse them. A running FastChat keeps running
# from the old copy; quit and reopen it to use the new one.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTCHAT_PRISM=$PRISM "-DCMAKE_OSX_ARCHITECTURES=$ARCHS"
cmake --build build $TARGETS

if [ -n "$DIST" ]; then
    ./dist.sh
fi

echo
echo "Build successful. Open FastChat.app to start."
