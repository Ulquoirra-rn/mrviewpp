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

#include <cstdlib>

#include <QCoreApplication>

#include "gui/mrview/data_path.h"

#include "exception.h"
#include "file/config.h"
#include "file/path.h"
#include "types.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {

      namespace
      {
        const char* const data_subdir = "mrviewpp";

        void add_candidate (vector<std::string>& out, const std::string& dir, const std::string& name)
        {
          if (dir.size())
            out.push_back (Path::join (dir, name));
        }
      }



      std::string find_data_file (const std::string& name)
      {
        vector<std::string> candidates;

        if (const char* env = getenv ("MRVIEWPP_DATA"))
          add_candidate (candidates, env, name);

        //CONF option: MRViewDataPath
        //CONF default: unset
        //CONF Directory holding MRView++'s bundled data (the tract atlas and its
        //CONF template). Set this if the data has been installed somewhere the
        //CONF automatic search below does not cover.
        add_candidate (candidates, File::Config::get ("MRViewDataPath"), name);

        // QCoreApplication::applicationDirPath needs an instance; when called
        // before one exists (or from a non-GUI context) fall back to the other
        // candidates rather than crashing.
        if (QCoreApplication::instance()) {
          const std::string bin (QCoreApplication::applicationDirPath().toStdString());
          // macOS: Contents/MacOS/mrview -> Contents/Resources/mrviewpp
          add_candidate (candidates, Path::join (Path::join (Path::dirname (bin), "Resources"), data_subdir), name);
          // installed layout: <prefix>/bin/mrview -> <prefix>/share/mrtrix3/mrviewpp
          add_candidate (candidates, Path::join (Path::join (Path::join (Path::dirname (bin), "share"), "mrtrix3"), data_subdir), name);
          // Portable layouts (Windows dist/, and the .deb, which puts mrview in
          // /usr/lib/mrview++): data in a share/ directory beside the executable.
          add_candidate (candidates, Path::join (Path::join (bin, "share"), data_subdir), name);
        }

        for (const auto& candidate : candidates) {
          if (Path::is_file (candidate))
            return candidate;
        }
        return std::string();
      }



      std::string data_file (const std::string& name)
      {
        const std::string found = find_data_file (name);
        if (found.empty())
          throw Exception ("could not find bundled data file \"" + name + "\"; "
                           "set the MRViewDataPath config option or the MRVIEWPP_DATA "
                           "environment variable to the directory containing it");
        return found;
      }


    }
  }
}
