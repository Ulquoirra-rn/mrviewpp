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

            // header.json is optional: when absent (or missing fields), the
            // stream/vertex counts are derived from the array sizes below.
            nb_streamlines = 0;
            nb_vertices = 0;
            auto hdr_it = entries.find ("header.json");
            if (hdr_it != entries.end()) {
              const vector<uint8_t> hdr_bytes = extract (zip, hdr_it->second, file);
              try {
                auto header = nlohmann::json::parse (std::string (hdr_bytes.begin(), hdr_bytes.end()));
                if (header.count ("NB_STREAMLINES")) nb_streamlines = header["NB_STREAMLINES"].get<uint64_t>();
                if (header.count ("NB_VERTICES"))    nb_vertices    = header["NB_VERTICES"].get<uint64_t>();
              } catch (...) { /* tolerate a malformed header and derive counts */ }
            }

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
            // Derive the vertex count from the array size when no header gave it.
            if (!nb_vertices) {
              const size_t bytes = (dtype == "float64") ? 8 : (dtype == "float16") ? 2 : 4;
              nb_vertices = raw.size() / (3 * bytes);
            }
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
            // Number of entries actually stored (may be NB_STREAMLINES or +1).
            const size_t bytes = (dtype == "uint64" || dtype == "int64") ? 8 : 4;
            const size_t count = raw.size() / bytes;
            auto get = [&] (size_t i) -> uint64_t {
              if (bytes == 8) { uint64_t v; std::memcpy (&v, &raw[8*i], 8); return v; }
              uint32_t v; std::memcpy (&v, &raw[4*i], 4); return v;   // uint32/int32: indices are non-negative
            };
            if (dtype != "uint64" && dtype != "int64" && dtype != "uint32" && dtype != "int32")
              throw Exception ("unsupported TRX offsets dtype \"" + dtype + "\" in \"" + file + "\"");

            // Derive the streamline count if no header gave it: a trailing entry
            // equal to NB_VERTICES indicates the N+1 convention.
            if (!nb_streamlines) {
              if (count && get (count - 1) == nb_vertices)
                nb_streamlines = count - 1;
              else
                nb_streamlines = count;
            }
            if (count < nb_streamlines)
              throw Exception ("TRX offsets array too small (\"" + file + "\")");

            offsets.resize (nb_streamlines);
            for (uint64_t i = 0; i != nb_streamlines; ++i)
              offsets[i] = get (i);
          }
      };


      //! Helpers to inspect the optional data arrays embedded in a TRX file.
      /*! TRX may carry per-vertex (dpv/) and per-streamline (dps/) data arrays
       * in addition to the geometry. These let mrview offer them as threshold
       * sources. */
      namespace TRX_Data
      {
        struct Array { NOMEMALIGN
          std::string entry;   // full ZIP entry name, e.g. "dpv/fa.float32"
          std::string name;    // display name, e.g. "fa"
          bool per_vertex;     // true for dpv/, false for dps/
        };

        // little-endian helpers over an in-memory buffer
        inline uint16_t _u16 (const vector<uint8_t>& b, size_t o) { return uint16_t(b[o]) | (uint16_t(b[o+1])<<8); }
        inline uint32_t _u32 (const vector<uint8_t>& b, size_t o) {
          return uint32_t(b[o]) | (uint32_t(b[o+1])<<8) | (uint32_t(b[o+2])<<16) | (uint32_t(b[o+3])<<24);
        }

        inline vector<uint8_t> _slurp (const std::string& file) {
          std::ifstream in (file, std::ios_base::in | std::ios_base::binary);
          if (!in)
            throw Exception ("error opening TRX file \"" + file + "\"");
          return vector<uint8_t> ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
        }

        // Walk the central directory, returning name -> (method, comp_size, uncomp_size, local_offset).
        struct _Entry { NOMEMALIGN uint16_t method; uint32_t csize, usize, loff; };
        inline std::map<std::string, _Entry> _entries (const vector<uint8_t>& b, const std::string& file) {
          std::map<std::string, _Entry> out;
          if (b.size() < 22)
            throw Exception ("file \"" + file + "\" is too small to be a TRX (ZIP) file");
          size_t eocd = b.size() - 22;
          const size_t min_pos = b.size() > 22 + 65535 ? b.size() - 22 - 65535 : 0;
          bool found = false;
          for (size_t i = eocd + 1; i-- > min_pos; ) {
            if (_u32 (b, i) == 0x06054b50) { eocd = i; found = true; break; }
            if (i == 0) break;
          }
          if (!found)
            throw Exception ("file \"" + file + "\" is not a valid TRX (ZIP) file");
          const uint16_t n = _u16 (b, eocd + 10);
          size_t p = _u32 (b, eocd + 16);
          for (uint16_t e = 0; e != n; ++e) {
            if (_u32 (b, p) != 0x02014b50)
              throw Exception ("corrupt central directory in TRX file \"" + file + "\"");
            const uint16_t method = _u16 (b, p + 10);
            const uint32_t csize  = _u32 (b, p + 20);
            const uint32_t usize  = _u32 (b, p + 24);
            const uint16_t fnlen  = _u16 (b, p + 28);
            const uint16_t exlen  = _u16 (b, p + 30);
            const uint16_t cmlen  = _u16 (b, p + 32);
            const uint32_t loff   = _u32 (b, p + 42);
            out[std::string (reinterpret_cast<const char*> (&b[p + 46]), fnlen)] = { method, csize, usize, loff };
            p += 46 + fnlen + exlen + cmlen;
          }
          return out;
        }

        inline vector<uint8_t> _extract (const vector<uint8_t>& b, const _Entry& e, const std::string& file) {
          if (_u32 (b, e.loff) != 0x04034b50)
            throw Exception ("corrupt local header in TRX file \"" + file + "\"");
          const size_t data = e.loff + 30 + _u16 (b, e.loff + 26) + _u16 (b, e.loff + 28);
          if (e.method == 0)
            return vector<uint8_t> (b.begin() + data, b.begin() + data + e.usize);
          if (e.method == 8) {
            vector<uint8_t> out (e.usize);
            z_stream s; memset (&s, 0, sizeof (s));
            if (inflateInit2 (&s, -15) != Z_OK) throw Exception ("zlib init failed for \"" + file + "\"");
            s.next_in = const_cast<Bytef*> (&b[data]); s.avail_in = uInt (e.csize);
            s.next_out = out.data(); s.avail_out = uInt (e.usize);
            const int ret = inflate (&s, Z_FINISH); inflateEnd (&s);
            if (ret != Z_STREAM_END) throw Exception ("failed to inflate entry in \"" + file + "\"");
            return out;
          }
          throw Exception ("unsupported ZIP compression method in TRX file \"" + file + "\"");
        }

        //! List the dpv/ and dps/ data arrays available in a TRX file.
        inline vector<Array> list (const std::string& file) {
          const vector<uint8_t> b = _slurp (file);
          const auto entries = _entries (b, file);
          vector<Array> out;
          for (const auto& kv : entries) {
            const std::string& n = kv.first;
            bool dpv = n.compare (0, 4, "dpv/") == 0;
            bool dps = n.compare (0, 4, "dps/") == 0;
            if (!dpv && !dps) continue;
            std::string base = n.substr (4);
            const size_t dot = base.find_last_of ('.');   // strip dtype suffix
            if (dot != std::string::npos) base = base.substr (0, dot);
            if (base.empty()) continue;
            out.push_back ({ n, base, dpv });
          }
          return out;
        }

        //! Read a named float data array (any float dtype) from a TRX file.
        inline vector<float> read (const std::string& file, const std::string& entry) {
          const vector<uint8_t> b = _slurp (file);
          const auto entries = _entries (b, file);
          auto it = entries.find (entry);
          if (it == entries.end())
            throw Exception ("TRX file \"" + file + "\" has no data array \"" + entry + "\"");
          const vector<uint8_t> raw = _extract (b, it->second, file);
          const size_t dot = entry.find_last_of ('.');
          const std::string dtype = dot == std::string::npos ? "" : entry.substr (dot + 1);
          vector<float> out;
          if (dtype == "float32") {
            out.resize (raw.size() / 4);
            for (size_t i = 0; i != out.size(); ++i) { float v; std::memcpy (&v, &raw[4*i], 4); out[i] = v; }
          } else if (dtype == "float64") {
            out.resize (raw.size() / 8);
            for (size_t i = 0; i != out.size(); ++i) { double v; std::memcpy (&v, &raw[8*i], 8); out[i] = float (v); }
          } else if (dtype == "float16") {
            out.resize (raw.size() / 2);
            for (size_t i = 0; i != out.size(); ++i) {
              uint16_t h; std::memcpy (&h, &raw[2*i], 2);
              const uint32_t sign = (uint32_t(h) & 0x8000u) << 16; uint32_t exp = (h>>10)&0x1Fu, mant = h&0x3FFu, f;
              if (exp == 0) { if (!mant) f = sign; else { exp = 1; while (!(mant & 0x400u)) { mant <<= 1; --exp; } mant &= 0x3FFu; f = sign | ((exp+(127-15))<<23) | (mant<<13); } }
              else if (exp == 0x1Fu) f = sign | 0x7F800000u | (mant<<13);
              else f = sign | ((exp+(127-15))<<23) | (mant<<13);
              std::memcpy (&out[i], &f, 4);
            }
          } else {
            // integer dtypes: interpret as scalar values
            if (dtype == "uint32" || dtype == "int32") {
              out.resize (raw.size() / 4);
              for (size_t i = 0; i != out.size(); ++i) { int32_t v; std::memcpy (&v, &raw[4*i], 4); out[i] = float (v); }
            } else if (dtype == "uint16" || dtype == "int16") {
              out.resize (raw.size() / 2);
              for (size_t i = 0; i != out.size(); ++i) { int16_t v; std::memcpy (&v, &raw[2*i], 2); out[i] = float (v); }
            } else if (dtype == "uint8" || dtype == "int8" || dtype == "bool") {
              out.resize (raw.size());
              for (size_t i = 0; i != out.size(); ++i) out[i] = float (raw[i]);
            } else {
              throw Exception ("unsupported TRX data-array dtype \"" + dtype + "\" in \"" + file + "\"");
            }
          }
          return out;
        }

        //! Read an integer data array (any int dtype) as uint64 values.
        inline vector<uint64_t> read_uint (const std::string& file, const std::string& entry) {
          const vector<uint8_t> b = _slurp (file);
          const auto entries = _entries (b, file);
          auto it = entries.find (entry);
          if (it == entries.end())
            throw Exception ("TRX file \"" + file + "\" has no array \"" + entry + "\"");
          const vector<uint8_t> raw = _extract (b, it->second, file);
          const size_t dot = entry.find_last_of ('.');
          const std::string dtype = dot == std::string::npos ? "" : entry.substr (dot + 1);
          const size_t bytes = (dtype == "uint64" || dtype == "int64") ? 8
                             : (dtype == "uint32" || dtype == "int32") ? 4
                             : (dtype == "uint16" || dtype == "int16") ? 2
                             : (dtype == "uint8"  || dtype == "int8" || dtype == "bool") ? 1 : 0;
          if (!bytes)
            throw Exception ("unsupported TRX integer dtype \"" + dtype + "\" in \"" + file + "\"");
          vector<uint64_t> out (raw.size() / bytes);
          for (size_t i = 0; i != out.size(); ++i) {
            uint64_t v = 0;
            std::memcpy (&v, &raw[bytes * i], bytes);
            out[i] = v;
          }
          return out;
        }

        //! Read the root-level offsets array (start vertex index per streamline).
        inline vector<uint64_t> read_offsets (const std::string& file) {
          const vector<uint8_t> b = _slurp (file);
          const auto entries = _entries (b, file);
          for (const auto& kv : entries) {
            if (kv.first.find ('/') != std::string::npos) continue;
            if (kv.first.compare (0, 8, "offsets.") == 0)
              return read_uint (file, kv.first);
          }
          throw Exception ("TRX file \"" + file + "\" has no offsets array");
        }

        //! Read every groups/<name> array in one pass over the archive.
        /*! read_uint() re-reads the whole file per array, which is fine for one
         *  lookup and quadratic for a whole atlas: 102 groups of an 82 MB archive
         *  cost 8 GB of reading and about sixteen seconds. */
        inline std::map<std::string, vector<uint64_t>> read_groups (const std::string& file) {
          const vector<uint8_t> b = _slurp (file);
          const auto entries = _entries (b, file);
          std::map<std::string, vector<uint64_t>> out;
          for (const auto& kv : entries) {
            const std::string& n = kv.first;
            if (n.compare (0, 7, "groups/") != 0) continue;
            std::string base = n.substr (7);
            const size_t dot = base.find_last_of ('.');
            if (dot == std::string::npos) continue;
            const std::string dtype = base.substr (dot + 1);
            base = base.substr (0, dot);
            if (base.empty()) continue;
            const size_t bytes = (dtype == "uint64" || dtype == "int64") ? 8
                               : (dtype == "uint32" || dtype == "int32") ? 4
                               : (dtype == "uint16" || dtype == "int16") ? 2
                               : (dtype == "uint8"  || dtype == "int8" || dtype == "bool") ? 1 : 0;
            if (!bytes) continue;
            const vector<uint8_t> raw = _extract (b, kv.second, file);
            vector<uint64_t>& indices = out[base];
            indices.resize (raw.size() / bytes);
            for (size_t i = 0; i != indices.size(); ++i) {
              uint64_t v = 0;
              std::memcpy (&v, &raw[bytes * i], bytes);
              indices[i] = v;
            }
          }
          return out;
        }

        //! List the named groups (groups/<name>.<dtype>) in a TRX file.
        inline vector<std::pair<std::string,std::string>> groups (const std::string& file) {
          const vector<uint8_t> b = _slurp (file);
          const auto entries = _entries (b, file);
          vector<std::pair<std::string,std::string>> out;   // (name, entry)
          for (const auto& kv : entries) {
            const std::string& n = kv.first;
            if (n.compare (0, 7, "groups/") != 0) continue;
            std::string base = n.substr (7);
            const size_t dot = base.find_last_of ('.');
            if (dot != std::string::npos) base = base.substr (0, dot);
            if (base.empty()) continue;   // skip the "groups/" directory entry
            out.push_back ({ base, n });
          }
          return out;
        }
      }


    }
  }
}

#endif
