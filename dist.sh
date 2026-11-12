#!/bin/sh
# Packages FastChat for others: FastChat-mac.zip with FastChat.app and the readme.
# FastChat.app needs nothing else: wxWidgets and Prism are linked into it, and
# everything it loads comes with macOS. It keeps its data in
# ~/Library/Application Support/FastChat.
#
# The app is signed ad hoc, not by an Apple developer, so on another Mac it
# opens the first time with Control-click (or right-click) > Open.
set -e
cd "$(dirname "$0")"

if [ ! -d FastChat.app ]; then
    echo "Error: FastChat.app not found. Run ./build.sh first."
    exit 1
fi

ZIPNAME=FastChat-mac.zip
rm -rf dist_temp "$ZIPNAME"
mkdir -p dist_temp/FastChat
ditto FastChat.app dist_temp/FastChat/FastChat.app
cp README.md dist_temp/FastChat/
echo "Creating $ZIPNAME..."
# ditto keeps the bundle's signature and attributes intact, unlike zip.
ditto -c -k --norsrc --keepParent dist_temp/FastChat "$ZIPNAME"
rm -rf dist_temp

echo
echo "Created $ZIPNAME ($(du -h "$ZIPNAME" | cut -f1 | tr -d ' ')):"
unzip -l "$ZIPNAME" | grep -E "MacOS/FastChat$|FastChat/README.md$" | awk '{printf "  %s (%d KB)\n", $4, $1/1024}'
