#!/usr/bin/env python3
"""Pack a directory tract atlas into the single .trx file MRView++ ships.

Input is a tree of <category>/<bundle>.tck, all in the same (MNI) space. Output
is one TRX archive whose group names carry the category, e.g.
"groups/association/AF_L.uint32", which is what puts the two drop-downs in the
Track generation tool.

Converting to TRX alone saves almost nothing - both formats store float32
coordinates, and deflate barely compresses those. The size comes off in three
places, all of them lossless as far as MRView++ is concerned:

  * streamlines per bundle, capped: the shape matcher compresses a bundle to a
    few dozen centroids and the tracking territory saturates long before the
    last streamline, so a bundle of 34,000 (AF_L in the HCP1065 average) is
    tens of times more than anything reads;
  * vertex spacing: these atlases are written at 0.5 mm steps, well under the
    1-2 mm voxels anything here rasterises onto;
  * float16 positions, which TRX supports and MRView++ reads. At MNI
    coordinates (under 128 mm from the origin) the worst rounding is 0.03 mm.

Measured on HCP1065_avg (102 bundles, 662,012 streamlines, 1.43 GB of .tck):

    as-is, as TRX float32                        1424 MB
    as-is, float16                                713 MB
    cap 2000/bundle, 1 mm, float32                157 MB
    cap 2000/bundle, 1 mm, float16                 79 MB   <- the default here
    cap 2000/bundle, 2 mm, float16                 40 MB
    cap 1000/bundle, 1 mm, float16                 44 MB

Usage:
    make_tract_atlas.py <source-directory> <output.trx>
        [--max-streamlines 2000] [--step 1.0] [--dtype float16|float32]
"""
import argparse
import os
import struct
import sys
import zipfile

import numpy as np


def read_tck(path, keep, header_only=False):
    """Read a .tck, keeping at most `keep` streamlines at an even stride.

    Strided rather than truncated: a track file is often written grouped by seed
    region, so the first N would sample one end of the bundle only.
    """
    with open(path, "rb") as f:
        header = b""
        while not header.endswith(b"END\n"):
            chunk = f.read(1)
            if not chunk:
                raise RuntimeError("unterminated header in " + path)
            header += chunk
        fields = dict(
            line.split(": ", 1) for line in header.decode().splitlines() if ": " in line
        )
        dtype = {"Float32LE": "<f4", "Float32BE": ">f4"}[fields["datatype"]]
        count = int(fields.get("count", "0"))
        if header_only:
            return count, None
        f.seek(int(fields["file"].split(". ")[1]))
        data = np.fromfile(f, dtype=dtype).reshape(-1, 3)

    stride = max(1, -(-count // keep)) if keep and count > keep else 1
    finite = np.isfinite(data[:, 0])
    breaks = np.flatnonzero(~finite)
    tracks, start, index = [], 0, 0
    for b in breaks:
        if np.isinf(data[b, 0]):
            break
        if index % stride == 0:
            tracks.append(np.asarray(data[start:b], dtype=np.float64))
        start = b + 1
        index += 1
    return count, tracks


def resample(track, step):
    """Resample to a fixed arc-length step, keeping both endpoints."""
    if len(track) < 2 or step <= 0:
        return track
    seg = np.linalg.norm(np.diff(track, axis=0), axis=1)
    length = seg.sum()
    if length <= step:
        return track[[0, -1]]
    n = max(2, int(round(length / step)) + 1)
    cumulative = np.concatenate([[0.0], np.cumsum(seg)])
    targets = np.linspace(0.0, length, n)
    return np.stack([np.interp(targets, cumulative, track[:, d]) for d in range(3)], axis=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source")
    parser.add_argument("output")
    parser.add_argument("--max-streamlines", type=int, default=2000,
                        help="per bundle; 0 keeps all (default: 2000)")
    parser.add_argument("--step", type=float, default=1.0,
                        help="vertex spacing in mm; 0 keeps the original (default: 1.0)")
    parser.add_argument("--dtype", choices=["float16", "float32"], default="float16")
    args = parser.parse_args()

    categories = sorted(
        d for d in os.listdir(args.source)
        if not d.startswith(".") and os.path.isdir(os.path.join(args.source, d))
    )
    if not categories:
        sys.exit("no category subdirectories in " + args.source)

    positions = []
    offsets = []
    groups = {}
    vertex_total = 0
    source_streamlines = 0

    for category in categories:
        directory = os.path.join(args.source, category)
        for entry in sorted(os.listdir(directory)):
            if entry.startswith(".") or not entry.endswith(".tck"):
                continue
            path = os.path.join(directory, entry)
            count, tracks = read_tck(path, args.max_streamlines)
            source_streamlines += count
            if not tracks:
                print("  %-30s empty, skipped" % (category + "/" + entry))
                continue
            indices = []
            for track in tracks:
                track = resample(track, args.step)
                indices.append(len(offsets))
                offsets.append(vertex_total)
                positions.append(track)
                vertex_total += len(track)
            name = category + "/" + entry[:-4]
            groups[name] = np.asarray(indices, dtype="<u4")
            print("  %-34s %6d of %6d streamlines, %8d vertices"
                  % (name, len(tracks), count, sum(len(t) for t in tracks)))

    all_positions = np.concatenate(positions).astype(
        "<f2" if args.dtype == "float16" else "<f4")
    all_offsets = np.asarray(offsets, dtype="<i4")

    # Stored, not deflated: float coordinates barely compress (about 5%), and a
    # stored archive is memory-mappable and quicker to open.
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_STORED) as z:
        z.writestr("positions.3." + args.dtype, all_positions.tobytes())
        z.writestr("offsets.int32", all_offsets.tobytes())
        for name, indices in groups.items():
            z.writestr("groups/%s.uint32" % name, indices.tobytes())

    size = os.path.getsize(args.output)
    print("\n%s: %d bundles, %d of %d streamlines, %.1fM vertices, %.1f MB"
          % (args.output, len(groups), len(all_offsets), source_streamlines,
             vertex_total / 1e6, size / 1e6))


if __name__ == "__main__":
    main()
