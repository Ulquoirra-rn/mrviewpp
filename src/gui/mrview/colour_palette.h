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

#ifndef __gui_mrview_colour_palette_h__
#define __gui_mrview_colour_palette_h__

#include <array>
#include <cmath>

#include "gui/opengl/gl.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {

      //! A visually well-separated solid colour for the given index.
      /*! Uses golden-angle hue spacing (137.5 degrees) with high saturation and
       * value, so any number of consecutive indices are easy to tell apart. */
      inline std::array<GLubyte,3> distinct_colour (size_t index)
      {
        const float hue = std::fmod (index * 137.508f, 360.0f) / 60.0f;   // sector [0,6)
        // Alternate saturation/value slightly so repeats after a full hue wrap
        // still differ.
        const float sat = 0.65f + 0.2f * ((index / 6) % 2);
        const float val = 1.0f  - 0.15f * ((index / 12) % 2);
        const float c = val * sat;
        const float x = c * (1.0f - std::fabs (std::fmod (hue, 2.0f) - 1.0f));
        const float m = val - c;
        float r = 0, g = 0, b = 0;
        switch (int (hue)) {
          case 0: r = c; g = x; b = 0; break;
          case 1: r = x; g = c; b = 0; break;
          case 2: r = 0; g = c; b = x; break;
          case 3: r = 0; g = x; b = c; break;
          case 4: r = x; g = 0; b = c; break;
          default: r = c; g = 0; b = x; break;
        }
        return { { GLubyte (std::lround ((r + m) * 255.0f)),
                   GLubyte (std::lround ((g + m) * 255.0f)),
                   GLubyte (std::lround ((b + m) * 255.0f)) } };
      }

    }
  }
}

#endif
