#!/usr/bin/bash

if [[ -n "$1" ]]; then
    VERSION="$1"
else
    VERSION=$(grep -m1 '^CMAKE_PROJECT_VERSION:' build/CMakeCache.txt | cut -d= -f2)
fi

cd bin

echo "Running macdeployqt"
macdeployqt iqView.app

# Copy AI scripts to bundle
cp -r ../scripts iqView.app/Contents/Resources/scripts

IMF_DIR=iqView.app/Contents/PlugIns/imageformats
if [[ (-f "$IMF_DIR/kimg_heif.dylib" || -f "$IMF_DIR/kimg_heif.so") && -f "$IMF_DIR/libqmacheif.dylib" ]]; then
    # Prefer kimageformats HEIF plugin for proper color space handling
    echo "Removing duplicate HEIF plugin"
    rm "$IMF_DIR/libqmacheif.dylib"
fi
if [[ (-f "$IMF_DIR/kimg_tga.dylib" || -f "$IMF_DIR/kimg_tga.so") && -f "$IMF_DIR/libqtga.dylib" ]]; then
    # Prefer kimageformats TGA plugin which supports more formats
    echo "Removing duplicate TGA plugin"
    rm "$IMF_DIR/libqtga.dylib"
fi

echo "Running codesign"
if [[ "$APPLE_NOTARIZE_REQUESTED" == "true" ]]; then
    APP_IDENTIFIER=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "iqView.app/Contents/Info.plist")
    codesign --sign "$CODESIGN_CERT_NAME" --deep --force --options runtime --timestamp "iqView.app"
else
    codesign --sign "$CODESIGN_CERT_NAME" --deep --force "iqView.app"
fi

echo "Creating disk image"
if [[ -n "$1" ]]; then
    BUILD_NAME=iqView-nightly-$1
    DMG_FILENAME=$BUILD_NAME.dmg
    mv iqView.app "$BUILD_NAME.app"
    hdiutil create -volname "$BUILD_NAME" -srcfolder "$BUILD_NAME.app" -fs HFS+ "$DMG_FILENAME"
else
    DMG_FILENAME=iqView-$VERSION.dmg
    brew install create-dmg
    create-dmg --volname "iqView $VERSION" --window-size 660 400 --icon-size 160 --icon "iqView.app" 180 170 --hide-extension iqView.app --app-drop-link 480 170 "$DMG_FILENAME" "iqView.app"
fi
if [[ "$APPLE_NOTARIZE_REQUESTED" == "true" ]]; then
    codesign --sign "$CODESIGN_CERT_NAME" --timestamp --identifier "$APP_IDENTIFIER.dmg" "$DMG_FILENAME"
    xcrun notarytool submit "$DMG_FILENAME" --apple-id "$APPLE_ID_USER" --password "$APPLE_ID_PASS" --team-id "${CODESIGN_CERT_NAME: -11:10}" --wait
    xcrun stapler staple "$DMG_FILENAME"
    xcrun stapler validate "$DMG_FILENAME"
fi

rm -r *.app