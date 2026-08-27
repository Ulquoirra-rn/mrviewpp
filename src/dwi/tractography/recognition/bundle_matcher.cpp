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

          // Squared throughout, with one square root at the end: the comparison
          // needs no root, and this is the innermost loop of every distance in the
          // module.
          float best = std::numeric_limits<float>::infinity();
          for (int32_t di = -1; di <= 1; ++di) {
            for (int32_t dj = -1; dj <= 1; ++dj) {
              for (int32_t dk = -1; dk <= 1; ++dk) {
                const auto it = cells.find (pack (ci+di, cj+dj, ck+dk));
                if (it == cells.end())
                  continue;
                for (const auto& q : it->second)
                  best = std::min (best, (p - q).squaredNorm());
              }
            }
          }
          return std::sqrt (best);
        }



        bool VoxelHashGrid::any_within (const Eigen::Vector3f& p, float radius) const
        {
          if (!(radius >= 0.0f))
            return false;
          const int32_t ci = int32_t (std::floor (p[0]/cell));
          const int32_t cj = int32_t (std::floor (p[1]/cell));
          const int32_t ck = int32_t (std::floor (p[2]/cell));
          const float r2 = radius * radius;

          // The centre cell first: a point that has one near it usually has it here,
          // and finding one ends the search.
          for (int32_t di = -1; di <= 1; ++di) {
            for (int32_t dj = -1; dj <= 1; ++dj) {
              for (int32_t dk = -1; dk <= 1; ++dk) {
                // Nothing in a cell can be nearer than the cell itself. Cheap to
                // check and it removes most of the 27 whenever the radius is small.
                const float dx = std::max (0.0f, std::abs (p[0] - (ci+di)*cell - 0.5f*cell) - 0.5f*cell);
                const float dy = std::max (0.0f, std::abs (p[1] - (cj+dj)*cell - 0.5f*cell) - 0.5f*cell);
                const float dz = std::max (0.0f, std::abs (p[2] - (ck+dk)*cell - 0.5f*cell) - 0.5f*cell);
                if (dx*dx + dy*dy + dz*dz > r2)
                  continue;
                const auto it = cells.find (pack (ci+di, cj+dj, ck+dk));
                if (it == cells.end())
                  continue;
                for (const auto& q : it->second)
                  if ((p - q).squaredNorm() <= r2)
                    return true;
              }
            }
          }
          return false;
        }



        bool VoxelHashGrid::nearest_point (const Eigen::Vector3f& p, Eigen::Vector3f& out) const
        {
          const int32_t ci = int32_t (std::floor (p[0]/cell));
          const int32_t cj = int32_t (std::floor (p[1]/cell));
          const int32_t ck = int32_t (std::floor (p[2]/cell));

          float best = std::numeric_limits<float>::infinity();
          bool found = false;
          for (int32_t di = -1; di <= 1; ++di) {
            for (int32_t dj = -1; dj <= 1; ++dj) {
              for (int32_t dk = -1; dk <= 1; ++dk) {
                const auto it = cells.find (pack (ci+di, cj+dj, ck+dk));
                if (it == cells.end())
                  continue;
                for (const auto& q : it->second) {
                  const float d = (p - q).norm();
                  if (d < best) {
                    best = d;
                    out = q;
                    found = true;
                  }
                }
              }
            }
          }
          return found;
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



        bool BundleMatcher::closer_than (const Streamline<float>& tck, float bound) const
        {
          if (!(bound > 0.0f))
            return false;
          // MDF has no per-sample structure to stop early on.
          if (!uses_vertex_grid() || !vertices_)
            return distance (tck) < bound;
          if (tck.size() < 2)
            return false;

          // distance_closest_point() caps a sample at this, so once the cap is below
          // the bound every sample is too, whatever the bundle is.
          const float cap = 3.0f * std::max (1.0f, params_.max_mdf);
          if (cap < bound)
            return true;

          const size_t stride = std::max<size_t> (1, tck.size() / params_.num_points);
          size_t total = 0;
          for (size_t i = 0; i < tck.size(); i += stride)
            ++total;
          if (!total)
            return false;

          switch (params_.metric) {
            case Metric::Hausdorff: {
              // The largest sample decides, so one sample without a vertex within
              // the bound settles it.
              for (size_t i = 0; i < tck.size(); i += stride)
                if (!vertices_->any_within (tck[i], bound))
                  return false;
              return true;
            }
            case Metric::ClosestP90: {
              // p90 exceeds the bound once more than a tenth of the samples do.
              const size_t k = std::min (total-1, size_t (std::floor (0.9 * (total-1))));
              const size_t allowed = total - 1 - k;   // how many may exceed it
              size_t over = 0;
              for (size_t i = 0; i < tck.size(); i += stride) {
                if (!vertices_->any_within (tck[i], bound) && ++over > allowed)
                  return false;
              }
              return true;
            }
            default: {
              // The mean needs the distances themselves, but not all of them: the
              // sum can already be past total*bound with samples still to come.
              const float budget = bound * float (total);
              float sum = 0.0f;
              for (size_t i = 0; i < tck.size(); i += stride) {
                const float q = vertices_->nearest_distance (tck[i]);
                sum += std::isfinite (q) ? std::min (q, cap) : cap;
                if (sum >= budget)
                  return false;
              }
              return true;
            }
          }
        }



        BundleScale suggest_bundle_thresholds (const vector<Streamline<float>>& bundle,
                                               int metric,
                                               size_t max_sample)
        {
          // Defaults if there is nothing to measure: the same fixed values the GUI
          // would otherwise use.
          const bool closest_point_metric = (metric == int (BundleMatcher::Metric::ClosestMean) ||
                                             metric == int (BundleMatcher::Metric::ClosestP90));
          BundleScale out { closest_point_metric ? 4.0f : 10.0f, 8.0f, 2.0f };
          if (bundle.size() < 8)
            return out;

          // Alternate streamlines into the two halves rather than splitting the
          // file order: bundles are often written grouped by seed region, and a
          // contiguous split would compare one end against the other.
          vector<Streamline<float>> reference, probe;
          const size_t stride = std::max<size_t> (1, bundle.size() / max_sample);
          for (size_t i = 0; i < bundle.size(); i += stride) {
            if ((i / stride) % 2)
              probe.push_back (bundle[i]);
            else
              reference.push_back (bundle[i]);
          }
          if (reference.size() < 4 || probe.size() < 4)
            return out;

          BundleMatcher::Params params;
          params.metric = BundleMatcher::Metric (metric);
          params.max_mdf = std::numeric_limits<float>::infinity();   // measuring, not filtering
          params.max_endpoint_dist = 0.0f;
          params.min_length_ratio = 0.0f;
          params.max_length_ratio = std::numeric_limits<float>::infinity();
          BundleMatcher matcher (reference, params);

          vector<float> distances;
          distances.reserve (probe.size());
          for (const auto& tck : probe) {
            const float d = matcher.distance (tck);
            if (std::isfinite (d))
              distances.push_back (d);
          }
          if (distances.size() < 4)
            return out;
          std::sort (distances.begin(), distances.end());

          auto percentile = [&distances] (float fraction) {
            const size_t index = std::min (distances.size() - 1,
                                           size_t (fraction * float (distances.size() - 1) + 0.5f));
            return distances[index];
          };

          // The 90th percentile keeps nearly every genuine member without letting a
          // single outlier set the threshold; the median is the typical separation,
          // which is the right scale for the clustering radius.
          //
          // Both are then doubled, because this measures atlas members against each
          // other and they are far more alike than a tracked candidate will be:
          // every one came from the same pipeline on the same averaged data.
          // Measured on the bundled atlas, the raw 90th percentile runs about 2.7
          // to 6.2 mm under the Hausdorff metric, so doubling puts a typical bundle
          // near the fixed default while a sparse one lands well above it.
          constexpr float candidate_allowance = 2.0f;
          out.distance = candidate_allowance * percentile (0.9f);
          out.qb_radius = candidate_allowance * percentile (0.5f);

          // Loosen only: the metric's default is the floor, never a starting point
          // to tighten from. What this function measures is how well the atlas
          // agrees with itself, which says nothing about how far the subject's tract
          // sits from it - and that offset, from residual registration error and
          // ordinary anatomy, is what the threshold has to absorb. Letting a densely
          // sampled bundle earn a strict threshold is therefore backwards, and it
          // was measured doing real damage: on the test FOD it gave AF_L 4.2 mm,
          // keeping 33 of 454 candidates where 10 mm keeps 299, and OR_L a threshold
          // that kept none at all. Sparse bundles may still go well above the
          // default, which is the safe direction.
          const float fixed_default = closest_point_metric ? 4.0f : 10.0f;
          out.distance = std::min (2.5f * fixed_default, std::max (fixed_default, out.distance));
          out.qb_radius = std::min (16.0f, std::max (2.0f, out.qb_radius));

          // Deliberately not derived from the bundle: the obvious candidate, the
          // median inter-streamline distance, conflates spatial spacing with shape
          // heterogeneity. Measured on the bundled atlas it ranks the densest bundle
          // (CC, 2477 streamlines) above the sparsest (F_L, 53) - backwards for
          // deciding how much room tracking needs. Dilation follows the tracking
          // image's voxel size instead; see TrackGen::suggest_thresholds_for_bundle.
          return out;
        }

      }
    }
  }
}
