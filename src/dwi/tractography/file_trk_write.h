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

#ifndef __dwi_tractography_file_trk_write_h__
#define __dwi_tractography_file_trk_write_h__

#include <cstring>
#include <fstream>
#include <cmath>
#include <limits>

#include "types.h"
#include "exception.h"
#include "dwi/tractography/streamline.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {

      //! Writer for TrackVis (.trk) streamline files.
      /*! Streamlines are supplied in MRtrix's scanner (RAS) mm convention and
       * stored in the file's "voxmm" coordinates using a unit (1 mm isotropic)
       * voxel grid whose vox->RAS transform is a pure translation chosen so all
       * stored coordinates are positive. This round-trips exactly through the
       * matching TRKReader.
       *
       * Streamlines are buffered in memory (the header records the bounding box
       * and count) and the file is written by close() / the destructor. */
      class TRKWriter
      { NOMEMALIGN
        public:
          TRKWriter (const std::string& path) :
              path (path), closed (false) { }

          ~TRKWriter () { if (!closed) { try { close(); } catch (...) { } } }

          void operator() (const Streamline<float>& tck) {
            tracks.push_back (tck);
            for (const auto& p : tck) {
              for (size_t i = 0; i != 3; ++i) {
                lo[i] = std::min (lo[i], p[i]);
                hi[i] = std::max (hi[i], p[i]);
              }
            }
          }

          void close ()
          {
            if (closed)
              return;
            closed = true;

            // Unit voxel grid; translation places the bounding box at positive
            // voxmm. Reader does: ras = vox2ras * offset(-0.5) * (voxmm) with
            // voxel_size = 1, i.e. ras = (voxmm - 0.5) + t, so voxmm = ras - t + 0.5.
            float t[3];
            int16_t dim[3];
            for (size_t i = 0; i != 3; ++i) {
              const float lower = tracks.empty() ? 0.0f : lo[i];
              const float upper = tracks.empty() ? 0.0f : hi[i];
              t[i] = std::floor (lower) - 1.0f;
              const double span = double (upper) - double (t[i]) + 1.0;
              dim[i] = int16_t (std::min (32000.0, std::max (1.0, std::ceil (span))));
            }

            std::ofstream out (path, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
            if (!out)
              throw Exception ("error creating TrackVis file \"" + path + "\"");

            char header[1000];
            std::memset (header, 0, sizeof (header));
            std::strcpy (header, "TRACK");
            put<int16_t> (header, 6, dim[0]);
            put<int16_t> (header, 8, dim[1]);
            put<int16_t> (header, 10, dim[2]);
            put<float> (header, 12, 1.0f);   // voxel_size
            put<float> (header, 16, 1.0f);
            put<float> (header, 20, 1.0f);
            put<float> (header, 24, 0.0f);   // origin
            put<float> (header, 28, 0.0f);
            put<float> (header, 32, 0.0f);
            put<int16_t> (header, 36, 0);    // n_scalars
            put<int16_t> (header, 238, 0);   // n_properties
            // vox_to_ras (row-major 4x4 float at offset 440): identity + translation t
            for (size_t r = 0; r != 4; ++r)
              for (size_t c = 0; c != 4; ++c)
                put<float> (header, 440 + 4*(4*r + c), (r == c) ? 1.0f : 0.0f);
            put<float> (header, 440 + 4*(4*0 + 3), t[0]);
            put<float> (header, 440 + 4*(4*1 + 3), t[1]);
            put<float> (header, 440 + 4*(4*2 + 3), t[2]);
            std::strcpy (header + 948, "LPS");      // voxel_order (informational)
            put<int32_t> (header, 988, int32_t (tracks.size())); // n_count
            put<int32_t> (header, 992, 2);          // version
            put<int32_t> (header, 996, 1000);       // hdr_size
            out.write (header, 1000);

            for (const auto& tck : tracks) {
              const int32_t m = int32_t (tck.size());
              out.write (reinterpret_cast<const char*> (&m), sizeof (m));
              for (const auto& p : tck) {
                const float xyz[3] = { p[0] - t[0] + 0.5f, p[1] - t[1] + 0.5f, p[2] - t[2] + 0.5f };
                out.write (reinterpret_cast<const char*> (xyz), sizeof (xyz));
              }
            }
            out.close();
            if (!out)
              throw Exception ("error writing TrackVis file \"" + path + "\"");
          }

        protected:
          const std::string path;
          bool closed;
          vector<Streamline<float>> tracks;
          float lo[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
          float hi[3] = { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };

          template <typename T>
          static void put (char* base, size_t offset, T v) {
            std::memcpy (base + offset, &v, sizeof (T));
          }
      };


    }
  }
}

#endif
