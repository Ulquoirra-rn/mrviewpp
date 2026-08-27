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

#include "dwi/tractography/recognition/cluster.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "math/rng.h"
#include "dwi/tractography/recognition/mdf.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {
      namespace Recognition
      {

        namespace
        {

          float streamline_length (const Streamline<float>& tck)
          {
            float length = 0.0f;
            for (size_t i = 1; i < tck.size(); ++i)
              length += (tck[i] - tck[i-1]).norm();
            return length;
          }

          //! Point a given fraction along the streamline by arc length, not by index.
          /*! Index would be the same thing only for a constant step size, which a
           *  resampled or hand-edited tractogram does not guarantee. */
          Eigen::Vector3f point_at (const Streamline<float>& tck, float fraction)
          {
            if (fraction <= 0.0f) return tck.front();
            if (fraction >= 1.0f) return tck.back();
            const float target = fraction * streamline_length (tck);
            float travelled = 0.0f;
            for (size_t i = 1; i < tck.size(); ++i) {
              const float step = (tck[i] - tck[i-1]).norm();
              if (travelled + step >= target) {
                const float f = step > 0.0f ? (target - travelled) / step : 0.0f;
                return tck[i-1] + f * (tck[i] - tck[i-1]);
              }
              travelled += step;
            }
            return tck.back();
          }

          inline size_t num_points_of (const ClusterFeature& f)
          {
            return size_t (f.size() - 1) / 3;
          }

        }



        ClusterFeature cluster_features (const Streamline<float>& tck, size_t num_points)
        {
          const size_t P = std::max<size_t> (2, num_points);
          ClusterFeature f (3*P + 1);
          f.setZero();
          if (tck.empty())
            return f;
          for (size_t i = 0; i != P; ++i) {
            const Eigen::Vector3f p = point_at (tck, float (i) / float (P - 1));
            f.segment<3> (3*i) = p;
          }
          f[3*P] = streamline_length (tck);
          return f;
        }



        ClusterFeature cluster_flip (const ClusterFeature& f)
        {
          const size_t P = num_points_of (f);
          ClusterFeature out (f);
          for (size_t i = 0; i != P; ++i)
            out.segment<3> (3*i) = f.segment<3> (3*(P-1-i));
          return out;
        }



        float cluster_distance_oriented (const ClusterFeature& a, const ClusterFeature& b,
                                         float length_weight)
        {
          // Distances between corresponding points, added rather than combined as a
          // Euclidean norm over the concatenated coordinates: a sum of distances lets
          // one point disagree without that disagreement being squared into
          // dominance, which keeps together a bundle whose middle scatters.
          const size_t P = num_points_of (a);
          float d = 0.0f;
          for (size_t i = 0; i != P; ++i)
            d += (a.segment<3> (3*i) - b.segment<3> (3*i)).norm();
          return d + length_weight * std::abs (a[3*P] - b[3*P]);
        }



        float cluster_distance (const ClusterFeature& a, const ClusterFeature& b,
                                float length_weight)
        {
          // Per pair, not against one global reference: which end of a streamline
          // comes first in the file is arbitrary, and in a set spanning several
          // bundles no single reference orientation is meaningful for all of them.
          return std::min (cluster_distance_oriented (a, b, length_weight),
                           cluster_distance_oriented (cluster_flip (a), b, length_weight));
        }



        namespace
        {

          //! \a f oriented to match \a centre, so means accumulate consistently.
          inline ClusterFeature oriented_to (const ClusterFeature& f, const ClusterFeature& centre, float w)
          {
            const ClusterFeature flipped = cluster_flip (f);
            return cluster_distance_oriented (flipped, centre, w) < cluster_distance_oriented (f, centre, w)
                 ? flipped : f;
          }

          struct KMeansFit { NOMEMALIGN
            vector<ClusterFeature> centres;
            vector<size_t> label;
            float total_distance = std::numeric_limits<float>::max();
            size_t iterations = 0;
            bool converged = false;
          };

          KMeansFit run_kmeans (const vector<ClusterFeature>& features, size_t K,
                                size_t max_iterations, float w, Math::RNG::Uniform<float>& rng)
          {
            KMeansFit fit;
            fit.label.assign (features.size(), 0);

            // k-means++ seeding, on the flip-invariant distance, so a run does not
            // depend on which streamlines happen to come first in the file.
            fit.centres.push_back (features[std::min (features.size() - 1,
                                   size_t (rng() * float (features.size())))]);
            vector<float> nearest (features.size(), std::numeric_limits<float>::max());
            while (fit.centres.size() < K) {
              float total = 0.0f;
              for (size_t i = 0; i != features.size(); ++i) {
                nearest[i] = std::min (nearest[i], cluster_distance (features[i], fit.centres.back(), w));
                total += nearest[i];
              }
              if (!(total > 0.0f))
                break;    // every remaining streamline coincides with a centre already
              const float target = rng() * total;
              float running = 0.0f;
              size_t pick = features.size() - 1;
              for (size_t i = 0; i != features.size(); ++i) {
                running += nearest[i];
                if (running >= target) { pick = i; break; }
              }
              fit.centres.push_back (features[pick]);
            }

            for (; fit.iterations != max_iterations; ) {
              // Assign, remembering the orientation that matched so the mean is
              // taken over consistently oriented features.
              bool changed = false;
              float total = 0.0f;
              vector<ClusterFeature> sums (fit.centres.size(), ClusterFeature::Zero (features[0].size()));
              vector<size_t> counts (fit.centres.size(), 0);
              for (size_t i = 0; i != features.size(); ++i) {
                size_t best = 0;
                float best_d = std::numeric_limits<float>::max();
                for (size_t k = 0; k != fit.centres.size(); ++k) {
                  const float d = cluster_distance (features[i], fit.centres[k], w);
                  if (d < best_d) { best_d = d; best = k; }
                }
                if (fit.label[i] != best) { fit.label[i] = best; changed = true; }
                total += best_d;
                sums[best] += oriented_to (features[i], fit.centres[best], w);
                ++counts[best];
              }
              fit.total_distance = total;
              ++fit.iterations;

              for (size_t k = 0; k != fit.centres.size(); ++k) {
                if (counts[k]) {
                  fit.centres[k] = sums[k] / float (counts[k]);
                } else {
                  // Re-seed an empty cluster onto the worst-fitting streamline rather
                  // than dropping it, so asking for K groups gives K where the data
                  // allows it.
                  size_t worst = 0;
                  float worst_d = -1.0f;
                  for (size_t i = 0; i != features.size(); ++i) {
                    const float d = cluster_distance (features[i], fit.centres[fit.label[i]], w);
                    if (d > worst_d) { worst_d = d; worst = i; }
                  }
                  fit.centres[k] = features[worst];
                  changed = true;
                }
              }
              if (!changed) { fit.converged = true; break; }
            }
            return fit;
          }

        }



        namespace
        {

          //! Average-linkage agglomerative clustering on a full pairwise distance.
          /*! Lance-Williams update, with a per-row minimum cached so each merge costs
           *  O(N) rather than a full O(N^2) rescan. */
          vector<size_t> agglomerative (vector<float>& d, size_t n, size_t k, size_t& merges)
          {
            auto at = [&d, n] (size_t i, size_t j) -> float& { return d[i*n + j]; };
            vector<uint8_t> active (n, 1);
            vector<size_t> size (n, 1);
            vector<size_t> parent (n);
            for (size_t i = 0; i != n; ++i)
              parent[i] = i;

            // Cached nearest active neighbour of each row.
            vector<size_t> nearest (n, 0);
            vector<float> nearest_d (n, std::numeric_limits<float>::max());
            auto rescan = [&] (size_t i) {
              nearest_d[i] = std::numeric_limits<float>::max();
              for (size_t j = 0; j != n; ++j) {
                if (j == i || !active[j]) continue;
                if (at(i,j) < nearest_d[i]) { nearest_d[i] = at(i,j); nearest[i] = j; }
              }
            };
            for (size_t i = 0; i != n; ++i)
              rescan (i);

            size_t remaining = n;
            merges = 0;
            while (remaining > k) {
              size_t a = n, b = n;
              float best = std::numeric_limits<float>::max();
              for (size_t i = 0; i != n; ++i) {
                if (!active[i] || !active[nearest[i]]) continue;
                if (nearest_d[i] < best) { best = nearest_d[i]; a = i; b = nearest[i]; }
              }
              if (a == n) {
                // Every cached neighbour was stale; rebuild and try once more.
                for (size_t i = 0; i != n; ++i)
                  if (active[i]) rescan (i);
                for (size_t i = 0; i != n; ++i) {
                  if (!active[i] || !active[nearest[i]]) continue;
                  if (nearest_d[i] < best) { best = nearest_d[i]; a = i; b = nearest[i]; }
                }
                if (a == n)
                  break;
              }
              if (a > b) std::swap (a, b);

              const float wa = float (size[a]), wb = float (size[b]);
              for (size_t j = 0; j != n; ++j) {
                if (!active[j] || j == a || j == b) continue;
                const float merged = (wa * at(a,j) + wb * at(b,j)) / (wa + wb);
                at(a,j) = merged;
                at(j,a) = merged;
              }
              size[a] += size[b];
              active[b] = 0;
              parent[b] = a;
              --remaining;
              ++merges;

              // a's row changed, and anything that pointed at a or b must look again.
              rescan (a);
              for (size_t i = 0; i != n; ++i)
                if (active[i] && i != a && (nearest[i] == a || nearest[i] == b))
                  rescan (i);
            }

            // Flatten the merge chain, then number the surviving clusters.
            vector<size_t> label (n, 0);
            std::map<size_t, size_t> index;
            for (size_t i = 0; i != n; ++i) {
              size_t root = i;
              while (parent[root] != root)
                root = parent[root];
              auto it = index.find (root);
              if (it == index.end())
                it = index.insert ({ root, index.size() }).first;
              label[i] = it->second;
            }
            return label;
          }

        }



        ClusterResult cluster_streamlines (const vector<Streamline<float>>& tracks,
                                           size_t num_clusters,
                                           const ClusterOptions& options)
        {
          const ClusterMethod method = options.method;
          const size_t max_iterations = options.max_iterations;
          const float w = options.length_weight;
          ClusterResult result;
          result.assignment.assign (tracks.size(), ClusterResult::invalid);
          if (tracks.empty() || !num_clusters)
            return result;

          vector<ClusterFeature> features;
          vector<size_t> source;            // index into tracks, for non-empty ones
          features.reserve (tracks.size());
          source.reserve (tracks.size());
          for (size_t i = 0; i != tracks.size(); ++i) {
            if (tracks[i].size() < 2)
              continue;
            features.push_back (cluster_features (tracks[i], options.num_points));
            source.push_back (i);
          }
          if (features.empty())
            return result;

          const size_t K = std::min (num_clusters, features.size());

          if (method == ClusterMethod::Hierarchical) {
            const size_t n = features.size();
            if (n > hierarchical_max_streamlines)
              throw Exception ("hierarchical clustering holds a " + str(n) + " x " + str(n)
                               + " distance matrix, which needs "
                               + str (size_t (double(n)*double(n)*4.0/1048576.0)) + " MB; "
                               "reduce the streamline count below "
                               + str (hierarchical_max_streamlines)
                               + " or use k-means or EM, which do not build one");

            // Whole-streamline MDF rather than the sampled-point distance: comparing
            // every pair is only worth its cost with the richer relation.
            vector<FixedTrack> fixed (n);
            for (size_t i = 0; i != n; ++i)
              to_fixed (tracks[source[i]], std::max<size_t> (2, options.mdf_num_points), fixed[i]);

            vector<float> d (n * n, 0.0f);
            for (size_t i = 0; i != n; ++i) {
              for (size_t j = i + 1; j != n; ++j) {
                const float v = mdf_flip (fixed[i], fixed[j]);
                d[i*n + j] = v;
                d[j*n + i] = v;
              }
              d[i*n + i] = std::numeric_limits<float>::max();
            }

            size_t merges = 0;
            const vector<size_t> label = agglomerative (d, n, K, merges);

            size_t num = 0;
            for (size_t i = 0; i != n; ++i)
              num = std::max (num, label[i] + 1);
            result.sizes.assign (num, 0);
            for (size_t i = 0; i != n; ++i) {
              result.assignment[source[i]] = label[i];
              ++result.sizes[label[i]];
            }
            // Mean distance to the nearest member of the cluster's own set, so the
            // number is comparable in spirit with the centroid methods'.
            float total = 0.0f;
            for (size_t i = 0; i != n; ++i) {
              float nearest = std::numeric_limits<float>::max();
              for (size_t j = 0; j != n; ++j)
                if (j != i && label[j] == label[i])
                  nearest = std::min (nearest, mdf_flip (fixed[i], fixed[j]));
              if (std::isfinite (nearest))
                total += nearest;
            }
            result.num_clusters = num;
            result.iterations = merges;
            result.converged = true;
            result.mean_distance = total / float (n);
            return result;
          }

          Math::RNG::Uniform<float> rng;
          KMeansFit best;
          for (size_t attempt = 0; attempt != std::max<size_t> (1, options.restarts); ++attempt) {
            KMeansFit fit = run_kmeans (features, K, max_iterations, w, rng);
            if (fit.total_distance < best.total_distance)
              best = std::move (fit);
          }

          vector<size_t> label = std::move (best.label);
          vector<ClusterFeature> centres = std::move (best.centres);
          size_t iteration = best.iterations;
          bool converged = best.converged;

          if (method == ClusterMethod::EM) {
            // Gaussian mixture with a full covariance per cluster, fitted by EM and
            // started from the k-means solution. Full rather than diagonal because a
            // bundle is an elongated, correlated cloud in this space - its endpoint
            // and midpoint coordinates move together - and a diagonal model cannot
            // represent that. Measured on seven neighbouring bundles of the built-in
            // atlas (AF_L, SLF1-3_L, CST_L, DRTT_L, ML_L in one file, k=7): k-means
            // 85%, diagonal EM 88%, this 89-90%. The ceiling for the feature set is
            // 99% (a supervised classifier with one full-covariance Gaussian per
            // bundle), so the remaining loss is the clustering, not the features -
            // almost all of it CST_L against ML_L, which run parallel a few mm apart
            // and are all but coincident at three sampled points.
            const size_t Kc = centres.size();
            const Eigen::Index D = features[0].size();
            vector<ClusterFeature> mean (centres);
            vector<Eigen::MatrixXf> covariance (Kc, Eigen::MatrixXf::Identity (D, D));
            vector<Eigen::LLT<Eigen::MatrixXf>> chol (Kc);
            vector<float> log_det (Kc, 0.0f);
            vector<float> weight (Kc, 1.0f / float (Kc));

            // Ridge on the covariance, in mm^2. Without it a cluster of a few nearly
            // identical streamlines gives a singular Gaussian and infinite
            // likelihood, and every other streamline then gets assigned to it.
            const Eigen::MatrixXf ridge = Eigen::MatrixXf::Identity (D, D);

            auto factorise = [&] (size_t k) {
              chol[k].compute (covariance[k]);
              const Eigen::MatrixXf L = chol[k].matrixL();
              log_det[k] = 0.0f;
              for (Eigen::Index d = 0; d != D; ++d)
                log_det[k] += 2.0f * std::log (std::max (L(d,d), 1e-6f));
            };

            // Features are oriented to the component they are being scored against;
            // a mixture has no notion of the flip, so it has to be resolved first.
            for (size_t k = 0; k != Kc; ++k) {
              ClusterFeature sum = ClusterFeature::Zero (D);
              size_t count = 0;
              for (size_t i = 0; i != features.size(); ++i) {
                if (label[i] != k) continue;
                sum += oriented_to (features[i], mean[k], w);
                ++count;
              }
              if (count) {
                mean[k] = sum / float (count);
                Eigen::MatrixXf scatter = Eigen::MatrixXf::Zero (D, D);
                for (size_t i = 0; i != features.size(); ++i) {
                  if (label[i] != k) continue;
                  const ClusterFeature d = oriented_to (features[i], mean[k], w) - mean[k];
                  scatter.noalias() += d * d.transpose();
                }
                covariance[k] = scatter / float (count) + ridge;
                weight[k] = float (count) / float (features.size());
              }
              factorise (k);
            }

            vector<vector<float>> resp (features.size(), vector<float> (Kc, 0.0f));
            converged = false;
            float previous_ll = -std::numeric_limits<float>::max();
            for (size_t em = 0; em != max_iterations; ++em) {
              float log_likelihood = 0.0f;
              vector<float> logp (Kc);
              for (size_t i = 0; i != features.size(); ++i) {
                float best_logp = -std::numeric_limits<float>::max();
                for (size_t k = 0; k != Kc; ++k) {
                  const ClusterFeature d = oriented_to (features[i], mean[k], w) - mean[k];
                  // Mahalanobis distance via the Cholesky factor, in log space: a
                  // ten-dimensional Gaussian underflows a float long before the
                  // responsibilities stop being meaningful.
                  const float q = d.dot (chol[k].solve (d));
                  logp[k] = std::log (std::max (weight[k], 1e-12f)) - 0.5f * (q + log_det[k]);
                  best_logp = std::max (best_logp, logp[k]);
                }
                float total = 0.0f;
                for (size_t k = 0; k != Kc; ++k) {
                  resp[i][k] = std::exp (logp[k] - best_logp);
                  total += resp[i][k];
                }
                for (size_t k = 0; k != Kc; ++k)
                  resp[i][k] /= total;
                log_likelihood += best_logp + std::log (total);

                size_t top = 0;
                for (size_t k = 1; k != Kc; ++k)
                  if (resp[i][k] > resp[i][top]) top = k;
                label[i] = top;
              }

              for (size_t k = 0; k != Kc; ++k) {
                float mass = 0.0f;
                ClusterFeature sum = ClusterFeature::Zero (D);
                for (size_t i = 0; i != features.size(); ++i) {
                  const float r = resp[i][k];
                  if (r < 1e-6f) continue;
                  mass += r;
                  sum += r * oriented_to (features[i], mean[k], w);
                }
                if (mass < 1e-6f)
                  continue;
                const ClusterFeature new_mean = sum / mass;
                Eigen::MatrixXf scatter = Eigen::MatrixXf::Zero (D, D);
                for (size_t i = 0; i != features.size(); ++i) {
                  const float r = resp[i][k];
                  if (r < 1e-6f) continue;
                  const ClusterFeature d = oriented_to (features[i], mean[k], w) - new_mean;
                  scatter.noalias() += r * d * d.transpose();
                }
                mean[k] = new_mean;
                covariance[k] = scatter / mass + ridge;
                weight[k] = mass / float (features.size());
                factorise (k);
              }
              ++iteration;

              // Converged when the likelihood stops improving materially. Comparing
              // the labels against the k-means ones instead would be wrong: EM is
              // meant to move points, so that test could never pass.
              if (std::abs (log_likelihood - previous_ll)
                  <= 1e-5f * std::max (1.0f, std::abs (log_likelihood))) {
                converged = true;
                break;
              }
              previous_ll = log_likelihood;
            }
            centres = mean;
          }

          result.sizes.assign (centres.size(), 0);
          float total_distance = 0.0f;
          for (size_t i = 0; i != features.size(); ++i) {
            result.assignment[source[i]] = label[i];
            ++result.sizes[label[i]];
            total_distance += cluster_distance (features[i], centres[label[i]], w);
          }
          result.num_clusters = centres.size();
          result.iterations = iteration;
          result.converged = converged;
          result.mean_distance = total_distance / float (features.size());
          return result;
        }

      }
    }
  }
}
