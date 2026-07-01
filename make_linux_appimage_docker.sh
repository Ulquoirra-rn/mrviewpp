#!/bin/bash
# Build the Linux x86_64 AppImage from macOS (or any Docker host) using an
# amd64 Ubuntu 22.04 container. The resulting AppImage runs on x86_64 Linux
# systems with glibc >= 2.35 (Ubuntu 22.04+, recent Fedora/Debian, etc.).
#
# Requires Docker running. On Apple Silicon the amd64 build runs under emulation
# (slow — expect 30-90 min for the compile).
set -e
cd "$(dirname "$0")"

docker run --rm --platform linux/amd64 \
  -v "$PWD":/src \
  ubuntu:22.04 \
  bash /src/packaging/linux/docker_build.sh

echo "Created $(pwd)/mrview++-x86_64.AppImage"
