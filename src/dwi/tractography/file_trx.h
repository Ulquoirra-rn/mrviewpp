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

#ifndef __dwi_tractography_file_trx_h__
#define __dwi_tractography_file_trx_h__

#include <cstring>
#include <fstream>
#include <map>
#include <zlib.h>

#include "types.h"
#include "exception.h"
#include "file/json.h"
#include "dwi/tractography/file.h"
#include "dwi/tractography/properties.h"
#include "dwi/tractography/streamline.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {

      //! Reader for TRX (.trx) streamline files.
      /*! TRX is a ZIP container (entries stored uncompressed by default, or
       * DEFLATE-compressed) holding header.json plus flat binary arrays:
       * positions.3.float{16,32,64} (NB_VERTICES x 3, already in RASMM world
       * space) and offsets.uint{32,64} (start vertex index of each streamline).
       * Positions need no spatial transform, matching the .tck convention. */
      template <class ValueType = float>
      class TRXReader : public ReaderInterface<ValueType>
      { NOMEMALIGN
        public:
          TRXReader (const std::string& file, Properties& properties)
          {
            std::ifstream in (file, std::ios_base::in | std::ios_base::binary);
            if (!in)
              throw Exception ("error opening TRX file \"" + file + "\"");
            const vector<uint8_t> zip ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
            in.close();

            std::map<std::string, ZipEntry> entries;
            parse_zip (zip, entries, file);

            // header.json
            const vector<uint8_t> hdr_bytes = extract (zip, find_entry (entries, "header.json", file), file);
            auto header = nlohmann::json::parse (std::string (hdr_bytes.begin(), hdr_bytes.end()));
            nb_streamlines = header.at ("NB_STREAMLINES").get<uint64_t>();
            nb_vertices    = header.at ("NB_VERTICES").get<uint64_t>();

            // positions (RASMM) -> flat float array
            const std::string pos_name = find_prefixed (entries, "positions", file);
            const std::string off_name = find_prefixed (entries, "offsets", file);
            const vector<uint8_t> pos_raw = extract (zip, entries.at (pos_name), file);
            const vector<uint8_t> off_raw = extract (zip, entries.at (off_name), file);

            load_positions (pos_raw, dtype_of (pos_name), file);
            load_offsets (off_raw, dtype_of (off_name), file);

            properties["count"] = str (nb_streamlines);
            current_index = 0;
          }


          bool operator() (Streamline<ValueType>& tck)
          {
            tck.clear();
            if (current_index >= nb_streamlines)
              return false;
            const uint64_t start = offsets[current_index];
            const uint64_t end   = (current_index + 1 < nb_streamlines) ? offsets[current_index + 1] : nb_vertices;
            tck.set_index (current_index);
            tck.weight = 1.0;
            for (uint64_t v = start; v < end; ++v)
              tck.push_back ({ ValueType (positions[3*v]), ValueType (positions[3*v+1]), ValueType (positions[3*v+2]) });
            ++current_index;
            return true;
          }


        protected:
          struct ZipEntry { NOMEMALIGN
            uint16_t method;
            uint64_t comp_size, uncomp_size, local_offset;
          };

          uint64_t nb_streamlines, nb_vertices, current_index;
          vector<float> positions;       // 3 * nb_vertices, RASMM
          vector<uint64_t> offsets;      // nb_streamlines

          // --- little-endian readers over an in-memory buffer ---
          static uint16_t u16 (const vector<uint8_t>& b, size_t o) { return uint16_t(b[o]) | (uint16_t(b[o+1])<<8); }
          static uint32_t u32 (const vector<uint8_t>& b, size_t o) {
            return uint32_t(b[o]) | (uint32_t(b[o+1])<<8) | (uint32_t(b[o+2])<<16) | (uint32_t(b[o+3])<<24);
          }

          static void parse_zip (const vector<uint8_t>& b, std::map<std::string, ZipEntry>& out, const std::string& file)
          {
            // locate End Of Central Directory (signature 0x06054b50)
            if (b.size() < 22)
              throw Exception ("file \"" + file + "\" is too small to be a TRX (ZIP) file");
            size_t eocd = b.size() - 22;
            const size_t min_pos = b.size() > 22 + 65535 ? b.size() - 22 - 65535 : 0;
            bool found = false;
            for (size_t i = eocd + 1; i-- > min_pos; ) {
              if (u32 (b, i) == 0x06054b50) { eocd = i; found = true; break; }
              if (i == 0) break;
            }
            if (!found)
              throw Exception ("file \"" + file + "\" is not a valid TRX (ZIP) file (no central directory)");
            const uint16_t n_entries = u16 (b, eocd + 10);
            uint32_t cd_offset = u32 (b, eocd + 16);
            if (cd_offset == 0xFFFFFFFF)
              throw Exception ("ZIP64 TRX files are not yet supported (\"" + file + "\")");

            size_t p = cd_offset;
            for (uint16_t e = 0; e != n_entries; ++e) {
              if (u32 (b, p) != 0x02014b50)
                throw Exception ("corrupt central directory in TRX file \"" + file + "\"");
              const uint16_t method   = u16 (b, p + 10);
              const uint32_t csize    = u32 (b, p + 20);
              const uint32_t usize    = u32 (b, p + 24);
              const uint16_t fnlen    = u16 (b, p + 28);
              const uint16_t extralen = u16 (b, p + 30);
              const uint16_t commlen  = u16 (b, p + 32);
              const uint32_t loff     = u32 (b, p + 42);
              if (csize == 0xFFFFFFFF || usize == 0xFFFFFFFF || loff == 0xFFFFFFFF)
                throw Exception ("ZIP64 TRX files are not yet supported (\"" + file + "\")");
              const std::string name (reinterpret_cast<const char*> (&b[p + 46]), fnlen);
              ZipEntry entry; entry.method = method; entry.comp_size = csize;
              entry.uncomp_size = usize; entry.local_offset = loff;
              out[name] = entry;
              p += 46 + fnlen + extralen + commlen;
            }
          }

          static vector<uint8_t> extract (const vector<uint8_t>& b, const ZipEntry& e, const std::string& file)
          {
            // local header: skip its own (possibly different) name/extra lengths
            if (u32 (b, e.local_offset) != 0x04034b50)
              throw Exception ("corrupt local header in TRX file \"" + file + "\"");
            const uint16_t fnlen    = u16 (b, e.local_offset + 26);
            const uint16_t extralen = u16 (b, e.local_offset + 28);
            const size_t data = e.local_offset + 30 + fnlen + extralen;

            if (e.method == 0) {       // stored
              return vector<uint8_t> (b.begin() + data, b.begin() + data + e.uncomp_size);
            }
            if (e.method == 8) {       // deflate
              vector<uint8_t> out (e.uncomp_size);
              z_stream strm; memset (&strm, 0, sizeof (strm));
              if (inflateInit2 (&strm, -15) != Z_OK)   // raw deflate
                throw Exception ("zlib init failed for TRX file \"" + file + "\"");
              strm.next_in = const_cast<Bytef*> (&b[data]);
              strm.avail_in = static_cast<uInt> (e.comp_size);
              strm.next_out = out.data();
              strm.avail_out = static_cast<uInt> (e.uncomp_size);
              const int ret = inflate (&strm, Z_FINISH);
              inflateEnd (&strm);
              if (ret != Z_STREAM_END)
                throw Exception ("failed to inflate entry in TRX file \"" + file + "\"");
              return out;
            }
            throw Exception ("unsupported ZIP compression method in TRX file \"" + file + "\"");
          }

          static const ZipEntry& find_entry (const std::map<std::string, ZipEntry>& m, const std::string& name, const std::string& file)
          {
            auto it = m.find (name);
            if (it == m.end())
              throw Exception ("TRX file \"" + file + "\" is missing mandatory entry \"" + name + "\"");
            return it->second;
          }

          // match a mandatory root-level array by basename prefix (e.g. "positions")
          static std::string find_prefixed (const std::map<std::string, ZipEntry>& m, const std::string& prefix, const std::string& file)
          {
            for (const auto& kv : m) {
              if (kv.first.find ('/') != std::string::npos) continue;   // skip dpv/, dps/ etc.
              if (kv.first.compare (0, prefix.size(), prefix) == 0 &&
                  kv.first.size() > prefix.size() && kv.first[prefix.size()] == '.')
                return kv.first;
            }
            throw Exception ("TRX file \"" + file + "\" is missing mandatory \"" + prefix + "\" array");
          }

          static std::string dtype_of (const std::string& name) {
            const size_t dot = name.find_last_of ('.');
            return dot == std::string::npos ? "" : name.substr (dot + 1);
          }

          static float half_to_float (const uint16_t h) {
            const uint32_t sign = (uint32_t (h) & 0x8000u) << 16;
            uint32_t exp = (h >> 10) & 0x1Fu;
            uint32_t mant = h & 0x3FFu;
            uint32_t f;
            if (exp == 0) {
              if (mant == 0) { f = sign; }
              else {
                exp = 1;
                while ((mant & 0x400u) == 0) { mant <<= 1; --exp; }
                mant &= 0x3FFu;
                f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
              }
            } else if (exp == 0x1Fu) {
              f = sign | 0x7F800000u | (mant << 13);
            } else {
              f = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
            }
            float out; std::memcpy (&out, &f, 4); return out;
          }

          void load_positions (const vector<uint8_t>& raw, const std::string& dtype, const std::string& file)
          {
            const size_t n = 3 * nb_vertices;
            positions.resize (n);
            if (dtype == "float32") {
              if (raw.size() < n * 4) throw Exception ("TRX positions array too small (\"" + file + "\")");
              for (size_t i = 0; i != n; ++i) { float v; std::memcpy (&v, &raw[4*i], 4); positions[i] = v; }
            } else if (dtype == "float64") {
              if (raw.size() < n * 8) throw Exception ("TRX positions array too small (\"" + file + "\")");
              for (size_t i = 0; i != n; ++i) { double v; std::memcpy (&v, &raw[8*i], 8); positions[i] = float (v); }
            } else if (dtype == "float16") {
              if (raw.size() < n * 2) throw Exception ("TRX positions array too small (\"" + file + "\")");
              for (size_t i = 0; i != n; ++i) { uint16_t v; std::memcpy (&v, &raw[2*i], 2); positions[i] = half_to_float (v); }
            } else {
              throw Exception ("unsupported TRX positions dtype \"" + dtype + "\" in \"" + file + "\"");
            }
          }

          void load_offsets (const vector<uint8_t>& raw, const std::string& dtype, const std::string& file)
          {
            offsets.resize (nb_streamlines);
            if (dtype == "uint64") {
              if (raw.size() < nb_streamlines * 8) throw Exception ("TRX offsets array too small (\"" + file + "\")");
              for (uint64_t i = 0; i != nb_streamlines; ++i) { uint64_t v; std::memcpy (&v, &raw[8*i], 8); offsets[i] = v; }
            } else if (dtype == "uint32") {
              if (raw.size() < nb_streamlines * 4) throw Exception ("TRX offsets array too small (\"" + file + "\")");
              for (uint64_t i = 0; i != nb_streamlines; ++i) { uint32_t v; std::memcpy (&v, &raw[4*i], 4); offsets[i] = v; }
            } else {
              throw Exception ("unsupported TRX offsets dtype \"" + dtype + "\" in \"" + file + "\"");
            }
          }
      };


    }
  }
}

#endif
