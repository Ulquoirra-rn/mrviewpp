#!/bin/bash
# Build a SELF-CONTAINED Debian/Ubuntu .deb for mrview++.
#
# Qt, the Qt platform plugins, and every image codec (tiff, png, jpeg, openjpeg)
# are bundled INSIDE the package under /opt/mrview++, so the target machine needs
# nothing pre-installed - no MRtrix, no Qt. Only the universal system libraries
# that ship with every desktop Linux (glibc, libGL, core X11) are relied upon.
#
# It uses the same bundler as the AppImage (linuxdeploy + its Qt plugin) to
# collect all dependencies, then lays that tree into a .deb with a small launcher
# on the PATH.
#
# Prerequisites (Debian/Ubuntu):
#   sudo apt-get install -y g++ python3 zlib1g-dev libeigen3-dev \
#       qtbase5-dev libqt5opengl5-dev libqt5svg5-dev \
#       libtiff-dev libpng-dev libjpeg-dev libopenjp2-7-dev libgl1-mesa-dev \
#       dpkg-dev wget file
#
# Then, from the repo root:
#   ./configure
#   ./build bin/mrview
#   packaging/linux/make_deb.sh
#
# Produces:  mrview++_<version>_<arch>.deb
#   Install with:  sudo apt install ./mrview++_<version>_<arch>.deb
#   (or:           sudo dpkg -i ./mrview++_<version>_<arch>.deb)
set -e
cd "$(dirname "$0")/../.."

[ -x bin/mrview ] || { echo "bin/mrview not found - run ./configure && ./build bin/mrview first"; exit 1; }
command -v dpkg-deb >/dev/null 2>&1 || { echo "dpkg-deb not found - install 'dpkg-dev'"; exit 1; }

# Version from the nearest git tag (strip a leading 'v'); fall back to 3.0.0.
VERSION="$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')"
[ -n "$VERSION" ] || VERSION="3.0.0"
ARCH="$(dpkg --print-architecture)"

# ---------------------------------------------------------------------------
# 1. Populate an AppDir with the binary + all bundled libraries and Qt plugins,
#    using linuxdeploy (same tool as the AppImage). We stop short of emitting an
#    AppImage - we only want the fully self-contained tree.
# ---------------------------------------------------------------------------
APPDIR=AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/512x512/apps"

cp bin/mrview "$APPDIR/usr/bin/mrview++"
cp -a lib/*.so* "$APPDIR/usr/lib/" 2>/dev/null || true
cp icons/mrview++.png "$APPDIR/usr/share/icons/hicolor/512x512/apps/mrview++.png"

cat > "$APPDIR/usr/share/applications/mrview++.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=mrview++
GenericName=Medical Image Viewer
Exec=mrview++ %F
Icon=mrview++
Categories=Science;MedicalSoftware;
Terminal=false
EOF

get () { [ -f "$1" ] || wget -q -O "$1" "$2"; chmod +x "$1"; }
get linuxdeploy-x86_64.AppImage           https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
get linuxdeploy-plugin-qt-x86_64.AppImage https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage

# Deploy Qt + all dependent libraries into AppDir (no --output: keep the tree).
./linuxdeploy-x86_64.AppImage --appdir "$APPDIR" --plugin qt \
    --desktop-file "$APPDIR/usr/share/applications/mrview++.desktop" \
    --icon-file    "$APPDIR/usr/share/icons/hicolor/512x512/apps/mrview++.png"

# ---------------------------------------------------------------------------
# 2. Lay the bundled tree into a .deb under /opt/mrview++, with a launcher that
#    points the loader and Qt at the bundled libraries/plugins.
# ---------------------------------------------------------------------------
PKG="mrview++_${VERSION}_${ARCH}"
ROOT="$PKG"
rm -rf "$ROOT"
mkdir -p "$ROOT/DEBIAN" "$ROOT/opt" "$ROOT/usr/bin" \
         "$ROOT/usr/share/applications" \
         "$ROOT/usr/share/icons/hicolor/512x512/apps"

# Ship the ENTIRE bundled AppDir - including linuxdeploy's AppRun. AppRun sets
# up LD_LIBRARY_PATH and the Qt plugin paths to the bundled Qt exactly right, so
# the system Qt is never loaded (avoids "cannot mix incompatible Qt library").
cp -a "$APPDIR" "$ROOT/opt/mrview++"
[ -x "$ROOT/opt/mrview++/AppRun" ] || { echo "ERROR: linuxdeploy did not produce AppRun in $APPDIR"; exit 1; }

cp "$APPDIR/usr/share/icons/hicolor/512x512/apps/mrview++.png" \
   "$ROOT/usr/share/icons/hicolor/512x512/apps/mrview++.png"
cp "$APPDIR/usr/share/applications/mrview++.desktop" \
   "$ROOT/usr/share/applications/mrview++.desktop"

# Launcher on the PATH delegates to the bundled AppRun (fully self-contained Qt).
cat > "$ROOT/usr/bin/mrview++" <<'EOF'
#!/bin/sh
exec /opt/mrview++/AppRun "$@"
EOF
chmod 0755 "$ROOT/usr/bin/mrview++"

# Qt + codecs are bundled, so only the universal desktop libraries are declared.
cat > "$ROOT/DEBIAN/control" <<EOF
Package: mrview++
Version: ${VERSION}
Section: science
Priority: optional
Architecture: ${ARCH}
Maintainer: BrainSight AI <aryan.tiwary@brainsightai.com>
Depends: libc6, libgl1, libglib2.0-0, libx11-6
Description: mrview++ medical image viewer (self-contained)
 A native cross-platform fork of MRtrix3's mrview for viewing MRI/medical
 images, overlays, tractograms, meshes and atlases. Qt and all image codecs
 are bundled, so no MRtrix or Qt installation is required on the target system.
EOF

dpkg-deb --build --root-owner-group "$ROOT" "${PKG}.deb"
rm -rf "$ROOT" "$APPDIR"

echo "Created ${PKG}.deb  (self-contained: Qt + codecs bundled)"
echo "Install with:  sudo apt install ./${PKG}.deb"
