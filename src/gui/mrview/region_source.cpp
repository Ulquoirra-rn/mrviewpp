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

#include <functional>

#include "gui/mrview/region_source.h"

#include "gui/mrview/window.h"
#include "gui/mrview/tool/base.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {

      namespace
      {
        void for_each_provider (const std::function<void(RegionProvider*)>& fn)
        {
          if (!Window::main)
            return;
          QList<QAction*> actions = Window::main->tools()->actions();
          for (int i = 0; i != actions.size(); ++i) {
            Tool::__Action__* action = dynamic_cast<Tool::__Action__*> (actions[i]);
            // A tool that has never been opened has no dock, and therefore no
            // regions - deliberately, so we never force tools open just to scan.
            if (!action || !action->dock || !action->dock->tool)
              continue;
            if (RegionProvider* provider = action->dock->tool->region_provider())
              fn (provider);
          }
        }
      }



      void collect_regions (vector<RegionRef>& out)
      {
        out.clear();
        for_each_provider ([&out] (RegionProvider* provider) {
          try {
            provider->list_regions (out);
          } catch (Exception& e) {
            e.display();
          }
        });
      }



      RegionProvider* provider_for (const RegionRef& region)
      {
        RegionProvider* found = nullptr;
        for_each_provider ([&] (RegionProvider* provider) {
          if (!found && provider->provider_name() == region.provider)
            found = provider;
        });
        return found;
      }


    }
  }
}
