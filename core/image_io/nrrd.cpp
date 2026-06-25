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
#include <zlib.h>

#include "image_io/nrrd.h"
#include "header.h"
#include "image_helpers.h"

namespace MR
{
  namespace ImageIO
  {

    void NRRD::load (const Header& header, size_t)
    {
      assert (files.size() == 1);

      // The decompressed buffer holds the data in its STORED datatype layout
      // (MRtrix converts to the access type on the fly); the buffer_size
      // argument refers to the access type and is therefore ignored here.
      segsize = (header.datatype().bits() * voxel_count (header) + 7) / 8;
      try {
        addresses.resize (1);
        addresses[0].reset (new uint8_t [segsize]);
      } catch (...) {
        throw Exception ("Error allocating memory for NRRD image \"" + header.name() + "\"");
      }
      uint8_t* const out = addresses[0].get();
      const size_t buffer_size = segsize;

      std::ifstream in (files[0].name, std::ios_base::binary);
      if (!in)
        throw Exception ("Error opening NRRD data file \"" + files[0].name + "\"");
      in.seekg (files[0].start);

      // Read the entire compressed block (always smaller than the volume we are
      // about to allocate), then inflate it in a single pass.
      const vector<char> comp ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
      if (comp.empty())
        throw Exception ("no data found in NRRD image \"" + header.name() + "\"");

      z_stream strm;
      memset (&strm, 0, sizeof (strm));
      // 15 (max window bits) + 32 enables automatic detection of both the
      // gzip and zlib stream headers that may be produced by NRRD writers.
      if (inflateInit2 (&strm, 15 + 32) != Z_OK)
        throw Exception ("Error initialising zlib for NRRD image \"" + header.name() + "\"");

      strm.next_in = reinterpret_cast<Bytef*> (const_cast<char*> (comp.data()));
      strm.avail_in = static_cast<uInt> (comp.size());
      strm.next_out = out;
      strm.avail_out = static_cast<uInt> (buffer_size);

      const int ret = inflate (&strm, Z_FINISH);
      const uInt remaining = strm.avail_out;
      const std::string msg = strm.msg ? strm.msg : str (ret);
      inflateEnd (&strm);

      if (ret != Z_STREAM_END && remaining != 0)
        throw Exception ("zlib inflate error for NRRD image \"" + header.name() + "\": " + msg);
      if (remaining != 0)
        throw Exception ("NRRD image \"" + header.name() + "\" data was truncated during decompression");
    }

  }
}
