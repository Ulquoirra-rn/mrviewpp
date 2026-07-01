#!/bin/bash
# Package the freshly-built bin/mrview into a native macOS "mrview++.app" bundle.
#
# WHY: homebrew qt@5.15.19's QMacStyle renders degraded (Fusion-like) on recent
# macOS. The official mrview install ships a Qt 5.15.17 (at /usr/local/mrtrix3)
# whose macStyle still renders natively. So we build the bundle around our fork's
# binary but re-point its Qt references at that working Qt + plugins.
#
# Requires the official mrtrix3 macOS install at /usr/local/mrtrix3.
# Re-run after each `./build bin/mrview`. Launch with:  ./mrview++ <image> ...
set -e
cd "$(dirname "$0")"

APP="mrview++.app"
EXE="mrview++"
SYS_QT_LIB="/usr/local/mrtrix3/lib"
SYS_QT_PLUGINS="/usr/local/mrtrix3/bin/plugins"

[ -x bin/mrview ]          || { echo "bin/mrview not found - build it first"; exit 1; }
[ -f lib/libmrtrix.dylib ] || { echo "lib/libmrtrix.dylib not found"; exit 1; }
[ -d "$SYS_QT_LIB" ]       || { echo "official Qt not found at $SYS_QT_LIB (install official mrtrix3)"; exit 1; }

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/lib" "$APP/Contents/PlugIns" "$APP/Contents/Resources"
cp bin/mrview "$APP/Contents/MacOS/$EXE"
# App icon: the mrview brain with a "++" badge (see icons/mrview++.icns).
[ -f icons/mrview++.icns ] && cp "icons/mrview++.icns" "$APP/Contents/Resources/$EXE.icns"
[ -f icons/mrtrix.png ] && cp icons/mrtrix.png "$APP/Contents/Resources/$EXE.png"
# qt.conf so Qt finds the bundled plugins (works for double-click / `open` too).
printf '[Paths]\nPlugins = PlugIns\n' > "$APP/Contents/Resources/qt.conf"

# Bundle the working Qt (+ its support dylibs) from the official install, then
# overwrite libmrtrix with OUR fork's build. The binary's existing rpath is
# @loader_path/../lib (= Contents/lib), so everything resolves there.
echo "Bundling working Qt 5.15.17 from $SYS_QT_LIB ..."
cp -a "$SYS_QT_LIB"/*.dylib "$APP/Contents/lib/" 2>/dev/null || true
cp lib/libmrtrix.dylib "$APP/Contents/lib/libmrtrix.dylib"   # our fork's core lib
# Bundle the Qt plugins (cocoa platform + macstyle) so the app is self-contained.
cp -a "$SYS_QT_PLUGINS"/. "$APP/Contents/plugins/" 2>/dev/null || true

# Re-point each homebrew qt@5 framework reference to @rpath (-> Contents/lib).
repoint_qt() {
  local f="$1"
  otool -L "$f" | awk '/qt@5/{print $1}' | while read -r ref; do
    mod=$(basename "$ref")                 # e.g. QtWidgets
    install_name_tool -change "$ref" "@rpath/libQt5${mod#Qt}.5.dylib" "$f" 2>/dev/null || true
  done
}
echo "Re-pointing Qt references to the bundled Qt ..."
repoint_qt "$APP/Contents/MacOS/$EXE"
repoint_qt "$APP/Contents/lib/libmrtrix.dylib"

# macOS selects native control rendering by the SDK the app was built against.
# The macOS 26 SDK gives the new AppKit control metrics, which mis-render mrview's
# compact tool buttons; the official build targets an older SDK (15.5) and gets
# the legacy rendering. Rewrite the recorded build version to match so macOS uses
# the same (correct) control rendering. (codesign below re-signs after this.)
if command -v vtool >/dev/null 2>&1; then
  echo "Setting legacy SDK build version (macOS 11.0 / SDK 15.5) ..."
  vtool -arch arm64 -set-build-version macos 11.0 15.5 -replace \
    -output "$APP/Contents/MacOS/$EXE.tmp" "$APP/Contents/MacOS/$EXE" 2>/dev/null \
    && mv "$APP/Contents/MacOS/$EXE.tmp" "$APP/Contents/MacOS/$EXE"
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>$EXE</string>
  <key>CFBundleIdentifier</key><string>ai.brainsight.mrviewpp</string>
  <key>CFBundleName</key><string>mrview++</string>
  <key>CFBundleDisplayName</key><string>mrview++</string>
  <key>CFBundleIconFile</key><string>$EXE.icns</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>3.0</string>
  <key>CFBundleVersion</key><string>3.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>10.14</string>
  <key>NSPrincipalClass</key><string>NSApplication</string>
</dict>
</plist>
PLIST

codesign --force --deep --sign - "$APP" >/dev/null 2>&1 && echo "signed OK" || echo "codesign failed"
xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true

# Launcher: point Qt at the working plugins (cocoa + macstyle) and exec the
# bundled binary directly (native rendering, no Gatekeeper dialog, passes args).
BUNDLE_ABS="$(pwd)/$APP"
cat > "$EXE" <<LAUNCH
#!/bin/bash
export QT_PLUGIN_PATH="$BUNDLE_ABS/Contents/plugins"
exec "$BUNDLE_ABS/Contents/MacOS/$EXE" "\$@"
LAUNCH
chmod +x "$EXE"

echo "Built $APP and launcher ./$EXE"
echo "Run:  ./$EXE <image.nii> [options]"
