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

#include "dwi/tractography/recognition/refine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include "math/math.h"

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
          constexpr float infinite = std::numeric_limits<float>::infinity();

          //! \a fraction of the way through an already sorted vector.
          float percentile_of_sorted (const vector<float>& sorted, float fraction)
          {
            if (sorted.empty())
              return infinite;
            const size_t index = std::min (sorted.size() - 1,
                                           size_t (fraction * float (sorted.size() - 1) + 0.5f));
            return sorted[index];
          }
        }



        const char* refine_stage_name (RefineStage stage)
        {
          switch (stage) {
            case RefineStage::Length:     return "length";
            case RefineStage::Endpoints:  return "endpoints";
            case RefineStage::Distance:   return "shape distance";
            case RefineStage::Population: return "rare in the population";
            case RefineStage::Competitor: return "claimed by a neighbour";
            case RefineStage::Territory:  return "on a neighbour's ground";
            case RefineStage::Outlier:    return "outlier";
            default:                      return "kept";
          }
        }



        std::string RefineReport::summary () const
        {
          std::string out = str (kept) + " of " + str (input) + " kept";
          const RefineStage order[] = { RefineStage::Length, RefineStage::Endpoints,
                                        RefineStage::Population, RefineStage::Distance,
                                        RefineStage::Competitor, RefineStage::Territory,
                                        RefineStage::Outlier };
          for (const RefineStage stage : order) {
            const size_t n = removed_by (stage);
            if (n)
              out += "; " + str (n) + " on " + refine_stage_name (stage);
          }
          // Which neighbour took them is the interesting part when it is CST losing
          // streamlines to ML, so name the busiest few rather than only counting.
          if (claimed_by.size()) {
            vector<std::pair<std::string, size_t>> by_count (claimed_by.begin(), claimed_by.end());
            std::sort (by_count.begin(), by_count.end(),
                       [] (const std::pair<std::string, size_t>& a,
                           const std::pair<std::string, size_t>& b) { return a.second > b.second; });
            out += " (";
            for (size_t i = 0; i != by_count.size() && i != 3; ++i)
              out += (i ? ", " : "") + by_count[i].first + " " + str (by_count[i].second);
            out += ")";
          }
          return out;
        }



        void endpoint_clouds (const vector<Streamline<float>>& tracks,
                              vector<Eigen::Vector3f>& cloud_a,
                              vector<Eigen::Vector3f>& cloud_b)
        {
          cloud_a.clear();
          cloud_b.clear();
          if (tracks.empty())
            return;

          // Orient against the longest streamline: streamline direction is
          // arbitrary, so without this each "cloud" would hold a mix of both ends.
          size_t reference = 0, longest = 0;
          for (size_t i = 0; i != tracks.size(); ++i) {
            if (tracks[i].size() > longest) {
              longest = tracks[i].size();
              reference = i;
            }
          }
          if (tracks[reference].size() < 2)
            return;
          const Eigen::Vector3f ref_front = tracks[reference].front();
          const Eigen::Vector3f ref_back  = tracks[reference].back();

          for (const auto& tck : tracks) {
            if (tck.size() < 2)
              continue;
            const float direct  = (tck.front()-ref_front).norm() + (tck.back()-ref_back).norm();
            const float flipped = (tck.front()-ref_back).norm()  + (tck.back()-ref_front).norm();
            if (direct <= flipped) {
              cloud_a.push_back (tck.front());
              cloud_b.push_back (tck.back());
            } else {
              cloud_a.push_back (tck.back());
              cloud_b.push_back (tck.front());
            }
          }
        }



        void bundle_length_window (const vector<Streamline<float>>& bundle,
                                   float& min_length, float& max_length,
                                   float low, float high)
        {
          vector<float> lengths;
          lengths.reserve (bundle.size());
          for (const auto& tck : bundle)
            if (tck.size() >= 2)
              lengths.push_back (track_length (tck));
          if (lengths.empty()) {
            min_length = 0.0f;
            max_length = infinite;
            return;
          }
          std::sort (lengths.begin(), lengths.end());
          min_length = percentile_of_sorted (lengths, low);
          max_length = percentile_of_sorted (lengths, high);
        }



        EndpointGate::EndpointGate (const vector<Streamline<float>>& bundle, float radius, bool paired) :
            radius_ (radius),
            paired_ (paired),
            usable_ (false),
            // Cells must be at least as big as the radius queried: nearest_distance
            // only looks at the 27 cells around the query point.
            cloud_a_ (std::max (4.0f, radius)),
            cloud_b_ (std::max (4.0f, radius))
        {
          if (radius <= 0.0f)
            return;
          vector<Eigen::Vector3f> a, b;
          endpoint_clouds (bundle, a, b);
          if (a.empty() || b.empty())
            return;
          for (const auto& p : a) cloud_a_.insert (p);
          for (const auto& p : b) cloud_b_.insert (p);
          // Unpaired asks only "near some terminus", so each grid must hold both
          // clouds; that is BundleMatcher's own pre-filter, reproduced here so
          // every rejection is attributed to a stage rather than showing up as an
          // infinite shape distance.
          if (!paired_) {
            for (const auto& p : b) cloud_a_.insert (p);
            for (const auto& p : a) cloud_b_.insert (p);
          }
          usable_ = true;
        }



        bool EndpointGate::accepts (const Streamline<float>& tck) const
        {
          if (!usable_)
            return true;
          if (tck.size() < 2)
            return false;
          const Eigen::Vector3f& front = tck.front();
          const Eigen::Vector3f& back = tck.back();
          if (!paired_)
            return cloud_a_.any_within (front, radius_) && cloud_b_.any_within (back, radius_);

          // Reject only what the pairing exists to catch: a streamline that leaves
          // and returns to the *same* end of the bundle. Anything else passes,
          // including a streamline that simply stops short of a terminus.
          //
          // Demanding one end at each terminus instead was the obvious reading, and
          // it was wrong: a tracked candidate covers as much of the bundle as the FOD
          // supports, so requiring it to reach both ends rejects correct streamlines
          // wholesale - it took every truncated DTT_LR streamline the length window
          // had not already taken. Gating on length instead only moved the problem:
          // it put a cliff at the threshold, where 96 mm passed and 97 mm did not.
          const bool front_a = cloud_a_.any_within (front, radius_);
          const bool front_b = cloud_b_.any_within (front, radius_);
          const bool back_a  = cloud_a_.any_within (back, radius_);
          const bool back_b  = cloud_b_.any_within (back, radius_);
          const bool doubled_back = (front_a && back_a && !front_b && !back_b)
                                 || (front_b && back_b && !front_a && !back_a);
          return !doubled_back;
        }



        CompetitorSet::CompetitorSet (const vector<Competitor>& competitors,
                                      const BundleMatcher::Params& params)
        {
          for (const auto& competitor : competitors) {
            if (competitor.bundle.empty())
              continue;
            try {
              // The same params as the target, or the two distances are measuring
              // different things and comparing them is meaningless. max_mdf is
              // deliberately left as the target's rather than raised to infinity:
              // a competitor is never thresholded, but that field also sizes the
              // matcher's spatial grid, and an infinite cell puts every bundle
              // vertex in one bucket and turns each query into a brute-force scan
              // (measured: 2.6 s against 0.35 s for four competitors). Distances
              // past the grid's reach come back capped at 3x max_mdf instead of
              // infinite, which cannot win a comparison against a candidate that
              // already passed the threshold - so the cap costs nothing here.
              matchers_.push_back (std::unique_ptr<BundleMatcher> (new BundleMatcher (competitor.bundle, params)));
              names_.push_back (competitor.name);
            } catch (Exception&) {
              // A bundle too small or degenerate to build a matcher from simply
              // does not compete; it must not stop the ones that can.
            }
          }
        }



        float CompetitorSet::nearest (const Streamline<float>& tck, std::string& name) const
        {
          name.clear();
          float best = infinite;
          for (size_t i = 0; i != matchers_.size(); ++i) {
            const float d = matchers_[i]->distance (tck);
            if (d < best) {
              best = d;
              name = names_[i];
            }
          }
          return best;
        }



        float CompetitorSet::penalty (const Streamline<float>& tck, float limit, std::string& name) const
        {
          name.clear();
          if (!(limit > 0.0f))
            return 0.0f;
          // The bound tightens as competitors are measured, so each one only has to
          // be asked whether it beats the best so far - and only one that does is
          // measured exactly. A competitor outside the limit costs a few empty cell
          // lookups.
          float best = limit;
          for (size_t i = 0; i != matchers_.size(); ++i) {
            if (!matchers_[i]->closer_than (tck, best))
              continue;
            const float d = matchers_[i]->distance (tck);
            if (d < best) {
              best = d;
              name = names_[i];
            }
          }
          return name.empty() ? 0.0f : limit - best;
        }



        bool CompetitorSet::claims (const Streamline<float>& tck, float limit, std::string& name) const
        {
          name.clear();
          if (!(limit > 0.0f))
            return false;
          for (size_t i = 0; i != matchers_.size(); ++i) {
            if (matchers_[i]->closer_than (tck, limit)) {
              name = names_[i];
              return true;
            }
          }
          return false;
        }



        PopulationMap::PopulationMap (const std::string& path, const transform_type& to_map) :
            path_ (path),
            to_map_ (to_map),
            image_ (Image<float>::open (path)),
            interp_ (image_) { }



        float PopulationMap::at (const Eigen::Vector3f& p)
        {
          const Eigen::Vector3d q = to_map_ * p.cast<double>();
          // Outside the map is not a missing answer: the map covers the whole brain,
          // so a point beyond it is a point no subject's tract reached.
          if (!interp_.scanner (q))
            return 0.0f;
          const float value = interp_.value();
          return std::isfinite (value) ? value : 0.0f;
        }



        float population_outside (const Streamline<float>& tck,
                                  PopulationMap& map,
                                  float min_probability,
                                  float trim)
        {
          if (tck.size() < 3)
            return 0.0f;
          const size_t drop = size_t (std::max (0.0f, trim) * float (tck.size()));
          if (tck.size() <= 2 * drop + 1)
            return 0.0f;

          size_t outside = 0, total = 0;
          for (size_t i = drop; i + drop < tck.size(); ++i) {
            ++total;
            if (map.at (tck[i]) < min_probability)
              ++outside;
          }
          return total ? float (outside) / float (total) : 0.0f;
        }



        size_t CompetitorSet::drop_self_claimants (const vector<Streamline<float>>& atlas,
                                                   const BundleMatcher::Params& params,
                                                   float competitive_margin,
                                                   float max_share,
                                                   vector<std::string>& dropped)
        {
          if (matchers_.empty() || !(max_share < 1.0f) || atlas.size() < 16)
            return 0;

          // Held out, not the bundle itself: a streamline that helped build the
          // matcher sits at zero distance from it, and nothing can be a fraction of
          // zero closer, so competing against the bundle's own members would find
          // nothing whatever the competitor was. Interleaved for the same reason
          // suggest_bundle_thresholds() interleaves - a track file is often written
          // grouped by seed region, and a contiguous split would compare one end of
          // the bundle against the other.
          vector<Streamline<float>> reference, probe;
          // 120 sampled, so about 60 held out. The signal being looked for is not
          // subtle - a part takes every one of them and the closest genuine rival
          // takes under 1% - so a few dozen settle it, and this runs once per bundle
          // against every competitor. At 400 sampled it cost 7.7 s of a 23.3 s run
          // against the whole projection category; at 120, 2.6 s of 17.8 s, and every
          // bundle measured comes out the same either way.
          const size_t stride = std::max<size_t> (1, atlas.size() / 120);
          for (size_t i = 0; i < atlas.size(); i += stride) {
            if ((i / stride) % 2)
              probe.push_back (atlas[i]);
            else
              reference.push_back (atlas[i]);
          }
          if (reference.size() < 8 || probe.size() < 8)
            return 0;

          std::unique_ptr<BundleMatcher> half;
          try {
            half.reset (new BundleMatcher (reference, params));
          } catch (Exception&) {
            return 0;
          }

          vector<size_t> taken (matchers_.size(), 0);
          size_t measured = 0;
          for (const auto& tck : probe) {
            const float own = half->distance (tck);
            if (!std::isfinite (own))
              continue;   // the half-bundle rejects it outright; it decides nothing
            ++measured;
            const float limit = competitive_margin * own;
            if (!(limit > 0.0f))
              continue;
            // Only whether each competitor is inside the limit, not by how much:
            // this is 60 probes against every competitor, once per bundle.
            for (size_t k = 0; k != matchers_.size(); ++k)
              if (matchers_[k]->closer_than (tck, limit))
                ++taken[k];
          }
          if (!measured)
            return 0;

          vector<std::string> keep_names;
          vector<std::unique_ptr<BundleMatcher>> keep_matchers;
          size_t removed = 0;
          for (size_t k = 0; k != matchers_.size(); ++k) {
            if (float (taken[k]) / float (measured) > max_share) {
              dropped.push_back (names_[k]);
              ++removed;
              continue;
            }
            keep_names.push_back (names_[k]);
            keep_matchers.push_back (std::move (matchers_[k]));
          }
          names_ = std::move (keep_names);
          matchers_ = std::move (keep_matchers);
          return removed;
        }



        float CompetitorSet::intrusion (const Streamline<float>& tck,
                                        const BundleMatcher& target,
                                        float margin,
                                        size_t samples,
                                        std::string& name) const
        {
          name.clear();
          if (matchers_.empty() || tck.size() < 2)
            return 0.0f;

          const size_t stride = std::max<size_t> (1, tck.size() / std::max<size_t> (1, samples));
          vector<size_t> intruding (matchers_.size(), 0);
          size_t sampled = 0;
          for (size_t i = 0; i < tck.size(); i += stride) {
            // Each grid answers only out to its own reach, so "too far to measure"
            // comes back infinite. That is not a missing answer here, it is the
            // answer: a sample the target cannot reach but a competitor can is as
            // far onto that competitor's ground as a sample can get. Only a sample
            // no bundle can reach says nothing, and those are left out of the
            // denominator rather than counted as innocent.
            const float own = target.vertex_distance (tck[i]);
            // A sample sitting on the target cannot be a margin nearer anything
            // else - distances are non-negative, so other + margin < own is
            // impossible once own <= margin. Exact, not an approximation, and it is
            // most of the work: nearly every sample of a genuine streamline is
            // within a couple of millimetres of its own bundle, so this settles them
            // with one query instead of one per competitor. Measured on CST_L
            // against the whole projection category, 400 candidates: 65 s to 20 s.
            if (std::isfinite (own) && own <= margin) {
              ++sampled;
              continue;
            }
            bool measurable = std::isfinite (own);
            for (size_t k = 0; k != matchers_.size(); ++k) {
              const float other = matchers_[k]->vertex_distance (tck[i]);
              if (!std::isfinite (other))
                continue;
              measurable = true;
              // Strictly nearer by a margin, so ground the two bundles genuinely
              // share counts for neither.
              if (!std::isfinite (own) || other + margin < own)
                ++intruding[k];
            }
            if (measurable)
              ++sampled;
          }
          if (!sampled)
            return 0.0f;

          float worst = 0.0f;
          for (size_t k = 0; k != matchers_.size(); ++k) {
            const float share = float (intruding[k]) / float (sampled);
            if (share > worst) {
              worst = share;
              name = names_[k];
            }
          }
          return worst;
        }



        namespace
        {
          //! Points sampled along each streamline, pooled into one cloud.
          vector<Eigen::Vector3f> sample_cloud (const vector<Streamline<float>>& tracks,
                                                size_t per_track, size_t max_tracks)
          {
            vector<Eigen::Vector3f> out;
            if (tracks.empty())
              return out;
            const size_t stride = std::max<size_t> (1, tracks.size() / std::max<size_t> (1, max_tracks));
            FixedTrack fixed;
            for (size_t i = 0; i < tracks.size(); i += stride) {
              to_fixed (tracks[i], per_track, fixed);
              for (Eigen::Index r = 0; r != fixed.rows(); ++r)
                out.push_back (fixed.row(r).transpose());
            }
            return out;
          }
        }



        bool fit_bundle_to_candidates (const vector<Streamline<float>>& bundle,
                                       const vector<Streamline<float>>& candidates,
                                       const RefineOptions& options,
                                       Eigen::Matrix4f& transform,
                                       float& translation, float& rotation, float& scale)
        {
          translation = 0.0f;
          rotation = 0.0f;
          scale = 1.0f;

          // Bounded samples on both sides: this is fitting a residual offset, and a
          // few thousand points describe the shape of a bundle perfectly well.
          vector<Eigen::Vector3f> source = sample_cloud (bundle, 20, 150);
          const vector<Eigen::Vector3f> target = sample_cloud (candidates, 20, 300);
          if (source.size() < 20 || target.size() < 20)
            return false;

          // Nearest-neighbour queries into the candidate cloud. The cell has to be at
          // least as large as the distances involved, since only the 27 surrounding
          // cells are searched.
          VoxelHashGrid grid (16.0f);
          grid.reserve (target.size());
          for (const auto& p : target)
            grid.insert (p);

          Eigen::Matrix4f total = Eigen::Matrix4f::Identity();
          // A handful of rounds: this is a small residual, not a general registration,
          // and each round is one pass over the sampled points.
          for (size_t iteration = 0; iteration != 8; ++iteration) {
            // Match each bundle point to the nearest candidate point.
            vector<Eigen::Vector3f> matched_source, matched_target;
            matched_source.reserve (source.size());
            matched_target.reserve (source.size());
            for (const auto& p : source) {
              Eigen::Vector3f nearest;
              if (!grid.nearest_point (p, nearest))
                continue;
              matched_source.push_back (p);
              matched_target.push_back (nearest);
            }
            if (matched_source.size() < 20)
              return false;

            // Umeyama: the rigid transform plus uniform scale minimising the squared
            // distance between the two matched sets.
            Eigen::Vector3f source_mean = Eigen::Vector3f::Zero();
            Eigen::Vector3f target_mean = Eigen::Vector3f::Zero();
            for (size_t i = 0; i != matched_source.size(); ++i) {
              source_mean += matched_source[i];
              target_mean += matched_target[i];
            }
            source_mean /= float (matched_source.size());
            target_mean /= float (matched_target.size());

            Eigen::Matrix3f covariance = Eigen::Matrix3f::Zero();
            float source_variance = 0.0f;
            for (size_t i = 0; i != matched_source.size(); ++i) {
              const Eigen::Vector3f a = matched_source[i] - source_mean;
              const Eigen::Vector3f b = matched_target[i] - target_mean;
              covariance += b * a.transpose();
              source_variance += a.squaredNorm();
            }
            if (!(source_variance > 0.0f))
              return false;
            covariance /= float (matched_source.size());
            source_variance /= float (matched_source.size());

            Eigen::JacobiSVD<Eigen::Matrix3f> svd (covariance, Eigen::ComputeFullU | Eigen::ComputeFullV);
            Eigen::Matrix3f correction = Eigen::Matrix3f::Identity();
            // Guard against the SVD returning a reflection: a mirrored bundle is not
            // a plausible registration residual.
            if ((svd.matrixU() * svd.matrixV().transpose()).determinant() < 0.0f)
              correction (2, 2) = -1.0f;
            const Eigen::Matrix3f rotation_matrix = svd.matrixU() * correction * svd.matrixV().transpose();
            const float step_scale = (svd.singularValues().cwiseProduct (correction.diagonal())).sum()
                                   / source_variance;
            if (!std::isfinite (step_scale) || step_scale <= 0.0f)
              return false;
            const Eigen::Vector3f step_translation = target_mean - step_scale * rotation_matrix * source_mean;

            Eigen::Matrix4f step = Eigen::Matrix4f::Identity();
            step.topLeftCorner<3,3>() = step_scale * rotation_matrix;
            step.topRightCorner<3,1>() = step_translation;
            total = step * total;
            for (auto& p : source)
              p = (step_scale * rotation_matrix * p + step_translation).eval();
          }

          // What the accumulated fit amounts to, so it can be judged and reported.
          const Eigen::Matrix3f linear = total.topLeftCorner<3,3>();
          scale = std::cbrt (std::abs (linear.determinant()));
          translation = total.topRightCorner<3,1>().norm();
          if (!(scale > 0.0f) || !std::isfinite (scale))
            return false;
          const Eigen::Matrix3f pure_rotation = linear / scale;
          // Rotation angle from the trace: trace(R) = 1 + 2cos(theta).
          const float cosine = std::max (-1.0f, std::min (1.0f, 0.5f * (pure_rotation.trace() - 1.0f)));
          rotation = std::acos (cosine) * 180.0f / float (Math::pi);

          // A fit is meant to absorb residual registration error. Anything larger is
          // the bundle being dragged onto whatever dominates the candidates, which
          // would make the shape match agree with almost anything.
          if (translation > options.max_fit_translation
              || rotation > options.max_fit_rotation
              || scale > options.max_fit_scale
              || scale < 1.0f / options.max_fit_scale)
            return false;

          transform = total;
          return true;
        }



        vector<Streamline<float>> transform_streamlines (const vector<Streamline<float>>& tracks,
                                                        const Eigen::Matrix4f& transform)
        {
          const Eigen::Matrix3f linear = transform.topLeftCorner<3,3>();
          const Eigen::Vector3f offset = transform.topRightCorner<3,1>();
          vector<Streamline<float>> out (tracks);
          for (auto& tck : out)
            for (auto& vertex : tck)
              vertex = (linear * vertex + offset).eval();
          return out;
        }



        vector<float> per_node_core_distance (const vector<Streamline<float>>& tracks,
                                              size_t num_points)
        {
          vector<float> out (tracks.size(), 0.0f);
          if (tracks.size() < 10)
            return out;

          // Resample everything to a common number of nodes, so node i of one
          // streamline is comparable with node i of another.
          vector<FixedTrack> fixed (tracks.size());
          vector<bool> usable (tracks.size(), false);
          const FixedTrack* reference = nullptr;
          for (size_t i = 0; i != tracks.size(); ++i) {
            to_fixed (tracks[i], num_points, fixed[i]);
            usable[i] = fixed[i].rows() == Eigen::Index (num_points);
            if (usable[i] && !reference)
              reference = &fixed[i];
          }
          if (!reference)
            return out;

          // Orient consistently: a streamline stored end-to-start would otherwise be
          // compared head-to-tail against the core and look like an outlier.
          for (size_t i = 0; i != tracks.size(); ++i) {
            if (usable[i] && is_flipped (*reference, fixed[i]))
              fixed[i] = fixed[i].colwise().reverse().eval();
          }

          // Mean and spread at each node across the bundle, then score each
          // streamline by its worst node. A whole-streamline distance averages away
          // exactly the case this is for: a streamline that follows the bundle and
          // then peels off at one end.
          for (size_t node = 0; node != num_points; ++node) {
            Eigen::Vector3f mean = Eigen::Vector3f::Zero();
            size_t count = 0;
            for (size_t i = 0; i != tracks.size(); ++i) {
              if (!usable[i])
                continue;
              mean += fixed[i].row (node).transpose();
              ++count;
            }
            if (count < 10)
              continue;
            mean /= float (count);

            // Spread as a single scalar rather than a full covariance: with 3 degrees
            // of freedom and often only tens of streamlines, an inverted 3x3
            // covariance is dominated by sampling noise, and the isotropic form is
            // what makes this robust on small bundles.
            double sum_squares = 0.0;
            for (size_t i = 0; i != tracks.size(); ++i) {
              if (!usable[i])
                continue;
              sum_squares += (fixed[i].row (node).transpose() - mean).squaredNorm();
            }
            const float sigma = std::sqrt (float (sum_squares / double (count)));
            // A floor, so a node where the bundle happens to be very tight cannot
            // make every streamline an outlier.
            const float scale = std::max (1.0f, sigma);
            for (size_t i = 0; i != tracks.size(); ++i) {
              if (!usable[i])
                continue;
              const float deviation = (fixed[i].row (node).transpose() - mean).norm() / scale;
              out[i] = std::max (out[i], deviation);
            }
          }
          return out;
        }



        //! The competitor set for a run, with self-claimants already dropped.
        /*! Split out because it is now needed before the shape threshold rather than
         *  after it: competitive scoring folds the neighbours into the same ranking
         *  strictness cuts, so they have to be measured first. */
        static std::unique_ptr<CompetitorSet> build_competitors (
            const vector<Streamline<float>>& atlas,
            const vector<Competitor>& competitors,
            const BundleMatcher::Params& matcher_params,
            const RefineOptions& options,
            RefineReport&)
        {
          std::unique_ptr<CompetitorSet> others;
          if (!(options.competitive || options.exclusive_territory) || competitors.empty())
            return others;
          others.reset (new CompetitorSet (competitors, matcher_params));
          vector<std::string> dropped;
          if (others->drop_self_claimants (atlas, matcher_params,
                                           options.competitive_margin,
                                           options.competitor_self_claim_limit, dropped)) {
            std::string list;
            for (const std::string& name : dropped)
              list += (list.size() ? ", " : "") + name;
            INFO ("not competing against " + list + ": each claims this bundle's own streamlines");
          }
          if (others->empty())
            others.reset();
          return others;
        }



        void refine_bundle (const vector<Streamline<float>>& atlas,
                            const BundleMatcher::Params& match,
                            const RefineOptions& options,
                            const vector<Competitor>& competitors,
                            const vector<Streamline<float>>& candidates,
                            vector<Streamline<float>>& kept,
                            vector<Streamline<float>>& rejected,
                            RefineReport& report,
                            vector<RefineStage>* stages,
                            PopulationMap* population)
        {
          kept.clear();
          rejected.clear();
          report = RefineReport();
          report.input = candidates.size();
          if (stages)
            stages->assign (candidates.size(), RefineStage::Kept);

          auto reject = [&] (size_t i, RefineStage stage) {
            ++report.removed[stage];
            if (stages)
              (*stages)[i] = stage;
          };

          if (candidates.empty() || atlas.empty())
            return;

          // --- 1. length ---
          // Applied here rather than left to BundleMatcher so that a length
          // rejection is reported as one, instead of arriving as an infinite
          // distance indistinguishable from a shape mismatch.
          float min_length = 0.0f, max_length = infinite;
          if (options.length_window) {
            bundle_length_window (atlas, min_length, max_length);
            min_length *= options.length_low_ratio;
            max_length *= options.length_high_ratio;
          } else {
            // The ratios BundleMatcher would have applied, against the bundle's
            // extremes rather than its percentiles.
            float shortest = infinite, longest = 0.0f;
            for (const auto& tck : atlas) {
              if (tck.size() < 2)
                continue;
              const float length = track_length (tck);
              shortest = std::min (shortest, length);
              longest = std::max (longest, length);
            }
            if (std::isfinite (shortest)) {
              min_length = match.min_length_ratio * shortest;
              max_length = match.max_length_ratio * longest;
            }
          }
          report.min_length = min_length;
          report.max_length = max_length;

          // --- 2. endpoints ---
          const float endpoint_radius = options.endpoint_gate ? options.endpoint_radius
                                                              : match.max_endpoint_dist;
          const EndpointGate gate (atlas, endpoint_radius, options.endpoint_gate);

          // Survivors of the two cheap gates, by index into candidates.
          vector<size_t> surviving;
          surviving.reserve (candidates.size());
          for (size_t i = 0; i != candidates.size(); ++i) {
            const auto& tck = candidates[i];
            if (tck.size() < 2) {
              reject (i, RefineStage::Length);
              continue;
            }
            const float length = track_length (tck);
            if (length < min_length || length > max_length) {
              reject (i, RefineStage::Length);
              continue;
            }
            if (!gate.accepts (tck)) {
              reject (i, RefineStage::Endpoints);
              continue;
            }
            surviving.push_back (i);
          }

          // --- 2b. population probability ---
          // Third of the cheap gates and by far the most decisive: one interpolated
          // sample per vertex, against a map that says how much of the population has
          // this tract here. It asks nothing about shape, so nothing about the
          // algorithm that drew the atlas, and it separates bundles that a distance
          // to the atlas streamlines cannot - measured against CST_L's own map, ML_L,
          // DRTT_L, DTT_LR and AF_L are all 0 of 400 with no competitors involved,
          // while CST_L's own held-out half keeps 381 of 400.
          if (population && options.population_max_outside < 1.0f) {
            vector<size_t> passed;
            passed.reserve (surviving.size());
            for (const size_t i : surviving) {
              const float share = population_outside (candidates[i], *population,
                                                      options.population_min_probability,
                                                      options.population_trim);
              if (share > options.population_max_outside)
                reject (i, RefineStage::Population);
              else
                passed.push_back (i);
            }
            surviving = std::move (passed);
          }

          // --- 2.5 local fit ---
          // Everything downstream measures against the bundle, so the bundle is
          // fitted onto what survived the cheap gates first. Doing it here rather
          // than earlier means the fit is solved against plausible candidates only,
          // not against whatever else was tracked in the territory.
          const vector<Streamline<float>>* reference = &atlas;
          vector<Streamline<float>> fitted;
          if (options.local_registration && surviving.size() >= 20) {
            vector<Streamline<float>> for_fit;
            for_fit.reserve (surviving.size());
            for (const size_t i : surviving)
              for_fit.push_back (candidates[i]);
            Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
            float shift = 0.0f, angle = 0.0f, factor = 1.0f;
            if (fit_bundle_to_candidates (atlas, for_fit, options, transform, shift, angle, factor)) {
              fitted = transform_streamlines (atlas, transform);
              reference = &fitted;
              report.fit_applied = true;
              report.fit_translation = shift;
              report.fit_rotation = angle;
              report.fit_scale = factor;
            }
          }

          // --- 3. shape distance ---
          // The gates above have already done what BundleMatcher's pre-filters
          // would, so they are switched off here; leaving both in would double the
          // work and hide which one rejected what.
          BundleMatcher::Params matcher_params = match;
          matcher_params.min_length_ratio = 0.0f;
          matcher_params.max_length_ratio = infinite;
          matcher_params.max_endpoint_dist = 0.0f;

          // A threshold of zero means "do not filter on shape": the bundle still
          // supplies the territory and the endpoint regions, but everything tracked
          // inside it is kept. Useful for seeing what the constraints alone produce.
          const bool filter_on_shape = match.max_mdf > 0.0f || std::isfinite (options.keep_fraction);
          vector<float> distances (candidates.size(), 0.0f);
          // What the threshold is actually applied to: the distance, plus whatever a
          // neighbouring bundle's better fit adds to it. Identical to `distances`
          // unless competitive scoring is on and something is contested.
          vector<float> scores (candidates.size(), 0.0f);
          vector<std::string> penalised_by (candidates.size());
          std::unique_ptr<BundleMatcher> matcher;
          std::unique_ptr<CompetitorSet> others;
          if (filter_on_shape) {
            matcher.reset (new BundleMatcher (*reference, matcher_params));
            for (const size_t i : surviving)
              distances[i] = scores[i] = matcher->distance (candidates[i]);

            // --- 3b. competition, as a penalty on the score ---
            // Built before the threshold rather than after it, because with scoring
            // the two are one decision: a contested streamline is not rejected for
            // being contested, it is moved up the ranking that strictness cuts.
            others = build_competitors (atlas, competitors, matcher_params, options, report);
            if (options.competitive && options.competitive_scoring && others) {
              for (const size_t i : surviving) {
                if (!std::isfinite (distances[i]))
                  continue;
                std::string claimant;
                const float extra = others->penalty (candidates[i],
                                                     options.competitive_margin * distances[i],
                                                     claimant);
                if (extra > 0.0f && claimant.size()) {
                  scores[i] = distances[i] + options.competitive_weight * extra;
                  penalised_by[i] = claimant;
                }
              }
            }

            vector<float> finite;
            finite.reserve (surviving.size());
            for (const size_t i : surviving)
              if (std::isfinite (distances[i]))
                finite.push_back (distances[i]);
            std::sort (finite.begin(), finite.end());

            float effective = match.max_mdf;
            if (std::isfinite (options.keep_fraction)) {
              // A strictness expressed as "keep the closest this fraction" behaves
              // the same on every bundle, where a millimetre threshold does not: the
              // distance that means "strict" for a densely sampled bundle rejects
              // every candidate of a sparse one. Below a handful of candidates the
              // percentile is noise and the millimetre threshold stands.
              // Ranked on the score, so strictness and competition are one ordering:
              // raise strictness and the streamlines a neighbour fits better are the
              // first to go, before equally distant streamlines nobody contests.
              vector<float> ranked;
              ranked.reserve (surviving.size());
              for (const size_t i : surviving)
                if (std::isfinite (scores[i]))
                  ranked.push_back (scores[i]);
              std::sort (ranked.begin(), ranked.end());
              effective = ranked.size() >= 10
                        ? percentile_of_sorted (ranked, std::min (1.0f, std::max (0.0f, options.keep_fraction)))
                        : match.max_mdf;
            } else if (finite.size() >= 20 && options.adaptive_loosening && !report.fit_applied) {
              // Skipped when the bundle was fitted: this rule exists to absorb a
              // registration offset by loosening, and the fit has just corrected
              // that offset instead. Doing both would loosen for an error that is
              // no longer there, and a looser threshold is what lets a neighbouring
              // bundle in.
              // Where the atlas bundle sits is only as good as the registration that
              // put it there, and the residual offset varies from bundle to bundle:
              // on the test FOD the nearest OR_L candidate was 5.6 mm away under the
              // Hausdorff metric (median 9.3), while OR_R - the same anatomy mirrored
              // - managed 1.5 mm (median 6.1). A threshold suiting OR_R throws away
              // almost all of OR_L, and nothing in the output would say so. The tenth
              // percentile measures that offset without being set by a single lucky
              // streamline, and the margin above it covers the bundle's own spread.
              // Loosening only, and capped, so a run that produced nothing but junk
              // cannot talk its way into accepting it.
              const float p10 = percentile_of_sorted (finite, 0.10f);
              effective = std::min (1.5f * match.max_mdf,
                                    std::max (match.max_mdf, p10 + 0.4f * match.max_mdf));
            }
            report.applied_distance = effective;

            vector<size_t> passed;
            passed.reserve (surviving.size());
            for (const size_t i : surviving) {
              if (scores[i] <= effective) {
                passed.push_back (i);
                continue;
              }
              // Which of the two put it over decides how it is reported: a
              // streamline the bundle itself does not fit was never a competition
              // question, and a run whose report blamed neighbours for both would
              // hide that.
              if (distances[i] > effective) {
                reject (i, RefineStage::Distance);
              } else {
                reject (i, RefineStage::Competitor);
                if (penalised_by[i].size())
                  ++report.claimed_by[penalised_by[i]];
              }
            }
            surviving = std::move (passed);
          }

          // --- 4. competitive assignment, as an outright rejection ---
          // The all-or-nothing form, for when scoring is switched off: a neighbour
          // that is clearly closer takes the streamline outright.
          if (options.competitive && !options.competitive_scoring && others) {
            vector<size_t> passed;
            passed.reserve (surviving.size());
            for (const size_t i : surviving) {
              // Below the margin the streamline stays with the bundle that was
              // asked for; a competitor has to be clearly closer to take it.
              std::string claimant;
              if (others->claims (candidates[i], options.competitive_margin * distances[i], claimant)) {
                reject (i, RefineStage::Competitor);
                if (claimant.size())
                  ++report.claimed_by[claimant];
              } else {
                passed.push_back (i);
              }
            }
            surviving = std::move (passed);
          }

          // --- 4b. exclusive territory ---
          // What survives stage 4 is closest to this bundle taken as a whole. That is
          // not the same as staying out of a neighbour's ground: a streamline running
          // with the target for most of its length and crossing into a neighbour for
          // the rest wins the whole-streamline comparison, and then draws the target
          // through voxels where the two atlas bundles never meet. Asked per sample
          // rather than per streamline, which is the resolution overlap is seen at.
          if (options.exclusive_territory && others && options.exclusive_fraction < 1.0f) {
            vector<size_t> passed;
            passed.reserve (surviving.size());
            for (const size_t i : surviving) {
              std::string intruded;
              const float share = others->intrusion (candidates[i], *matcher,
                                                     options.exclusive_margin,
                                                     RefineOptions::exclusive_samples, intruded);
              if (share > options.exclusive_fraction) {
                reject (i, RefineStage::Territory);
                if (intruded.size())
                  ++report.claimed_by[intruded];
              } else {
                passed.push_back (i);
              }
            }
            surviving = std::move (passed);
          }

          // --- 5. outlier prune, per node ---
          if (std::isfinite (options.outlier_k) && options.per_node_outliers && surviving.size() >= 10) {
            vector<Streamline<float>> kept_so_far;
            kept_so_far.reserve (surviving.size());
            for (const size_t i : surviving)
              kept_so_far.push_back (candidates[i]);
            const vector<float> deviation = per_node_core_distance (kept_so_far);
            report.outlier_cutoff = options.outlier_k;
            vector<size_t> passed;
            passed.reserve (surviving.size());
            for (size_t k = 0; k != surviving.size(); ++k) {
              if (k < deviation.size() && deviation[k] > options.outlier_k)
                reject (surviving[k], RefineStage::Outlier);
              else
                passed.push_back (surviving[k]);
            }
            surviving = std::move (passed);
          }
          // --- 5. outlier prune, whole streamline ---
          else if (std::isfinite (options.outlier_k) && surviving.size() >= 10 && matcher) {
            // Against what was kept, not against the atlas: the point is to remove
            // the tail of this result, whose scale is not known in advance. Median
            // and MAD rather than mean and standard deviation, because the tail
            // being measured would otherwise set the scale that is meant to catch it.
            vector<float> d;
            d.reserve (surviving.size());
            for (const size_t i : surviving)
              if (std::isfinite (distances[i]))
                d.push_back (distances[i]);
            if (d.size() >= 10) {
              std::sort (d.begin(), d.end());
              const float median = percentile_of_sorted (d, 0.5f);
              vector<float> deviation;
              deviation.reserve (d.size());
              for (const float q : d)
                deviation.push_back (std::abs (q - median));
              std::sort (deviation.begin(), deviation.end());
              const float mad = percentile_of_sorted (deviation, 0.5f);
              // A bundle so uniform that its MAD is zero has no tail to cut; the
              // scale floor stops that case from rejecting everything but the median.
              const float cutoff = median + options.outlier_k * std::max (0.5f, mad);
              report.outlier_cutoff = cutoff;
              vector<size_t> passed;
              passed.reserve (surviving.size());
              for (const size_t i : surviving) {
                if (std::isfinite (distances[i]) && distances[i] > cutoff)
                  reject (i, RefineStage::Outlier);
                else
                  passed.push_back (i);
              }
              surviving = std::move (passed);
            }
          }

          // --- collect, in candidate order ---
          vector<bool> is_kept (candidates.size(), false);
          for (const size_t i : surviving)
            is_kept[i] = true;
          kept.reserve (surviving.size());
          rejected.reserve (candidates.size() - surviving.size());
          for (size_t i = 0; i != candidates.size(); ++i)
            (is_kept[i] ? kept : rejected).push_back (candidates[i]);
          report.kept = kept.size();
        }

      }
    }
  }
}
