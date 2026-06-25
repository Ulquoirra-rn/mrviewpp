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

#ifndef __image_io_nrrd_h__
#define __image_io_nrrd_h__

#include "image_io/base.h"

namespace MR
{
  namespace ImageIO
  {

    //! Loads NRRD image data whose encoding requires decompression into RAM.
    /*! Used for NRRD files with "encoding: gzip" (and "gz"), where the data
     * block cannot simply be memory-mapped. The single File::Entry provides
     * the path to the (possibly detached) data file and the byte offset at
     * which the compressed stream begins. The whole volume is inflated into a
     * RAM buffer on load. Raw (uncompressed) NRRD data uses ImageIO::Default
     * instead and never reaches this class. */
    class NRRD : public Base
    { NOMEMALIGN
      public:
        NRRD (const Header& header) : Base (header) { }

        // is_file_backed() inherits the base default (true): the data is held
        // in its stored datatype and accessed via indirect IO with on-the-fly
        // conversion, exactly as for the PNG and Default handlers.

      protected:
        virtual void load (const Header&, size_t);
        virtual void unload (const Header&) { }
    };

  }
}

#endif
