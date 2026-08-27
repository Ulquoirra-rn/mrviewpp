#!/bin/bash
# Build a self-contained mrview++ AppImage on Ubuntu/Linux.
#
# Prerequisites (Ubuntu 22.04+):
#   sudo apt-get install -y g++ python3 zlib1g-dev libeigen3-dev \
#       qtbase5-dev libqt5opengl5-dev libqt5svg5-dev \
#       libtiff-dev libpng-dev libjpeg-dev libopenjp2-7-dev \
#       libfftw3-dev libgl1-mesa-dev wget file
#
# Then, from the repo root:
#   ./configure            # detects Qt5 + the image libs
#   ./build bin/mrview     # builds bin/mrview and lib/libmrtrix*.so
#   packaging/linux/make_appimage.sh
#
# Produces:  mrview++-x86_64.AppImage  (runs on most modern Linux distros)
set -e
cd "$(dirname "$0")/../.."

[ -x bin/mrview ] || { echo "bin/mrview not found - run ./configure && ./build bin/mrview first"; exit 1; }

APPDIR=AppDir
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/512x512/apps"

cp bin/mrview "$APPDIR/usr/bin/mrview++"
# Built-in tract atlas: antsRegistration beside the executable, data one level up
# in share/mrtrix3/mrviewpp - both are where AtlasTemplate/data_path search.
[ -x bin/antsRegistration ] && cp bin/antsRegistration "$APPDIR/usr/bin/"
mkdir -p "$APPDIR/usr/share/mrtrix3/mrviewpp"
cp share/mrtrix3/mrviewpp/* "$APPDIR/usr/share/mrtrix3/mrviewpp/" 2>/dev/null || true
[ -f packaging/ANTS_LICENSE.txt ] && cp packaging/ANTS_LICENSE.txt "$APPDIR/usr/share/"
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

# linuxdeploy pulls in Qt + all dependent libraries and emits the AppImage.
get () { [ -f "$1" ] || wget -q -O "$1" "$2"; chmod +x "$1"; }
get linuxdeploy-x86_64.AppImage        https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
get linuxdeploy-plugin-qt-x86_64.AppImage https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage

export OUTPUT="mrview++-x86_64.AppImage"
./linuxdeploy-x86_64.AppImage --appdir "$APPDIR" --plugin qt --output appimage \
    --desktop-file "$APPDIR/usr/share/applications/mrview++.desktop" \
    --icon-file    "$APPDIR/usr/share/icons/hicolor/512x512/apps/mrview++.png"

echo "Created $OUTPUT"
echo "Install: chmod +x $OUTPUT && ./$OUTPUT   (or move it anywhere and double-click)"
