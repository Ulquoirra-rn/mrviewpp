# mrview++ installers

Out-of-the-box installers for the three platforms. Because each installer must
be built with the **native** toolchain (Qt + OpenGL + the MRtrix3 build system),
they cannot be cross-built from a single machine — build each on its own OS, or
let the GitHub Actions workflow (`.github/workflows/installers.yml`) build all
three and upload them as artifacts.

| Platform | Output | How |
|----------|--------|-----|
| macOS    | `mrview++.dmg` (drag to Applications) | `./make_macos_dmg.sh` |
| Ubuntu/Linux | `mrview++-x86_64.AppImage` | `packaging/linux/make_appimage.sh` |
| Windows  | `mrview++-setup.exe` | build `dist\`, then `iscc packaging/windows/mrviewpp.iss` |

All three bundle Qt and every image/codec dependency (tiff, png, jpeg,
openjpeg), so the end user needs nothing pre-installed.

## macOS  (verified)
```
./configure ...            # (already configured in this repo)
./build bin/mrview
./make_macos_dmg.sh        # -> mrview++.dmg, fully self-contained
```

## Ubuntu / Linux
```
sudo apt-get install -y g++ python3 zlib1g-dev libeigen3-dev \
    qtbase5-dev libqt5opengl5-dev libqt5svg5-dev \
    libtiff-dev libpng-dev libjpeg-dev libopenjp2-7-dev libgl1-mesa-dev wget file
./configure
./build bin/mrview
packaging/linux/make_appimage.sh   # -> mrview++-x86_64.AppImage
```

## Windows
Build with MSYS2 MinGW64 (see the MRtrix3 Windows build docs), stage a
self-contained `dist\` folder (see the header of `packaging/windows/mrviewpp.iss`),
then run Inno Setup:
```
iscc packaging\windows\mrviewpp.iss   # -> Output\mrview++-setup.exe
```

## Building the Linux AppImage from macOS via Docker
`make_linux_appimage_docker.sh` builds inside an amd64 `ubuntu:22.04` container.
On **Intel Macs / native x86_64 Docker hosts** this produces the AppImage
end-to-end. On **Apple Silicon** it successfully **compiles the Linux binary**
(under emulation), but the AppImage packaging tools (`linuxdeploy`,
`appimagetool`) are static-PIE AppImages that the amd64 emulation layer refuses
to `exec` ("Exec format error"), so the final `.AppImage` cannot be assembled
locally there. Use the CI workflow (native x86_64 runner) to get the AppImage
on Apple Silicon.

## CI
`.github/workflows/installers.yml` runs the three recipes on
`macos-latest`, `ubuntu-latest` and `windows-latest` and uploads the `.dmg`,
`.AppImage` and `-setup.exe` as build artifacts. It is a starting point:
only the macOS path is verified locally; the Linux/Windows jobs may need
dependency tweaks for your runner images.
