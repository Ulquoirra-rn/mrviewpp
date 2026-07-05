#!/bin/bash
# One command to build the native installer for whatever OS you run it on:
#
#   macOS    ->  mrview++.dmg                     (drag to Applications)
#   Linux    ->  mrview++_<version>_<arch>.deb    (sudo apt install ./...)
#   Windows  ->  packaging/windows/Output/mrview++-setup.exe   (MSYS2 MinGW64)
#
# It builds bin/mrview first if needed, then packages. Each OS must be built on
# its own machine (the native Qt/OpenGL toolchain can't be cross-built).
#
# Usage:   ./make_installer.sh
set -e
cd "$(dirname "$0")"

OS="$(uname -s)"

# Windows uses a .exe (bin/mrview.exe); every other platform builds bin/mrview.
case "$OS" in
  MINGW*|MSYS*|CYGWIN*) BIN="bin/mrview.exe" ;;
  *)                    BIN="bin/mrview" ;;
esac

if [ ! -x "$BIN" ]; then
  echo "==> $BIN not found; building it first"
  [ -f config ] || ./configure
  ./build bin/mrview
  [ -x "$BIN" ] || { echo "ERROR: build did not produce $BIN (check the compile output for 'error:')"; exit 1; }
fi

case "$OS" in
  Darwin)
    echo "==> macOS detected: building mrview++.dmg"
    ./make_macos_dmg.sh
    ;;

  Linux)
    echo "==> Linux detected: building .deb package"
    packaging/linux/make_deb.sh
    echo "(For a distro-independent single file instead, run packaging/linux/make_appimage.sh)"
    ;;

  MINGW*|MSYS*|CYGWIN*)
    echo "==> Windows detected: staging dist\\ and building the Inno Setup .exe"
    command -v iscc >/dev/null 2>&1 || command -v ISCC.exe >/dev/null 2>&1 || {
      echo "ERROR: Inno Setup (iscc) not found on PATH. Install it (e.g. 'choco install innosetup')."; exit 1; }
    rm -rf dist && mkdir -p dist
    cp bin/mrview.exe dist/mrview++.exe
    cp -u lib/libmrtrix.dll dist/ 2>/dev/null || cp -u bin/libmrtrix.dll dist/ 2>/dev/null || true
    WDQ="$(command -v windeployqt-qt5 || command -v windeployqt || true)"
    [ -n "$WDQ" ] && "$WDQ" --release --no-translations --no-angle --no-opengl-sw dist/mrview++.exe
    # copy any remaining non-system DLLs the exe still needs
    for dll in $(ldd dist/mrview++.exe 2>/dev/null | awk '{print $3}' | grep -viE '^/c/windows'); do
      [ -f "$dll" ] && cp -u "$dll" dist/ 2>/dev/null || true
    done
    ISCC="$(command -v iscc || command -v ISCC.exe)"
    "$ISCC" packaging/windows/mrviewpp.iss
    echo "Installer: packaging/windows/Output/mrview++-setup.exe"
    ;;

  *)
    echo "Unsupported OS: $OS"; exit 1 ;;
esac

echo "Done."
