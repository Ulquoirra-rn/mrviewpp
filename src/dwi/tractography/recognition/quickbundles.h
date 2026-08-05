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

#ifndef __dwi_tractography_recognition_quickbundles_h__
#define __dwi_tractography_recognition_quickbundles_h__

#include <limits>

#include "dwi/tractography/recognition/mdf.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {
      namespace Recognition
      {

        //! Single-pass greedy streamline clustering (Garyfallidis et al., QuickBundles).
        /*! Each streamline joins the first existing cluster whose centroid is
         *  within \a threshold under mdf_flip, otherwise it starts a new one; the
         *  centroid is maintained as a running mean, with the streamline flipped
         *  first if that is the closer orientation.
         *
         *  The point of it here is compression: a bundle of thousands of
         *  streamlines collapses to a few dozen centroids that still describe its
         *  shape, so matching a candidate against the bundle costs a few dozen
         *  O(P) comparisons instead of thousands. */
        class QuickBundles { MEMALIGN(QuickBundles)
          public:
            QuickBundles (const vector<Streamline<float>>& tracks,
                          float threshold = 8.0f,
                          size_t num_points = 20);

            const vector<FixedTrack>& centroids () const { return centroids_; }
            const vector<size_t>& cluster_sizes () const { return sizes_; }
            //! Cluster index of each input streamline (invalid for empty ones).
            const vector<size_t>& assignment () const { return assignment_; }

            size_t num_clusters () const { return centroids_.size(); }
            size_t num_points () const { return num_points_; }

            static constexpr size_t invalid = std::numeric_limits<size_t>::max();

          private:
            vector<FixedTrack> centroids_;
            vector<size_t> sizes_;
            vector<size_t> assignment_;
            const float threshold_;
            const size_t num_points_;
        };

      }
    }
  }
}

#endif
