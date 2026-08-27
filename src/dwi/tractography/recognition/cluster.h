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

#ifndef __dwi_tractography_recognition_cluster_h__
#define __dwi_tractography_recognition_cluster_h__

#include <limits>

#include "types.h"
#include "dwi/tractography/streamline.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {
      namespace Recognition
      {

        //! Streamline clustering on a few sampled points and a length, DSI Studio style.
        /*! Each streamline is reduced to points sampled along it plus its length, and
         *  two streamlines are compared by the distances between their corresponding
         *  points:
         *
         *      d(a,b) = sum_i |a_i - b_i| + w |length(a) - length(b)|
         *
         *  taken as the smaller of the two ways round, since a track file's vertex
         *  order is arbitrary. Every term is a distance in millimetres.
         *
         *  This needs no point correspondence and no pairwise distance matrix over
         *  whole streamlines, so it scales to a full tractogram, and it separates
         *  bundles by where they begin, end and turn rather than by how much they
         *  overlap along their length.
         */

        //! Points sampled along each streamline, plus its length as a final element.
        /*! Three points - the two ends and the middle - is what DSI Studio uses, and
         *  is the default. More can be sampled: with three, two tracts that share
         *  their endpoints and their midpoint are indistinguishable however
         *  differently they run in between, which is exactly the case for the
         *  corticospinal tract against the medial lemniscus. */
        using ClusterFeature = Eigen::VectorXf;
        constexpr size_t cluster_default_points = 3;
        //! Weight on the length difference, relative to a point distance in mm.
        /*! A whole bundle can span 100 mm of length (AF_L runs 76-171 mm in the
         *  built-in atlas), which is larger than the distances between its
         *  streamlines' sampled points - so at full weight the length term splits a
         *  long bundle by length instead of separating neighbouring bundles. */
        constexpr float cluster_default_length_weight = 0.25f;

        //! Sample  num_points equally spaced by arc length, ends included, + length.
        /*! Orientation is left as read from file; comparisons handle it per pair,
         *  which is the only way that is meaningful when the set spans several
         *  bundles. */
        ClusterFeature cluster_features (const Streamline<float>& tck,
                                         size_t num_points = cluster_default_points);

        //! The same feature vector with its sampled points reversed.
        ClusterFeature cluster_flip (const ClusterFeature&);

        //! Flip-invariant distance between two feature vectors, in mm.
        float cluster_distance (const ClusterFeature& a, const ClusterFeature& b,
                                float length_weight = cluster_default_length_weight);

        //! Distance in the given orientation only, in mm.
        float cluster_distance_oriented (const ClusterFeature& a, const ClusterFeature& b,
                                         float length_weight = cluster_default_length_weight);

        enum class ClusterMethod {
          //! Hard assignment to the nearest centre (Lloyd's algorithm, k-means++ seeded).
          KMeans,
          //! Average-linkage agglomerative clustering on the full pairwise distance.
          /*! Compares every streamline with every other rather than with K centres,
           *  using the flip-invariant mean point-to-point distance over the whole
           *  streamline (MDF) rather than a few sampled points - the richest relation
           *  of the three, and the closest to what DSI Studio's single-linkage
           *  clustering does.
           *
           *  It costs O(N^2) time and memory, against O(N.K) per iteration for the
           *  other two, so it is limited to hierarchical_max_streamlines. On seven
           *  neighbouring atlas bundles at k=7 it measured worse than either
           *  centroid method - 83% against EM's 90% - so it is offered for
           *  comparison rather than as the default. */
          Hierarchical,
          //! Gaussian mixture with diagonal covariance, fitted by expectation-maximisation.
          /*! Started from the k-means solution. Because each cluster carries its own
           *  per-feature variance, it can hold a bundle that is tight at its ends but
           *  loose in the middle, which k-means - implicitly spherical - cannot. */
          EM
        };

        struct ClusterResult { NOMEMALIGN
          //! Cluster index per input streamline, or `invalid` for an empty one.
          vector<size_t> assignment;
          //! Streamlines per cluster; a cluster may end up empty.
          vector<size_t> sizes;
          size_t num_clusters = 0;
          //! Iterations actually run, for reporting whether it converged.
          size_t iterations = 0;
          bool converged = false;
          //! Mean distance from a streamline to its cluster centre, in mm.
          /*! The objective being minimised, so it says how tight the grouping is and
           *  lets two runs be compared. */
          float mean_distance = 0.0f;

          static constexpr size_t invalid = std::numeric_limits<size_t>::max();
        };

        //! Cluster \a tracks into at most \a num_clusters groups.
        /*! Fewer are returned when the data does not support that many (duplicate
         *  feature vectors, or fewer streamlines than clusters asked for).
         *
         *  k-means is restarted \a restarts times from different seeds and the
         *  tightest result kept: a single run of Lloyd's algorithm settles into
         *  whatever local minimum its seeding leads to, and on real bundles the
         *  spread between restarts is large enough to see. Past about five restarts
         *  it stops helping, and for EM it can hurt slightly - the tightest k-means
         *  solution is not necessarily the best seed for a mixture. */
        //! Above this, Hierarchical refuses to run: N^2 floats is the limit, not time.
        constexpr size_t hierarchical_max_streamlines = 12000;

        struct ClusterOptions { NOMEMALIGN
          //! EM by default: measurably better than k-means on adjacent bundles.
          ClusterMethod method = ClusterMethod::EM;
          size_t max_iterations = 100;
          //! k-means restarts; the tightest result is kept.
          size_t restarts = 5;
          size_t num_points = cluster_default_points;
          float length_weight = cluster_default_length_weight;
          //! Points each streamline is resampled to for the Hierarchical metric.
          size_t mdf_num_points = 20;
        };

        ClusterResult cluster_streamlines (const vector<Streamline<float>>& tracks,
                                           size_t num_clusters,
                                           const ClusterOptions& options = ClusterOptions());

      }
    }
  }
}

#endif
