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

#include "dwi/tractography/recognition/quickbundles.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {
      namespace Recognition
      {


        QuickBundles::QuickBundles (const vector<Streamline<float>>& tracks,
                                    float threshold,
                                    size_t num_points) :
            threshold_ (threshold),
            num_points_ (num_points)
        {
          assignment_.assign (tracks.size(), invalid);

          FixedTrack fixed, flipped;
          for (size_t n = 0; n != tracks.size(); ++n) {
            to_fixed (tracks[n], num_points_, fixed);
            if (!fixed.rows())
              continue;

            // Nearest existing centroid.
            size_t best = invalid;
            float best_distance = std::numeric_limits<float>::infinity();
            for (size_t c = 0; c != centroids_.size(); ++c) {
              const float d = mdf_flip (centroids_[c], fixed);
              if (d < best_distance) {
                best_distance = d;
                best = c;
              }
            }

            if (best == invalid || best_distance > threshold_) {
              centroids_.push_back (fixed);
              sizes_.push_back (1);
              assignment_[n] = centroids_.size() - 1;
              continue;
            }

            // Fold into the running mean, orienting to match the centroid first;
            // without that, streamlines tracked in opposite directions would
            // average into a meaningless shape.
            const FixedTrack* to_add = &fixed;
            if (is_flipped (centroids_[best], fixed)) {
              flipped = fixed.colwise().reverse();
              to_add = &flipped;
            }
            const float count = float (sizes_[best]);
            centroids_[best] = (centroids_[best] * count + *to_add) / (count + 1.0f);
            ++sizes_[best];
            assignment_[n] = best;
          }
        }


      }
    }
  }
}
