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

#ifndef __gui_mrview_tool_session_h__
#define __gui_mrview_tool_session_h__

#include "gui/mrview/tool/base.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        // Session tool: save and restore the full scene (base images, overlays,
        // tractograms, meshes, atlases) to/from a JSON file. Each contributing
        // tool serialises itself via the Tool::Base session hooks; the main image
        // list is handled directly through Tool::Base's friendship with Window.
        class Session : public Base
        { MEMALIGN(Session)
            Q_OBJECT

          public:
            Session (Dock* parent);

          private slots:
            void save_slot ();
            void open_slot ();
            void restore_autosave_slot ();

          protected:
            QLabel* status_label;

            Dock* ensure_tool_open (const std::string& tool_name);
        };

      }
    }
  }
}

#endif
