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

#ifndef __dwi_tractography_file_trk_h__
#define __dwi_tractography_file_trk_h__

#include <cstring>
#include <fstream>

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

      //! Reader for TrackVis (.trk) streamline files.
      /*! Parses the fixed 1000-byte TrackVis header and yields one streamline
       * per operator() call, converting the stored "voxmm" coordinates into
       * MRtrix's scanner (RAS) mm convention. Little- and big-endian files are
       * both supported; per-point scalars and per-streamline properties are
       * skipped (only geometry is retained). */
      template <class ValueType = float>
      class TRKReader : public ReaderInterface<ValueType>
      { NOMEMALIGN
        public:
          TRKReader (const std::string& file, Properties& properties) :
              in (file, std::ios_base::in | std::ios_base::binary)
          {
            if (!in)
              throw Exception ("error opening TrackVis file \"" + file + "\"");

            char header[1000];
            in.read (header, 1000);
            if (in.gcount() != 1000 || std::strncmp (header, "TRACK", 5) != 0)
              throw Exception ("file \"" + file + "\" is not a valid TrackVis (.trk) file");

            // endianness is determined from the trailing hdr_size field (==1000);
            // read it raw (before 'swap' is known) and compare.
            int32_t hdr_size_raw;
            std::memcpy (&hdr_size_raw, header + 996, sizeof (hdr_size_raw));
            swap = (hdr_size_raw != 1000);

            for (size_t i = 0; i != 3; ++i)
              voxel_size[i] = get<float> (header, 12 + 4*i);
            n_scalars    = get<int16_t> (header, 36);
            n_properties = get<int16_t> (header, 238);
            n_count      = get<int32_t> (header, 988);

            // vox_to_ras: 4x4 row-major float matrix at offset 440
            Eigen::Matrix4d vox2ras;
            for (size_t r = 0; r != 4; ++r)
              for (size_t c = 0; c != 4; ++c)
                vox2ras(r,c) = get<float> (header, 440 + 4*(4*r + c));

            // If vox_to_ras is not recorded (old files leave it zero), fall back
            // to a LPS->RAS mapping built from the voxel sizes.
            if (vox2ras(3,3) == 0.0) {
              vox2ras.setIdentity();
              vox2ras(0,0) = -voxel_size[0];
              vox2ras(1,1) = -voxel_size[1];
              vox2ras(2,2) =  voxel_size[2];
            }

            // TrackVis points are in voxmm (mm in voxel-order space, origin at
            // the corner of the first voxel). Compose: scale voxmm->voxel,
            // shift -0.5 (corner->centre), then voxel->RAS.
            Eigen::Matrix4d scale = Eigen::Matrix4d::Identity();
            for (size_t i = 0; i != 3; ++i)
              scale(i,i) = (voxel_size[i] != 0.0f) ? 1.0 / voxel_size[i] : 0.0;
            Eigen::Matrix4d offset = Eigen::Matrix4d::Identity();
            offset(0,3) = offset(1,3) = offset(2,3) = -0.5;
            voxmm_to_ras = vox2ras * offset * scale;

            if (n_count > 0)
              properties["count"] = str (n_count);
            current_index = 0;
            values_per_point = 3 + n_scalars;
          }


          bool operator() (Streamline<ValueType>& tck)
          {
            tck.clear();
            if (!in.is_open())
              return false;

            int32_t m = 0;
            in.read (reinterpret_cast<char*> (&m), sizeof (m));
            if (in.eof() || in.gcount() != sizeof (m)) {
              in.close();
              return false;
            }
            if (swap) byte_swap (m);
            if (m < 0)
              throw Exception ("corrupt TrackVis file (negative point count)");

            tck.set_index (current_index++);
            tck.weight = 1.0;

            vector<float> row (values_per_point);
            for (int32_t i = 0; i != m; ++i) {
              in.read (reinterpret_cast<char*> (row.data()), values_per_point * sizeof (float));
              if (in.gcount() != std::streamsize (values_per_point * sizeof (float))) {
                in.close();
                throw Exception ("truncated streamline data in TrackVis file");
              }
              float x = row[0], y = row[1], z = row[2];
              if (swap) { byte_swap (x); byte_swap (y); byte_swap (z); }
              const Eigen::Vector4d p = voxmm_to_ras * Eigen::Vector4d (x, y, z, 1.0);
              tck.push_back ({ ValueType(p[0]), ValueType(p[1]), ValueType(p[2]) });
            }

            // skip per-streamline properties
            if (n_properties)
              in.seekg (n_properties * sizeof (float), std::ios_base::cur);

            return true;
          }


        protected:
          std::ifstream in;
          bool swap;
          int16_t n_scalars, n_properties;
          int32_t n_count;
          size_t current_index, values_per_point;
          float voxel_size[3];
          Eigen::Matrix4d voxmm_to_ras;

          template <typename T>
          static void byte_swap (T& v) {
            char* b = reinterpret_cast<char*> (&v);
            for (size_t i = 0; i != sizeof (T) / 2; ++i)
              std::swap (b[i], b[sizeof(T)-1-i]);
          }

          template <typename T>
          T get (const char* base, const size_t offset) const {
            T v;
            std::memcpy (&v, base + offset, sizeof (T));
            if (swap) byte_swap (v);
            return v;
          }
      };


    }
  }
}

#endif
