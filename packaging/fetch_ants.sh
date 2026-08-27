#!/usr/bin/env bash
# Fetch the antsRegistration executable for one platform.
#
# MRView++ aligns the built-in tract atlas with antsRegistration. Only that one
# executable is needed and it is statically linked (the release ships no shared
# libraries at all), so rather than downloading a 400-800 MB release archive we
# read the zip's central directory over HTTP range requests and inflate the
# single entry. That costs about the size of the binary itself.
#
# Usage: fetch_ants.sh <platform-key> <destination-directory>
#   platform-key: a substring of the release asset name, e.g.
#                 macos-14-ARM64, macos-15-intel, ubuntu-22.04, windows-2022
set -euo pipefail

if [ $# -ne 2 ]; then
  echo "usage: $0 <platform-key> <destination-directory>" >&2
  exit 2
fi
PLATFORM="$1"
DEST="$2"
mkdir -p "$DEST"

python3 - "$PLATFORM" "$DEST" <<'PYTHON'
import json, os, stat, struct, sys, urllib.request, zlib

platform, dest = sys.argv[1], sys.argv[2]
headers = {"User-Agent": "mrviewpp-packaging"}
token = os.environ.get("GITHUB_TOKEN")
if token:
    headers["Authorization"] = "Bearer " + token

def fetch(url, extra=None):
    h = dict(headers)
    if extra:
        h.update(extra)
    return urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=300)

release = json.load(fetch("https://api.github.com/repos/ANTsX/ANTs/releases/latest"))
assets = [a for a in release["assets"] if platform in a["name"] and a["name"].endswith(".zip")]
if not assets:
    sys.exit("no ANTs asset matching '%s' in release %s" % (platform, release.get("tag_name")))
asset = assets[0]
url, total = asset["browser_download_url"], asset["size"]
print("ANTs %s: %s (%.0f MB)" % (release.get("tag_name"), asset["name"], total / 1e6))

def get(start, end):
    return fetch(url, {"Range": "bytes=%d-%d" % (start, end)}).read()

# End of central directory, then the central directory itself.
tail = get(max(0, total - 70000), total - 1)
eocd = tail.rfind(b"PK\x05\x06")
if eocd < 0:
    sys.exit("could not locate the zip central directory")
cd_size, cd_offset = struct.unpack("<II", tail[eocd + 12: eocd + 20])
cd = get(cd_offset, cd_offset + cd_size - 1)

target = None
pos = 0
while pos + 46 <= len(cd) and cd[pos:pos + 4] == b"PK\x01\x02":
    method, = struct.unpack("<H", cd[pos + 10: pos + 12])
    csize, usize = struct.unpack("<II", cd[pos + 20: pos + 28])
    nlen, elen, clen = struct.unpack("<HHH", cd[pos + 28: pos + 34])
    local = struct.unpack("<I", cd[pos + 42: pos + 46])[0]
    name = cd[pos + 46: pos + 46 + nlen].decode("utf-8", "replace")
    base = name.rstrip("/").rsplit("/", 1)[-1]
    if base in ("antsRegistration", "antsRegistration.exe"):
        target = (base, method, csize, usize, local)
    pos += 46 + nlen + elen + clen

if not target:
    sys.exit("antsRegistration not found in %s" % asset["name"])
base, method, csize, usize, local = target

# The local header repeats the name/extra lengths, which the central directory's
# copy does not reliably match; read it to find where the data actually starts.
lh = get(local, local + 29)
nlen2, elen2 = struct.unpack("<HH", lh[26:30])
data = get(local + 30 + nlen2 + elen2, local + 30 + nlen2 + elen2 + csize - 1)
raw = zlib.decompress(data, -15) if method == 8 else data
if len(raw) != usize:
    sys.exit("inflated size %d does not match the expected %d" % (len(raw), usize))

out = os.path.join(dest, base)
with open(out, "wb") as f:
    f.write(raw)
os.chmod(out, 0o755)
print("wrote %s (%.1f MB)" % (out, len(raw) / 1e6))
PYTHON
