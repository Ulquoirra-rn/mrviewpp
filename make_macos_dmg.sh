#!/bin/bash
# Build a fully self-contained macOS installer (mrview++.dmg) that runs out of
# the box on a clean Mac (no Homebrew / MRtrix install required).
#
#   1. builds the .app via package_macos_app.sh (bundles Qt + libmrtrix)
#   2. BFS-bundles every remaining non-system dylib (libtiff, libpng, libjpeg,
#      libopenjp2 + their deps) into Contents/lib, rewriting refs to @rpath
#   3. re-signs (ad-hoc) and packages into a drag-to-Applications .dmg
set -e
cd "$(dirname "$0")"

APP="mrview++.app"
EXE="mrview++"
LIB="$APP/Contents/lib"
PLUGINS="$APP/Contents/plugins"

./package_macos_app.sh

echo "Bundling remaining non-system libraries ..."

copy_lib () {   # $1 = absolute dep path; copies into $LIB once, fixes id + rpath
  local dep="$1" base; base="$(basename "$dep")"
  [ -f "$LIB/$base" ] && return 0
  [ -f "$dep" ] || return 0
  cp -L "$dep" "$LIB/$base"; chmod u+w "$LIB/$base"
  install_name_tool -id "@rpath/$base" "$LIB/$base" 2>/dev/null || true
  otool -l "$LIB/$base" | grep -q "path @loader_path " || \
    install_name_tool -add_rpath "@loader_path" "$LIB/$base" 2>/dev/null || true
}

# BFS starting from the executable, libmrtrix and the Qt plugins. Each file is
# visited once (no repeated edits -> no corruption).
queue=( "$APP/Contents/MacOS/$EXE" "$LIB/libmrtrix.dylib" )
while IFS= read -r pl; do queue+=("$pl"); done < <(find "$PLUGINS" -type f -name "*.dylib" 2>/dev/null)

i=0
while [ $i -lt ${#queue[@]} ]; do
  f="${queue[$i]}"; i=$((i+1))
  [ -f "$f" ] || continue
  while IFS= read -r dep; do
    [ -z "$dep" ] && continue
    base="$(basename "$dep")"
    newlib=0; [ -f "$LIB/$base" ] || newlib=1
    copy_lib "$dep"
    install_name_tool -change "$dep" "@rpath/$base" "$f" 2>/dev/null || true
    [ $newlib -eq 1 ] && [ -f "$LIB/$base" ] && queue+=("$LIB/$base")
  done < <(otool -L "$f" 2>/dev/null | awk 'NR>1{print $1}' | grep -E '^/opt/|^/usr/local/')
done

leftover=$( { for f in "$APP/Contents/MacOS/$EXE" "$LIB"/*.dylib; do otool -L "$f" 2>/dev/null | awk 'NR>1{print $1}'; done; } | grep -E '^/opt/|^/usr/local/' | sort -u || true)
[ -n "$leftover" ] && { echo "WARNING: unbundled libs remain:"; echo "$leftover"; } || echo "All dependencies bundled."

# Re-sign each dylib, then the whole bundle (install_name_tool invalidates sigs).
for d in "$LIB"/*.dylib; do codesign --force -s - "$d" >/dev/null 2>&1 || true; done
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 && echo "signed OK" || echo "codesign failed"
xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true

echo "Building disk image ..."
STAGE="$(mktemp -d)"
cp -a "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$EXE.dmg"
hdiutil create -volname "mrview++" -srcfolder "$STAGE" -ov -format UDZO "$EXE.dmg" >/dev/null
rm -rf "$STAGE"
echo "Created $EXE.dmg  ($(du -h "$EXE.dmg" | cut -f1))"
