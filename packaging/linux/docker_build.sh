#!/bin/bash
# Runs INSIDE an amd64 ubuntu:22.04 container to build the mrview++ AppImage.
# The repo is mounted read-only-ish at /src; we copy the sources to /build so we
# never clobber the host's (macOS) build artifacts, then emit the AppImage back
# to /src.  Invoked by make_linux_appimage_docker.sh.
set -e
export DEBIAN_FRONTEND=noninteractive

echo "==> installing build + packaging dependencies"
apt-get update -qq
apt-get install -y --no-install-recommends \
  g++ make python3 python-is-python3 pkg-config git ca-certificates file wget desktop-file-utils \
  zlib1g-dev libeigen3-dev \
  qtbase5-dev qtbase5-dev-tools qt5-qmake libqt5opengl5-dev libqt5svg5-dev \
  libtiff-dev libpng-dev libjpeg-dev libopenjp2-7-dev \
  libgl1-mesa-dev libglu1-mesa-dev libfontconfig1 libxkbcommon0 >/dev/null

echo "==> copying sources to /build (excluding host build outputs)"
mkdir -p /build
# keep lib/ (holds the mrtrix3 python package); only drop mac build outputs
tar -C /src --exclude=./tmp --exclude=./bin --exclude=./lib/libmrtrix.dylib \
    --exclude=./release --exclude=./.git --exclude='*.dmg' --exclude='*.app' \
    --exclude=./dcmsamples --exclude=./dcmvenv -cf - . | tar -C /build -xf -
cd /build

echo "==> configuring"
python3 ./configure

echo "==> building bin/mrview (this is slow under emulation) ..."
python3 ./build bin/mrview

echo "==> packaging AppImage"
export APPIMAGE_EXTRACT_AND_RUN=1     # no FUSE inside the container
export QMAKE=/usr/bin/qmake
bash packaging/linux/make_appimage.sh

cp -f mrview++-x86_64.AppImage /src/
echo "==> DONE: /src/mrview++-x86_64.AppImage"
