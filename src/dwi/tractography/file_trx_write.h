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

#ifndef __dwi_tractography_file_trx_write_h__
#define __dwi_tractography_file_trx_write_h__

#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <zlib.h>

#include "types.h"
#include "exception.h"
#include "mrtrix.h"
#include "dwi/tractography/streamline.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {

      //! Writer for TRX (.trx) streamline files.
      /*! Produces a TRX container (a ZIP archive with entries stored
       * uncompressed) holding header.json, positions.3.float32 (RASMM, no
       * spatial transform — matching the .tck convention) and offsets.uint64.
       *
       * Multiple tractograms can be combined into a single file: each is added
       * as a named entry under groups/ (an array of the streamline indices that
       * belong to it). Per-vertex threshold values are stored under dpv/ and
       * per-streamline values under dps/, so the threshold used at export time
       * is preserved with the data.
       *
       * Streamlines are accumulated in memory and the archive is written by
       * close() (or the destructor). */
      class TRXWriter
      { NOMEMALIGN
        public:
          TRXWriter (const std::string& path) :
              path (path), closed (false) { }

          ~TRXWriter () { if (!closed) { try { close(); } catch (...) { } } }

          //! Begin a new named group (tractogram). Streamlines added after this
          //  call are recorded as members of \a name.
          void begin_group (const std::string& name) {
            current_group = sanitise (name);
            if (group_order.empty() || group_order.back() != current_group)
              group_order.push_back (current_group);
          }

          //! Add one streamline. \a dpv (if non-null) holds one threshold scalar
          //  per vertex (size must equal tck.size()); \a dps (if non-null) holds
          //  a single per-streamline threshold scalar.
          void add (const Streamline<float>& tck, const vector<float>* dpv, const float* dps)
          {
            const uint64_t index = nb_streamlines;
            offsets.push_back (nb_vertices);
            for (const auto& p : tck) {
              positions.push_back (p[0]);
              positions.push_back (p[1]);
              positions.push_back (p[2]);
            }
            const float nan = std::numeric_limits<float>::quiet_NaN();
            for (size_t i = 0; i != tck.size(); ++i) {
              if (dpv) { dpv_data.push_back ((*dpv)[i]); have_dpv = true; }
              else      dpv_data.push_back (nan);
            }
            if (dps) { dps_data.push_back (*dps); have_dps = true; }
            else      dps_data.push_back (nan);

            if (!current_group.empty())
              groups[current_group].push_back (uint32_t (index));

            nb_vertices += tck.size();
            ++nb_streamlines;
          }

          void close ()
          {
            if (closed)
              return;
            closed = true;

            // header.json (positions are already RASMM, so the affine is identity)
            std::string header = "{";
            header += "\"DIMENSIONS\": [1, 1, 1], ";
            header += "\"VOXEL_TO_RASMM\": [[1.0,0.0,0.0,0.0],[0.0,1.0,0.0,0.0],[0.0,0.0,1.0,0.0],[0.0,0.0,0.0,1.0]], ";
            header += "\"NB_VERTICES\": " + str (nb_vertices) + ", ";
            header += "\"NB_STREAMLINES\": " + str (nb_streamlines);
            header += "}";

            std::ofstream out (path, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
            if (!out)
              throw Exception ("error creating TRX file \"" + path + "\"");

            add_entry (out, "header.json",
                       reinterpret_cast<const uint8_t*> (header.data()), header.size());
            add_entry (out, "positions.3.float32",
                       reinterpret_cast<const uint8_t*> (positions.data()), positions.size() * sizeof (float));
            add_entry (out, "offsets.uint64",
                       reinterpret_cast<const uint8_t*> (offsets.data()), offsets.size() * sizeof (uint64_t));
            for (const auto& name : group_order) {
              const auto& members = groups[name];
              add_entry (out, "groups/" + name + ".uint32",
                         reinterpret_cast<const uint8_t*> (members.data()), members.size() * sizeof (uint32_t));
            }
            if (have_dpv)
              add_entry (out, "dpv/threshold.float32",
                         reinterpret_cast<const uint8_t*> (dpv_data.data()), dpv_data.size() * sizeof (float));
            if (have_dps)
              add_entry (out, "dps/threshold.float32",
                         reinterpret_cast<const uint8_t*> (dps_data.data()), dps_data.size() * sizeof (float));

            write_central_directory (out);
            out.close();
            if (!out)
              throw Exception ("error writing TRX file \"" + path + "\"");
          }

        protected:
          struct Entry { NOMEMALIGN
            std::string name;
            uint32_t crc, size, local_offset;
          };

          const std::string path;
          bool closed;
          std::string current_group;
          vector<std::string> group_order;

          uint64_t nb_vertices = 0, nb_streamlines = 0;
          vector<float> positions;
          vector<uint64_t> offsets;
          vector<float> dpv_data, dps_data;
          bool have_dpv = false, have_dps = false;
          std::map<std::string, vector<uint32_t>> groups;
          vector<Entry> entries;

          static std::string sanitise (const std::string& name) {
            std::string s = name;
            const size_t slash = s.find_last_of ("/\\");
            if (slash != std::string::npos)
              s = s.substr (slash + 1);
            const size_t dot = s.find_last_of ('.');
            if (dot != std::string::npos && dot != 0)
              s = s.substr (0, dot);
            for (auto& c : s)
              if (c == '/' || c == '\\' || c == ':')
                c = '_';
            return s.empty() ? "group" : s;
          }

          static void w16 (std::ofstream& o, uint16_t v) { uint8_t b[2] = { uint8_t(v), uint8_t(v>>8) }; o.write (reinterpret_cast<char*>(b), 2); }
          static void w32 (std::ofstream& o, uint32_t v) { uint8_t b[4] = { uint8_t(v), uint8_t(v>>8), uint8_t(v>>16), uint8_t(v>>24) }; o.write (reinterpret_cast<char*>(b), 4); }

          // Write one "stored" (uncompressed) ZIP entry: local header + data.
          void add_entry (std::ofstream& out, const std::string& name, const uint8_t* data, size_t size)
          {
            if (size > 0xFFFFFFFFu)
              throw Exception ("TRX entry \"" + name + "\" exceeds the 4 GB ZIP limit");
            Entry e;
            e.name = name;
            e.size = uint32_t (size);
            e.crc = uint32_t (crc32 (crc32 (0L, Z_NULL, 0), data, uInt (size)));
            e.local_offset = uint32_t (out.tellp());

            w32 (out, 0x04034b50);          // local file header signature
            w16 (out, 20);                  // version needed
            w16 (out, 0);                   // flags
            w16 (out, 0);                   // method 0 = stored
            w16 (out, 0); w16 (out, 0);     // mod time / date
            w32 (out, e.crc);
            w32 (out, e.size);              // compressed size
            w32 (out, e.size);              // uncompressed size
            w16 (out, uint16_t (name.size()));
            w16 (out, 0);                   // extra length
            out.write (name.data(), name.size());
            if (size)
              out.write (reinterpret_cast<const char*> (data), size);

            entries.push_back (e);
          }

          void write_central_directory (std::ofstream& out)
          {
            const uint32_t cd_offset = uint32_t (out.tellp());
            for (const auto& e : entries) {
              w32 (out, 0x02014b50);        // central directory header signature
              w16 (out, 20);                // version made by
              w16 (out, 20);                // version needed
              w16 (out, 0);                 // flags
              w16 (out, 0);                 // method
              w16 (out, 0); w16 (out, 0);   // mod time / date
              w32 (out, e.crc);
              w32 (out, e.size);
              w32 (out, e.size);
              w16 (out, uint16_t (e.name.size()));
              w16 (out, 0);                 // extra length
              w16 (out, 0);                 // comment length
              w16 (out, 0);                 // disk number start
              w16 (out, 0);                 // internal attributes
              w32 (out, 0);                 // external attributes
              w32 (out, e.local_offset);
              out.write (e.name.data(), e.name.size());
            }
            const uint32_t cd_size = uint32_t (out.tellp()) - cd_offset;

            w32 (out, 0x06054b50);          // end of central directory signature
            w16 (out, 0);                   // disk number
            w16 (out, 0);                   // disk with central directory
            w16 (out, uint16_t (entries.size()));
            w16 (out, uint16_t (entries.size()));
            w32 (out, cd_size);
            w32 (out, cd_offset);
            w16 (out, 0);                   // comment length
          }
      };


    }
  }
}

#endif
