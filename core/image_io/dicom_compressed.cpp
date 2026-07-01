/* Copyright (c) 2008-2026 the MRtrix3 contributors.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Covered Software is provided under this License on an "as is"
 * basis, without warranty of any kind, either expressed, implied, or
 * statutory, including, without limitation, warranties that the
 * Covered Software is free of defects, merchantable, fit for a
 * particular purpose or non-infringing.
 * See the Mozilla Public License v. 2.0 for more details.
 *
 * For more details, see http://www.mrtrix.org/.
 */

#include <cstring>
#include <fstream>

#include "app.h"
#include "progressbar.h"
#include "header.h"
#include "image_io/dicom_compressed.h"
#include "file/dicom/dicom_decode.h"

namespace MR
{
  namespace ImageIO
  {

    using File::Dicom::TransferSyntax;


    namespace {

      // Read from a file, starting at the given offset, to end-of-file.
      vector<uint8_t> read_from (const std::string& fname, int64_t offset)
      {
        std::ifstream in (fname, std::ios::in | std::ios::binary);
        if (!in)
          throw Exception ("error opening DICOM file \"" + fname + "\"");
        in.seekg (0, std::ios::end);
        const int64_t size = in.tellg();
        if (offset > size)
          throw Exception ("invalid pixel-data offset in DICOM file \"" + fname + "\"");
        vector<uint8_t> out (size - offset);
        in.seekg (offset);
        if (out.size())
          in.read (reinterpret_cast<char*> (out.data()), out.size());
        return out;
      }

      inline uint16_t u16 (const vector<uint8_t>& b, size_t o) { return uint16_t(b[o]) | (uint16_t(b[o+1])<<8); }
      inline uint32_t u32 (const vector<uint8_t>& b, size_t o) {
        return uint32_t(b[o]) | (uint32_t(b[o+1])<<8) | (uint32_t(b[o+2])<<16) | (uint32_t(b[o+3])<<24);
      }

      // Parse encapsulated pixel-data (Basic Offset Table + fragment items) that
      // begins at the start of \a enc, returning the concatenated fragment bytes
      // for the (single) frame. (Common single-frame-per-file case.)
      vector<uint8_t> concat_fragments (const vector<uint8_t>& enc)
      {
        vector<uint8_t> frame;
        size_t p = 0;
        bool skipped_bot = false;
        while (p + 8 <= enc.size()) {
          const uint16_t g = u16(enc,p), e = u16(enc,p+2);
          const uint32_t len = u32(enc,p+4);
          p += 8;
          if (g != 0xFFFE)
            break;
          if (e == 0xE0DD)               // sequence delimitation -> done
            break;
          if (e != 0xE000)               // not an item -> stop
            break;
          if (p + len > enc.size())
            break;
          if (!skipped_bot) {            // first item is the Basic Offset Table
            skipped_bot = true;
          } else {
            frame.insert (frame.end(), enc.begin() + p, enc.begin() + p + len);
          }
          p += len;
        }
        return frame;
      }

      // DICOM RLE (PS3.5 Annex G): a 64-byte header (number of segments + 32-bit
      // little-endian offsets) followed by PackBits-encoded segments.
      vector<uint8_t> decode_rle (const vector<uint8_t>& in, size_t rows, size_t cols, size_t bits, size_t samples)
      {
        if (in.size() < 64)
          throw Exception ("truncated RLE header in DICOM data");
        const uint32_t n_seg = u32 (in, 0);
        const size_t bytes_per_sample = bits / 8;
        const size_t npix = rows * cols;
        vector<uint8_t> out (npix * bytes_per_sample * samples, 0);

        auto unpack = [&] (size_t start, size_t stop, vector<uint8_t>& dst) {
          dst.clear(); dst.reserve (npix);
          size_t p = start;
          while (p < stop && dst.size() < npix) {
            const int8_t n = int8_t (in[p++]);
            if (n >= 0) {                       // literal run of n+1 bytes
              for (int i = 0; i <= n && p < stop; ++i)
                dst.push_back (in[p++]);
            } else if (n != -128) {             // replicate next byte (1-n) times
              if (p >= stop) break;
              const uint8_t v = in[p++];
              for (int i = 0; i < 1 - n; ++i)
                dst.push_back (v);
            }
          }
          dst.resize (npix, 0);
        };

        // Segments are ordered MSB-first for each sample.
        vector<uint8_t> plane;
        for (uint32_t s = 0; s < n_seg; ++s) {
          const uint32_t off = u32 (in, 4 + 4*s);
          const uint32_t next = (s + 1 < n_seg) ? u32 (in, 4 + 4*(s+1)) : uint32_t (in.size());
          if (off >= in.size())
            continue;
          unpack (off, std::min<size_t> (next, in.size()), plane);

          const size_t sample = s / bytes_per_sample;      // which channel
          const size_t byte   = bytes_per_sample - 1 - (s % bytes_per_sample); // MSB-first -> byte index
          for (size_t i = 0; i < npix; ++i)
            out[(i * samples + sample) * bytes_per_sample + byte] = plane[i];
        }
        return out;
      }

    }



    void DICOMCompressed::load (const Header& header, size_t)
    {
      if (files.empty())
        throw Exception ("no files specified in header for image \"" + header.name() + "\"");

      const size_t bytes = bits / 8;
      const size_t seg_bytes = rows * cols * samples * bytes;

      addresses.resize (1);
      addresses[0].reset (new uint8_t [files.size() * seg_bytes]);
      if (!addresses[0])
        throw Exception ("failed to allocate memory for image \"" + header.name() + "\"");
      memset (addresses[0].get(), 0, files.size() * seg_bytes);

      ProgressBar progress ("decompressing DICOM data", files.size());
      for (size_t n = 0; n < files.size(); ++n) {
        const vector<uint8_t> enc = read_from (files[n].name, files[n].start);
        const vector<uint8_t> frame = concat_fragments (enc);

        vector<uint8_t> raw;
        switch (codec) {
          case TransferSyntax::RLE:
            raw = decode_rle (frame, rows, cols, bits, samples);
            break;
          case TransferSyntax::JPEG:
            raw = File::Dicom::decode_jpeg (frame, rows, cols, bits, samples, is_signed);
            break;
          case TransferSyntax::JPEG2000:
            raw = File::Dicom::decode_jpeg2000 (frame, rows, cols, bits, samples, is_signed);
            break;
          default:
            throw Exception ("internal error: unhandled DICOM compression codec");
        }
        if (raw.size() < seg_bytes)
          raw.resize (seg_bytes, 0);
        memcpy (addresses[0].get() + n * seg_bytes, raw.data(), seg_bytes);
        ++progress;
      }

      segsize = std::numeric_limits<size_t>::max();
    }


  }
}
