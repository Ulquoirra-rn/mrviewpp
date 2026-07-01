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

#ifndef __image_io_dicom_compressed_h__
#define __image_io_dicom_compressed_h__

#include "image_io/base.h"
#include "file/dicom/element.h"

namespace MR
{
  namespace ImageIO
  {

    //! Loads DICOM images stored with a compressed (encapsulated) transfer
    //! syntax (RLE, JPEG, JPEG 2000), decoding each frame into a RAM buffer.
    class DICOMCompressed : public Base
    { NOMEMALIGN
      public:
        DICOMCompressed (const Header& header, File::Dicom::TransferSyntax codec,
                         size_t rows, size_t cols, size_t bits, size_t samples, bool is_signed) :
          Base (header), codec (codec), rows (rows), cols (cols),
          bits (bits), samples (samples), is_signed (is_signed) {
            segsize = header.size(0) * header.size(1) * header.size(2);
          }

      protected:
        File::Dicom::TransferSyntax codec;
        size_t rows, cols, bits, samples;
        bool is_signed;

        virtual void load (const Header&, size_t);
        virtual void unload (const Header&) { }
    };

  }
}

#endif
