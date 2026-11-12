#!/bin/sh
# Packages FastChat for others.
#   ./dist.sh            FastChat-mac.zip with FastChat.app (signed ad hoc) and the readme
#   ./dist.sh notarize   FastChat-mac.dmg and FastChat-mac.zip, signed with a
#                        Developer ID and notarized by Apple, so they open
#                        everywhere without warnings
#
# FastChat.app needs nothing else: wxWidgets and Prism are linked into it, and
# everything it loads comes with macOS. It keeps its data in
# ~/Library/Application Support/FastChat.
#
# Without notarize the app is signed ad hoc, not by an Apple developer, so on
# another Mac it opens the first time with Control-click (or right-click) > Open.
#
# notarize needs:
#   - a "Developer ID Application" certificate in the keychain (Xcode > Settings >
#     Accounts > Manage Certificates, or developer.apple.com). The first one found
#     is used; set FASTCHAT_SIGN_ID to its name or hash to choose another.
#   - an App Store Connect API key for the notary service (the same key and
#     defaults as FastPlay's ios/testflight.sh): AuthKey_<key ID>.p8 in
#     ~/.appstoreconnect/private_keys, or ASC_KEY_ID, ASC_ISSUER_ID and
#     ASC_KEY_PATH set. Or set FASTCHAT_NOTARY_PROFILE to use credentials stored
#     with xcrun notarytool store-credentials instead.
# The signed copy is made in dist_temp; FastChat.app itself stays as built.
set -e
cd "$(dirname "$0")"

NOTARIZE=
for arg in "$@"; do
    case "$arg" in
        notarize) NOTARIZE=1 ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

if [ ! -d FastChat.app ]; then
    echo "Error: FastChat.app not found. Run ./build.sh first."
    exit 1
fi

ZIPNAME=FastChat-mac.zip
DMGNAME=FastChat-mac.dmg
rm -rf dist_temp "$ZIPNAME"
mkdir -p dist_temp/FastChat
ditto FastChat.app dist_temp/FastChat/FastChat.app
cp README.md dist_temp/FastChat/
APP=dist_temp/FastChat/FastChat.app

if [ -n "$NOTARIZE" ]; then
    SIGN_ID="$FASTCHAT_SIGN_ID"
    if [ -z "$SIGN_ID" ]; then
        SIGN_ID=$(security find-identity -v -p codesigning | grep '"Developer ID Application:' | head -1 | awk '{print $2}')
    fi
    if [ -z "$SIGN_ID" ]; then
        echo "Error: no Developer ID Application certificate in the keychain."
        exit 1
    fi
    if [ -n "$FASTCHAT_NOTARY_PROFILE" ]; then
        set -- --keychain-profile "$FASTCHAT_NOTARY_PROFILE"
    else
        KEY_ID="${ASC_KEY_ID:-FB9N292RPN}"
        ISSUER="${ASC_ISSUER_ID:-02117eeb-7d87-4d4d-bf11-850f80204c4c}"
        KEY_PATH="${ASC_KEY_PATH:-$HOME/.appstoreconnect/private_keys/AuthKey_${KEY_ID}.p8}"
        if [ ! -f "$KEY_PATH" ]; then
            echo "Error: the App Store Connect key is not at $KEY_PATH."
            exit 1
        fi
        set -- --key "$KEY_PATH" --key-id "$KEY_ID" --issuer "$ISSUER"
    fi
    rm -f "$DMGNAME"

    echo "Signing FastChat.app with $(security find-identity -v -p codesigning | grep "$SIGN_ID" | head -1 | sed 's/.*"\(.*\)"/\1/')..."
    # The hardened runtime and a secure timestamp are what notarization requires.
    codesign --force --sign "$SIGN_ID" --options runtime --timestamp \
        --entitlements res/mac/FastChat.entitlements "$APP"
    codesign --verify --strict --deep "$APP"

    echo "Creating $DMGNAME..."
    # The disk image shows the app beside a link to Applications, to drag it to.
    mkdir -p dist_temp/dmg
    ditto "$APP" dist_temp/dmg/FastChat.app
    cp README.md dist_temp/dmg/
    ln -s /Applications dist_temp/dmg/Applications
    hdiutil create -quiet -volname FastChat -srcfolder dist_temp/dmg -fs HFS+ -format UDZO -ov "$DMGNAME"
    codesign --force --sign "$SIGN_ID" --timestamp "$DMGNAME"

    echo "Notarizing $DMGNAME (this usually takes a few minutes)..."
    if ! xcrun notarytool submit "$DMGNAME" "$@" --wait --output-format plist > dist_temp/notary.plist; then
        cat dist_temp/notary.plist
        exit 1
    fi
    STATUS=$(/usr/libexec/PlistBuddy -c "Print :status" dist_temp/notary.plist)
    if [ "$STATUS" != "Accepted" ]; then
        ID=$(/usr/libexec/PlistBuddy -c "Print :id" dist_temp/notary.plist)
        echo "Error: notarization $STATUS. Apple's log:"
        xcrun notarytool log "$ID" "$@"
        rm -f "$DMGNAME"
        exit 1
    fi
    # Staple the tickets, so Gatekeeper accepts them without asking Apple online.
    # The disk image's notarization covers the app inside, so the app in the zip
    # takes a ticket too.
    xcrun stapler staple -q "$DMGNAME"
    xcrun stapler staple -q "$APP"
    spctl --assess --type open --context context:primary-signature "$DMGNAME"
    spctl --assess --type execute "$APP"
fi

echo "Creating $ZIPNAME..."
# ditto keeps the bundle's signature and attributes intact, unlike zip.
ditto -c -k --norsrc --keepParent dist_temp/FastChat "$ZIPNAME"
rm -rf dist_temp

echo
if [ -n "$NOTARIZE" ]; then
    echo "Created $DMGNAME ($(du -h "$DMGNAME" | cut -f1 | tr -d ' ')), notarized."
fi
echo "Created $ZIPNAME ($(du -h "$ZIPNAME" | cut -f1 | tr -d ' ')):"
unzip -l "$ZIPNAME" | grep -E "MacOS/FastChat$|FastChat/README.md$" | awk '{printf "  %s (%d KB)\n", $4, $1/1024}'
