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

#ifndef __dwi_tractography_file_dicom_h__
#define __dwi_tractography_file_dicom_h__

#include <cstring>
#include <fstream>
#include <set>

#include "types.h"
#include "exception.h"
#include "dwi/tractography/file.h"
#include "dwi/tractography/properties.h"
#include "dwi/tractography/streamline.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {

      //! Reader for streamlines stored in a DICOM Tractography Results object.
      /*! Parses the DICOM Tractography Results IOD (SOP class
       * 1.2.840.10008.5.1.4.1.1.66.6): the Track Set Sequence (0066,0101) ->
       * Track Sequence (0066,0102) -> per-track Point Coordinates Data
       * (0066,0016), an OF array of x,y,z triplets in the patient (LPS mm)
       * coordinate system. Points are converted to MRtrix's scanner (RAS mm)
       * convention (negate x and y), matching the .tck convention.
       *
       * Little-endian Explicit- and Implicit-VR datasets are supported. */
      template <class ValueType = float>
      class DICOMTrackReader : public ReaderInterface<ValueType>
      { NOMEMALIGN
        public:
          DICOMTrackReader (const std::string& file, Properties& properties) :
              current_index (0)
          {
            std::ifstream in (file, std::ios_base::in | std::ios_base::binary);
            if (!in)
              throw Exception ("error opening DICOM file \"" + file + "\"");
            buf.assign (std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>());
            in.close();

            if (buf.size() < 132 || std::memcmp (&buf[128], "DICM", 4) != 0)
              throw Exception ("file \"" + file + "\" is not a DICOM file");

            // File-meta group (0002) is always Explicit-VR little-endian; use it
            // to locate the dataset and read the dataset's transfer syntax.
            size_t p = 132;
            bool explicit_vr = true;
            size_t dataset_start = p;
            {
              // (0002,0000) UL gives the meta group length (bytes after it).
              if (p + 12 <= buf.size() && u16(p) == 0x0002 && u16(p+2) == 0x0000) {
                const uint32_t meta_len = u32 (p + 8);
                dataset_start = p + 12 + meta_len;
              }
              // find (0002,0010) transfer syntax UID within the meta group
              size_t q = p;
              while (q + 8 < dataset_start && q + 8 < buf.size()) {
                const uint16_t g = u16(q), e = u16(q+2);
                const std::string vr (reinterpret_cast<const char*> (&buf[q+4]), 2);
                size_t len, val;
                if (is_long_vr (vr)) { len = u32(q+8); val = q+12; }
                else                 { len = u16(q+6); val = q+8; }
                if (g == 0x0002 && e == 0x0010) {
                  std::string ts (reinterpret_cast<const char*> (&buf[val]), len);
                  while (ts.size() && (ts.back() == '\0' || ts.back() == ' ')) ts.pop_back();
                  if (ts == "1.2.840.10008.1.2")        explicit_vr = false; // Implicit VR LE
                  else if (ts == "1.2.840.10008.1.2.2")
                    throw Exception ("big-endian DICOM tractography is not supported (\"" + file + "\")");
                  break;
                }
                q = val + len;
              }
            }

            if (dataset_start > buf.size())
              dataset_start = buf.size();
            walk (dataset_start, buf.size(), explicit_vr);

            properties["count"] = str (tracks.size());
          }


          bool operator() (Streamline<ValueType>& tck)
          {
            tck.clear();
            if (current_index >= tracks.size())
              return false;
            const vector<float>& pts = tracks[current_index];
            tck.set_index (current_index);
            tck.weight = 1.0;
            for (size_t i = 0; i + 2 < pts.size(); i += 3)
              tck.push_back ({ ValueType (-pts[i]), ValueType (-pts[i+1]), ValueType (pts[i+2]) });
            ++current_index;
            return true;
          }


        protected:
          vector<uint8_t> buf;
          vector<vector<float>> tracks;
          size_t current_index;

          uint16_t u16 (size_t o) const { return uint16_t(buf[o]) | (uint16_t(buf[o+1]) << 8); }
          uint32_t u32 (size_t o) const {
            return uint32_t(buf[o]) | (uint32_t(buf[o+1])<<8) | (uint32_t(buf[o+2])<<16) | (uint32_t(buf[o+3])<<24);
          }
          static bool is_long_vr (const std::string& vr) {
            static const std::set<std::string> longs { "OB","OW","OF","SQ","UT","UN","OD","OL","UC","UR" };
            return longs.count (vr) > 0;
          }
          static bool is_sequence_tag (uint16_t g, uint16_t e) {
            // Known SQ tags in the Tractography Results IOD, for implicit-VR files
            // where the VR is not stored inline.
            if (g != 0x0066) return false;
            switch (e) {
              case 0x0101: case 0x0102: case 0x0104: case 0x0108:
              case 0x0112: case 0x0114: case 0x002F: case 0x0134:
                return true;
              default: return false;
            }
          }

          //! Recursively walk a (sub-)dataset, collecting Point Coordinates Data.
          void walk (size_t p, size_t end, bool explicit_vr)
          {
            while (p + 8 <= end) {
              const uint16_t g = u16(p), e = u16(p+2);
              p += 4;

              // Item / delimitation items carry no VR.
              if (g == 0xFFFE) {
                const uint32_t len = u32(p); p += 4;
                if (e == 0xE000) {                 // item
                  if (len == 0xFFFFFFFF) {         // undefined length -> until item delim
                    p = walk_until (p, end, explicit_vr, 0xE00D);
                  } else {
                    walk (p, p + len, explicit_vr);
                    p += len;
                  }
                }
                // E00D / E0DD delimiters: nothing to skip.
                continue;
              }

              std::string vr;
              uint32_t len;
              bool is_sq = false;
              if (explicit_vr) {
                vr.assign (reinterpret_cast<const char*> (&buf[p]), 2);
                if (is_long_vr (vr)) { len = u32(p+4); p += 8; }
                else                 { len = u16(p+2); p += 4; }
                is_sq = (vr == "SQ");
              } else {
                len = u32(p); p += 4;
                is_sq = is_sequence_tag (g, e) || len == 0xFFFFFFFF;
              }

              if (g == 0x0066 && e == 0x0016 && !is_sq && len != 0xFFFFFFFF) {
                const size_t n = len / 4;
                vector<float> pts (n);
                for (size_t i = 0; i != n; ++i) {
                  uint32_t bits = u32 (p + 4*i);
                  float f; std::memcpy (&f, &bits, 4);
                  pts[i] = f;
                }
                tracks.push_back (std::move (pts));
                p += len;
                continue;
              }

              if (is_sq) {
                if (len == 0xFFFFFFFF)
                  p = walk_until (p, end, explicit_vr, 0xE0DD);
                else {
                  walk (p, p + len, explicit_vr);
                  p += len;
                }
              } else {
                if (len == 0xFFFFFFFF) len = 0;
                p += len;
              }
            }
          }

          //! Walk items of an undefined-length sequence/item until its delimiter
          //! (FFFE, \a delim_elem); returns the position just past the delimiter.
          size_t walk_until (size_t p, size_t end, bool explicit_vr, uint16_t delim_elem)
          {
            while (p + 8 <= end) {
              const uint16_t g = u16(p), e = u16(p+2);
              if (g == 0xFFFE) {
                const uint32_t len = u32(p+4);
                p += 8;
                if (e == delim_elem)
                  return p;
                if (e == 0xE000) {                 // item
                  if (len == 0xFFFFFFFF)
                    p = walk_until (p, end, explicit_vr, 0xE00D);
                  else {
                    walk (p, p + len, explicit_vr);
                    p += len;
                  }
                }
                continue;
              }
              // A bare element inside an undefined-length item: walk one element
              // by re-entering the general walker for a minimal span is awkward;
              // instead handle it inline via a single-element step.
              p = step_element (p, end, explicit_vr);
            }
            return p;
          }

          //! Advance past a single element (collecting point data / recursing into
          //! sequences), returning the new position.
          size_t step_element (size_t p, size_t end, bool explicit_vr)
          {
            const uint16_t g = u16(p), e = u16(p+2);
            p += 4;
            std::string vr;
            uint32_t len;
            bool is_sq = false;
            if (explicit_vr) {
              vr.assign (reinterpret_cast<const char*> (&buf[p]), 2);
              if (is_long_vr (vr)) { len = u32(p+4); p += 8; }
              else                 { len = u16(p+2); p += 4; }
              is_sq = (vr == "SQ");
            } else {
              len = u32(p); p += 4;
              is_sq = is_sequence_tag (g, e) || len == 0xFFFFFFFF;
            }
            if (g == 0x0066 && e == 0x0016 && !is_sq && len != 0xFFFFFFFF) {
              const size_t n = len / 4;
              vector<float> pts (n);
              for (size_t i = 0; i != n; ++i) {
                uint32_t bits = u32 (p + 4*i);
                float f; std::memcpy (&f, &bits, 4);
                pts[i] = f;
              }
              tracks.push_back (std::move (pts));
              return p + len;
            }
            if (is_sq) {
              if (len == 0xFFFFFFFF)
                return walk_until (p, end, explicit_vr, 0xE0DD);
              walk (p, p + len, explicit_vr);
              return p + len;
            }
            if (len == 0xFFFFFFFF) len = 0;
            return p + len;
          }
      };


    }
  }
}

#endif
