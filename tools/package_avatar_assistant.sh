#!/bin/sh
set -eu
# Run from this project root after the XcodeBuildMCP Swift package build/test.
test -f macos/RoundTextBridge/.build/release/RoundTextBridge
mkdir -p macos/RoundTextBridge/RoundAvatar.app/Contents/MacOS
cp macos/RoundTextBridge/.build/release/RoundTextBridge macos/RoundTextBridge/RoundAvatar.app/Contents/MacOS/RoundTextBridge
cp macos/RoundTextBridge/Avatar-Info.plist macos/RoundTextBridge/RoundAvatar.app/Contents/Info.plist
codesign --force --sign - macos/RoundTextBridge/RoundAvatar.app
echo 'Built macos/RoundTextBridge/RoundAvatar.app (local ad-hoc signature).'
