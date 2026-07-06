#!/bin/bash
# Build a Debian/Ubuntu .deb package for mrview++.
#
# Qt and the image codecs are NOT bundled - they are declared as apt
# dependencies, so apt pulls a matching system Qt on install. This keeps the
# package small and, crucially, avoids the "cannot mix incompatible Qt library"
# problem that bundling causes on machines with more than one Qt: with the
# system Qt only, a single consistent Qt is ever loaded. (On Linux the system Qt
# renders mrview correctly - the bundled-Qt workaround is only needed on macOS.)
#
# Only our own libmrtrix is shipped inside the package.
#
# IMPORTANT: build mrview against the DISTRO Qt so the binary matches the Qt that
# apt installs on the target, e.g.:
#     export QMAKE="$(command -v qmake-qt5 || echo /usr/lib/qt5/bin/qmake)"
#     ./configure && ./build bin/mrview
#
# Prerequisites (Debian/Ubuntu):
#   sudo apt-get install -y g++ python3 zlib1g-dev libeigen3-dev \
#       qtbase5-dev libqt5opengl5-dev libqt5svg5-dev qt5-qmake \
#       libtiff-dev libpng-dev libjpeg-dev libopenjp2-7-dev libgl1-mesa-dev \
#       dpkg-dev
#
# Then, from the repo root:
#   ./configure
#   ./build bin/mrview
#   packaging/linux/make_deb.sh
#
# Produces:  mrview++_<version>_<arch>.deb
#   Install with:  sudo apt install ./mrview++_<version>_<arch>.deb
set -e
cd "$(dirname "$0")/../.."

[ -x bin/mrview ] || { echo "bin/mrview not found - run ./configure && ./build bin/mrview first"; exit 1; }
command -v dpkg-deb >/dev/null 2>&1 || { echo "dpkg-deb not found - install 'dpkg-dev'"; exit 1; }

# Version from the nearest git tag (strip a leading 'v'); fall back to 3.0.0.
VERSION="$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')"
[ -n "$VERSION" ] || VERSION="3.0.0"
ARCH="$(dpkg --print-architecture)"

PKG="mrview++_${VERSION}_${ARCH}"
ROOT="$PKG"
rm -rf "$ROOT"
mkdir -p "$ROOT/DEBIAN" \
         "$ROOT/usr/bin" \
         "$ROOT/usr/lib/mrview++" \
         "$ROOT/usr/share/applications" \
         "$ROOT/usr/share/icons/hicolor/512x512/apps"

# The ELF + our own core library live in a private dir; a wrapper on the PATH
# points the loader at it (Qt itself comes from the system packages).
cp bin/mrview "$ROOT/usr/lib/mrview++/mrview"
cp -a lib/*.so* "$ROOT/usr/lib/mrview++/" 2>/dev/null || true

cat > "$ROOT/usr/bin/mrview++" <<'EOF'
#!/bin/sh
export LD_LIBRARY_PATH="/usr/lib/mrview++:${LD_LIBRARY_PATH}"
exec /usr/lib/mrview++/mrview "$@"
EOF
chmod 0755 "$ROOT/usr/bin/mrview++"

cp icons/mrview++.png "$ROOT/usr/share/icons/hicolor/512x512/apps/mrview++.png"

cat > "$ROOT/usr/share/applications/mrview++.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=mrview++
GenericName=Medical Image Viewer
Exec=mrview++ %F
Icon=mrview++
Categories=Science;MedicalSoftware;
Terminal=false
EOF

# Runtime dependencies: compute them from the actual linked libraries with
# dpkg-shlibdeps so the package declares the CORRECT system-Qt package names for
# whatever distro it is built on (Qt is 'libqt5core5a' on Ubuntu 22.04 but
# 'libqt5core5t64' on 24.04, etc.). Fall back to a 22.04-style list if the tool
# isn't available. Build on the distro you target so the binary + deps + system
# Qt all match (this uses the system Qt - nothing is bundled).
DEPS=""
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
  ABSROOT="$PWD/$ROOT"
  TMPD="$(mktemp -d)"
  mkdir -p "$TMPD/debian"
  printf 'Source: mrview++\nPackage: mrview++\nArchitecture: any\n' > "$TMPD/debian/control"
  ( cd "$TMPD" && dpkg-shlibdeps -O --ignore-missing-info \
      "$ABSROOT/usr/lib/mrview++/mrview" "$ABSROOT"/usr/lib/mrview++/*.so* 2>/dev/null ) > "$TMPD/deps.txt" || true
  DEPS="$(sed -n 's/^shlibs:Depends=//p' "$TMPD/deps.txt")"
  rm -rf "$TMPD"
fi
[ -n "$DEPS" ] || DEPS="libc6, libstdc++6, libgcc-s1, zlib1g, libgl1, libqt5core5a, libqt5gui5, libqt5widgets5, libqt5opengl5, libqt5svg5, libtiff5, libpng16-16, libjpeg-turbo8, libopenjp2-7"
echo "computed Depends: $DEPS"

cat > "$ROOT/DEBIAN/control" <<EOF
Package: mrview++
Version: ${VERSION}
Section: science
Priority: optional
Architecture: ${ARCH}
Maintainer: BrainSight AI <aryan.tiwary@brainsightai.com>
Depends: ${DEPS}
Description: mrview++ medical image viewer
 A native cross-platform fork of MRtrix3's mrview for viewing MRI/medical
 images, overlays, tractograms, meshes and atlases. Qt and the image codecs
 are pulled from the system packages (no MRtrix install required).
EOF

dpkg-deb --build --root-owner-group "$ROOT" "${PKG}.deb"
rm -rf "$ROOT"

echo "Created ${PKG}.deb"
echo "Install with:  sudo apt install ./${PKG}.deb"
