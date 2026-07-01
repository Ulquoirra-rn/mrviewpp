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

#ifndef __file_dicom_dicom_decode_h__
#define __file_dicom_dicom_decode_h__

#include "types.h"

namespace MR
{
  namespace File
  {
    namespace Dicom
    {

      //! Decode one JPEG-compressed DICOM frame to native-order raw pixels.
      vector<uint8_t> decode_jpeg (const vector<uint8_t>& in, size_t rows, size_t cols,
                                   size_t bits, size_t samples, bool is_signed);

      //! Decode one JPEG 2000-compressed DICOM frame to native-order raw pixels.
      vector<uint8_t> decode_jpeg2000 (const vector<uint8_t>& in, size_t rows, size_t cols,
                                       size_t bits, size_t samples, bool is_signed);

    }
  }
}

#endif
