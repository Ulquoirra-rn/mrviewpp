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

#ifndef __gui_mrview_region_source_h__
#define __gui_mrview_region_source_h__

#include <QColor>

#include "image.h"
#include "types.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {

      // A binary region offered by one of the tools, in a form that can be
      // handed to the tracking engine as a seed / include / exclude / mask ROI.
      //
      // Regions live in their owning tool (an overlay image, an atlas label, a
      // hand-drawn ROI), so this is a lightweight reference: the actual voxel
      // data is only materialised on demand, because for the ROI editor that
      // means reading back a GPU texture.
      struct RegionRef { NOMEMALIGN
        std::string provider;   // "ROI editor" | "Overlay" | "Atlas"
        std::string name;       // display name, e.g. "Left-Thalamus"
        std::string key;        // stable id for session save, e.g. "atlas:<path>#12"
        QColor colour;
        size_t index;           // provider-private handle (list row / label value)

        RegionRef () : colour (Qt::white), index (0) { }
        std::string label () const { return provider + ": " + name; }
      };


      // Implemented by any tool that can supply regions. Tools return their
      // provider from Tool::Base::region_provider(); a tool that has never been
      // opened contributes nothing, which is the intended semantics - you can
      // only use regions you can actually see.
      class RegionProvider { NOMEMALIGN
        public:
          virtual ~RegionProvider () { }

          virtual std::string provider_name () const = 0;

          virtual void list_regions (vector<RegionRef>&) const = 0;

          //! Realise a region as a binary image.
          /*! MUST be called on the GUI thread with a current GL context: the ROI
           *  editor keeps its mask only in an OpenGL texture. Throws if the
           *  region has gone away. */
          virtual MR::Image<bool> get_region_mask (const RegionRef&) const = 0;

          //! Re-resolve a saved key after a session reload; false if it is gone.
          virtual bool resolve (const std::string& key, RegionRef&) const = 0;

          //! Set the display opacity of a region, 0-1.
          /*! Used to dim a region acting as "avoid"/exclude so its role is visible
           *  in the viewer, not just in a panel. Only the ROI editor implements
           *  this: an overlay's opacity is the user's own display choice, and the
           *  Atlas has no per-region alpha (it dims by focus instead). */
          virtual void set_region_opacity (const RegionRef&, float) { }
      };


      //! Opacity applied to a region acting as "avoid" / exclude.
      /*! Dimming it makes the role visible in the viewer, not just in a panel. */
      constexpr float avoid_region_opacity = 0.2f;

      //! Every region currently offered by every open tool.
      void collect_regions (vector<RegionRef>&);

      //! Find the provider that owns a region reference (nullptr if gone).
      RegionProvider* provider_for (const RegionRef&);

    }
  }
}

#endif
