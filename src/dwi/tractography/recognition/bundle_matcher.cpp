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

#include "dwi/tractography/recognition/bundle_matcher.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {
      namespace Recognition
      {


        float VoxelHashGrid::nearest_distance (const Eigen::Vector3f& p) const
        {
          const int32_t ci = int32_t (std::floor (p[0]/cell));
          const int32_t cj = int32_t (std::floor (p[1]/cell));
          const int32_t ck = int32_t (std::floor (p[2]/cell));

          float best = std::numeric_limits<float>::infinity();
          for (int32_t di = -1; di <= 1; ++di) {
            for (int32_t dj = -1; dj <= 1; ++dj) {
              for (int32_t dk = -1; dk <= 1; ++dk) {
                const auto it = cells.find (pack (ci+di, cj+dj, ck+dk));
                if (it == cells.end())
                  continue;
                for (const auto& q : it->second)
                  best = std::min (best, (p - q).norm());
              }
            }
          }
          return best;
        }



        BundleMatcher::BundleMatcher (const vector<Streamline<float>>& atlas_bundle, const Params& params) :
            params_ (params),
            endpoints_ (std::max (4.0f, params.max_endpoint_dist)),
            min_length_ (std::numeric_limits<float>::infinity()),
            max_length_ (0.0f)
        {
          if (atlas_bundle.empty())
            throw Exception ("cannot build a bundle matcher from an empty bundle");

          for (const auto& tck : atlas_bundle) {
            if (tck.size() < 2)
              continue;
            const float length = track_length (tck);
            min_length_ = std::min (min_length_, length);
            max_length_ = std::max (max_length_, length);
            endpoints_.insert (tck.front());
            endpoints_.insert (tck.back());
          }

          if (!std::isfinite (min_length_))
            throw Exception ("bundle contains no usable streamlines");

          if (params_.metric != Metric::MDF) {
            // Cells must be at least as big as the distance we care about, since
            // nearest_distance() only searches the 27 surrounding cells.
            vertices_.reset (new VoxelHashGrid (std::max (4.0f, params_.max_mdf)));
            for (const auto& tck : atlas_bundle)
              for (const auto& p : tck)
                vertices_->insert (p);
            return;   // no centroids needed for this metric
          }

          // Clustering only pays for itself on a bundle large enough that the
          // centroids still describe it; below that, compare against everything.
          if (atlas_bundle.size() >= params_.min_streamlines_for_clustering) {
            QuickBundles qb (atlas_bundle, params_.qb_threshold, params_.num_points);
            references_ = qb.centroids();
          } else {
            FixedTrack fixed;
            for (const auto& tck : atlas_bundle) {
              to_fixed (tck, params_.num_points, fixed);
              if (fixed.rows())
                references_.push_back (fixed);
            }
          }

          if (references_.empty())
            throw Exception ("bundle produced no reference shapes");
        }



        float BundleMatcher::distance_mdf (const Streamline<float>& tck) const
        {
          FixedTrack fixed;
          to_fixed (tck, params_.num_points, fixed);
          if (!fixed.rows())
            return std::numeric_limits<float>::infinity();
          float best = std::numeric_limits<float>::infinity();
          for (const auto& reference : references_)
            best = std::min (best, mdf_flip (reference, fixed));
          return best;
        }



        float BundleMatcher::distance_closest_point (const Streamline<float>& tck) const
        {
          if (!vertices_)
            return std::numeric_limits<float>::infinity();
          // Mean over the candidate's vertices of the distance to the nearest
          // bundle vertex. Distances beyond the grid's reach are capped rather
          // than treated as infinite, so one stray excursion does not by itself
          // reject an otherwise good streamline - the mean handles it.
          const float cap = 3.0f * std::max (1.0f, params_.max_mdf);
          // Sample a bounded number of positions rather than every vertex: a
          // streamline's course is captured by ~20 samples, and the cost of this
          // metric is one grid query per sample.
          const size_t stride = std::max<size_t> (1, tck.size() / params_.num_points);
          vector<float> d;
          d.reserve (params_.num_points + 1);
          for (size_t i = 0; i < tck.size(); i += stride) {
            const float q = vertices_->nearest_distance (tck[i]);
            d.push_back (std::isfinite (q) ? std::min (q, cap) : cap);
          }
          if (d.empty())
            return std::numeric_limits<float>::infinity();

          // All three variants reduce the same per-vertex distances; they differ
          // only in how much weight a local excursion carries.
          switch (params_.metric) {
            case Metric::Hausdorff:
              return *std::max_element (d.begin(), d.end());
            case Metric::ClosestP90: {
              const size_t k = std::min (d.size()-1, size_t (std::floor (0.9 * (d.size()-1))));
              std::nth_element (d.begin(), d.begin()+k, d.end());
              return d[k];
            }
            default: {
              double sum = 0.0;
              for (const float q : d) sum += q;
              return float (sum / d.size());
            }
          }
        }



        float BundleMatcher::distance (const Streamline<float>& tck) const
        {
          constexpr float rejected = std::numeric_limits<float>::infinity();

          if (tck.size() < 2)
            return rejected;

          // 1. Length: a streamline far outside the bundle's own range is not it.
          const float length = track_length (tck);
          if (length < params_.min_length_ratio * min_length_ ||
              length > params_.max_length_ratio * max_length_)
            return rejected;

          // 2. Endpoints: both ends should land near where the bundle terminates.
          if (params_.max_endpoint_dist > 0.0f && !endpoints_.empty()) {
            if (endpoints_.nearest_distance (tck.front()) > params_.max_endpoint_dist ||
                endpoints_.nearest_distance (tck.back())  > params_.max_endpoint_dist)
              return rejected;
          }

          // 3. Shape.
          return params_.metric == Metric::MDF
               ? distance_mdf (tck)
               : distance_closest_point (tck);
        }


      }
    }
  }
}
