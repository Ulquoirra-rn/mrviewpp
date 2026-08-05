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

#ifndef __dwi_tractography_recognition_bundle_matcher_h__
#define __dwi_tractography_recognition_bundle_matcher_h__

#include <unordered_map>

#include "exception.h"
#include "dwi/tractography/recognition/mdf.h"
#include "dwi/tractography/recognition/quickbundles.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {
      namespace Recognition
      {

        //! Coarse spatial index over a point cloud, for nearest-point queries.
        /*! Points are bucketed into cubic cells; a query only examines the 27
         *  cells around it, so "is there any reference point within r?" costs
         *  O(points per cell) rather than O(all points). */
        class VoxelHashGrid { MEMALIGN(VoxelHashGrid)
          public:
            VoxelHashGrid (float cell_size = 4.0f) : cell (cell_size) { }

            void insert (const Eigen::Vector3f& p) { cells[key (p)].push_back (p); }
            void reserve (size_t n) { cells.reserve (n); }

            //! Distance to the closest inserted point; infinity if none is near.
            /*! Only the 27 neighbouring cells are searched, so a point further
             *  than one cell away is reported as infinity - which is exactly what
             *  a rejection test wants. */
            float nearest_distance (const Eigen::Vector3f& p) const;

            bool any_within (const Eigen::Vector3f& p, float radius) const {
              return nearest_distance (p) <= radius;
            }

            bool empty () const { return cells.empty(); }

          private:
            const float cell;
            std::unordered_map<uint64_t, vector<Eigen::Vector3f>> cells;

            uint64_t key (const Eigen::Vector3f& p) const {
              return pack (int32_t (std::floor (p[0]/cell)),
                           int32_t (std::floor (p[1]/cell)),
                           int32_t (std::floor (p[2]/cell)));
            }
            static uint64_t pack (int32_t i, int32_t j, int32_t k) {
              // 21 bits per axis, biased to keep negative coordinates in range.
              return (uint64_t ((i + (1<<20)) & 0x1FFFFF) << 42)
                   | (uint64_t ((j + (1<<20)) & 0x1FFFFF) << 21)
                   |  uint64_t ((k + (1<<20)) & 0x1FFFFF);
            }
            friend class BundleMatcher;
        };



        //! Decides whether a streamline belongs to a reference bundle.
        /*! This is what makes atlas-driven auto-tracking work: track generously
         *  within the bundle's territory, then keep only the streamlines whose
         *  shape actually matches the atlas bundle.
         *
         *  Scoring is min-over-centroids of the flip-invariant MDF, with the
         *  atlas bundle compressed by QuickBundles first. Cheap rejections run
         *  before that: length ratio, then endpoint proximity. */
        class BundleMatcher { MEMALIGN(BundleMatcher)
          public:
            //! How a candidate's shape is compared to the bundle.
            enum class Metric {
              //! Flip-invariant mean direct-flip distance to the closest centroid.
              /*! Point-to-point at matched relative positions, so it is only
               *  meaningful between streamlines of comparable length. Best for a
               *  bundle whose streamlines are all a similar length. */
              MDF,
              //! Mean distance from each candidate vertex to the nearest bundle vertex.
              /*! Needs no correspondence and no equal length, so it handles a
               *  bundle spanning a wide range of lengths, and a candidate that
               *  covers only part of the bundle's course. Directed on purpose: it
               *  asks "does this streamline run along the bundle", not "is it the
               *  same length as the bundle". */
              ClosestMean,
              //! 90th percentile of the same distances.
              /*! A robust Hausdorff: catches a sustained departure from the bundle
               *  that the mean dilutes, without letting one stray vertex decide. */
              ClosestP90,
              //! Maximum of the same distances - the directed Hausdorff distance.
              /*! The strictest of the three: a single excursion rejects the whole
               *  streamline. Note only the directed form is meaningful here; the
               *  symmetric Hausdorff would additionally demand that one streamline
               *  cover the entire bundle. */
              Hausdorff
            };

            struct Params { NOMEMALIGN
              Metric metric = Metric::MDF;
              size_t num_points = 20;
              float qb_threshold = 8.0f;         //!< atlas compression radius, mm
              float max_mdf = 10.0f;             //!< acceptance threshold, mm
              float max_endpoint_dist = 20.0f;   //!< 0 disables the endpoint filter
              float min_length_ratio = 0.5f;     //!< relative to the atlas length range
              float max_length_ratio = 1.75f;
              size_t min_streamlines_for_clustering = 200;
            };

            BundleMatcher (const vector<Streamline<float>>& atlas_bundle, const Params& params);

            //! Distance from a candidate to the bundle, in mm.
            /*! Returns infinity if a pre-filter rejected it outright. */
            float distance (const Streamline<float>&) const;

            bool matches (const Streamline<float>& tck, float& d) const {
              d = distance (tck);
              return d <= params_.max_mdf;
            }

            const Params& params () const { return params_; }
            size_t num_references () const { return references_.size(); }
            float atlas_min_length () const { return min_length_; }
            float atlas_max_length () const { return max_length_; }

          private:
            Params params_;
            // Either QuickBundles centroids, or every atlas streamline when the
            // bundle is too small for clustering to be worthwhile.
            vector<FixedTrack> references_;
            VoxelHashGrid endpoints_;
            //! All bundle vertices, for the ClosestPoint metric.
            std::unique_ptr<VoxelHashGrid> vertices_;
            float min_length_, max_length_;

            float distance_mdf (const Streamline<float>&) const;
            float distance_closest_point (const Streamline<float>&) const;
            bool uses_vertex_grid () const {
              return params_.metric != Metric::MDF;
            }
        };

      }
    }
  }
}

#endif
