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

#include "dwi/tractography/recognition/mdf.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {
      namespace Recognition
      {


        void to_fixed (const Streamline<float>& in, size_t num_points, FixedTrack& out)
        {
          if (in.size() < 2 || num_points < 2) {
            out.resize (0, 3);
            return;
          }

          // Cumulative arc length at each input vertex.
          vector<float> arc (in.size(), 0.0f);
          for (size_t i = 1; i != in.size(); ++i)
            arc[i] = arc[i-1] + (in[i] - in[i-1]).norm();
          const float total = arc.back();

          out.resize (num_points, 3);
          if (total <= 0.0f) {
            // Degenerate (all vertices coincident): every sample is that point.
            for (size_t p = 0; p != num_points; ++p)
              out.row (p) = in.front().transpose();
            return;
          }

          const float step = total / float (num_points - 1);
          size_t segment = 0;
          for (size_t p = 0; p != num_points; ++p) {
            const float target = step * float (p);
            while (segment + 2 < in.size() && arc[segment+1] < target)
              ++segment;
            const float span = arc[segment+1] - arc[segment];
            const float frac = span > 0.0f ? (target - arc[segment]) / span : 0.0f;
            out.row (p) = (in[segment] + frac * (in[segment+1] - in[segment])).transpose();
          }
          // Guarantee the endpoints exactly, free of accumulated rounding.
          out.row (0) = in.front().transpose();
          out.row (num_points-1) = in.back().transpose();
        }


      }
    }
  }
}
