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

#ifndef __gui_mrview_data_path_h__
#define __gui_mrview_data_path_h__

#include <string>

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {

      //! Absolute path of a file shipped in MRView++'s own data directory.
      /*! Files live in share/mrtrix3/mrviewpp in the source tree, and are
       *  installed alongside the binary by the packaging scripts. The search
       *  order is: the MRVIEWPP_DATA environment variable, the MRViewDataPath
       *  config option, then locations relative to the running executable
       *  (macOS .app Resources, ../share/mrtrix3/mrviewpp, and the source tree).
       *
       *  Throws if \a name cannot be found, so callers should treat the data as
       *  optional and degrade gracefully - a packaging slip must not stop
       *  MRView++ from starting. */
      std::string data_file (const std::string& name);

      //! As data_file(), but returns an empty string instead of throwing.
      std::string find_data_file (const std::string& name);

    }
  }
}

#endif
