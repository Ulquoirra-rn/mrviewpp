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

#include "gui/mrview/tool/tractography/bundle_stats.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "transform.h"
#include "interp/linear.h"

#include "dwi/tractography/recognition/mdf.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        using namespace MR::DWI::Tractography;


        std::string BundleStats::csv_header ()
        {
          return "bundle,count,mean_length_mm,median_length_mm,sd_length_mm,"
                 "min_length_mm,max_length_mm,total_length_mm,mean_span_mm,"
                 "mean_curvature_deg_per_mm,volume_mm3,volume_voxels";
        }


        std::string BundleStats::as_csv_row (const std::string& name) const
        {
          return name + "," + str(count) + "," + str(mean_length,6) + "," + str(median_length,6)
               + "," + str(stdev_length,6) + "," + str(min_length,6) + "," + str(max_length,6)
               + "," + str(total_length,6) + "," + str(mean_span,6) + "," + str(mean_curvature,6)
               + "," + str(volume,6) + "," + str(volume_voxels);
        }


        std::string BundleStats::as_text (const std::string& name) const
        {
          if (!count)
            return name + ": no streamlines";
          return name + "\n"
               + "  streamlines:    " + str(count) + "\n"
               + "  length (mm):    mean " + str(mean_length,5) + ", median " + str(median_length,5)
                 + ", sd " + str(stdev_length,4) + "\n"
               + "                  range " + str(min_length,5) + " - " + str(max_length,5) + "\n"
               + "  total length:   " + str(total_length,6) + " mm\n"
               + "  mean span:      " + str(mean_span,5) + " mm\n"
               + "  mean curvature: " + str(mean_curvature,4) + " deg/mm\n"
               + "  volume:         " + str(volume,6) + " mm3 (" + str(volume_voxels) + " voxels)";
        }



        BundleStats compute_bundle_stats (const vector<Streamline<float>>& tracks,
                                          const MR::Header* grid)
        {
          BundleStats stats;
          if (tracks.empty())
            return stats;

          vector<float> lengths;
          lengths.reserve (tracks.size());
          double span_sum = 0.0, curvature_sum = 0.0;
          size_t curvature_n = 0;

          for (const auto& tck : tracks) {
            if (tck.size() < 2)
              continue;
            const float length = Recognition::track_length (tck);
            lengths.push_back (length);
            span_sum += (tck.back() - tck.front()).norm();

            // Mean turning angle per unit arc length: a scale-free measure of how
            // much the streamline bends.
            if (tck.size() >= 3 && length > 0.0f) {
              double angle_sum = 0.0;
              for (size_t i = 1; i + 1 < tck.size(); ++i) {
                const Eigen::Vector3f a = (tck[i] - tck[i-1]);
                const Eigen::Vector3f b = (tck[i+1] - tck[i]);
                const float na = a.norm(), nb = b.norm();
                if (na <= 0.0f || nb <= 0.0f)
                  continue;
                const float c = std::max (-1.0f, std::min (1.0f, a.dot(b) / (na*nb)));
                angle_sum += std::acos (c) * 180.0 / Math::pi;
              }
              curvature_sum += angle_sum / length;
              ++curvature_n;
            }
          }

          if (lengths.empty())
            return stats;

          stats.count = lengths.size();
          double sum = 0.0;
          for (const float l : lengths)
            sum += l;
          stats.total_length = float (sum);
          stats.mean_length = float (sum / lengths.size());

          vector<float> sorted (lengths);
          std::sort (sorted.begin(), sorted.end());
          stats.min_length = sorted.front();
          stats.max_length = sorted.back();
          stats.median_length = sorted.size() % 2
              ? sorted[sorted.size()/2]
              : 0.5f * (sorted[sorted.size()/2 - 1] + sorted[sorted.size()/2]);

          // Sample standard deviation (n-1), to agree with tckstats.
          double var = 0.0;
          for (const float l : lengths)
            var += Math::pow2 (double (l) - stats.mean_length);
          stats.stdev_length = lengths.size() > 1
              ? float (std::sqrt (var / (lengths.size() - 1)))
              : 0.0f;

          stats.mean_span = float (span_sum / lengths.size());
          if (curvature_n)
            stats.mean_curvature = float (curvature_sum / curvature_n);

          // Volume: count the distinct voxels the bundle occupies. Vertices are
          // stepped at half a voxel so no voxel between two vertices is missed.
          if (grid) {
            const MR::Transform T (*grid);
            const float step = 0.5f * std::min ({ float (grid->spacing(0)),
                                                  float (grid->spacing(1)),
                                                  float (grid->spacing(2)) });
            std::unordered_set<uint64_t> voxels;
            auto add = [&] (const Eigen::Vector3f& p) {
              const Eigen::Vector3d v = T.scanner2voxel * p.cast<double>();
              const int64_t i = std::lround (v[0]), j = std::lround (v[1]), k = std::lround (v[2]);
              if (i < 0 || j < 0 || k < 0 ||
                  i >= grid->size(0) || j >= grid->size(1) || k >= grid->size(2))
                return;
              voxels.insert ((uint64_t(i) << 42) | (uint64_t(j) << 21) | uint64_t(k));
            };
            for (const auto& tck : tracks) {
              if (tck.empty())
                continue;
              add (tck.front());
              for (size_t i = 1; i < tck.size(); ++i) {
                const float d = (tck[i] - tck[i-1]).norm();
                const size_t n = std::max<size_t> (1, size_t (std::ceil (d / step)));
                for (size_t s = 1; s <= n; ++s)
                  add (tck[i-1] + (float(s)/float(n)) * (tck[i] - tck[i-1]));
              }
            }
            stats.volume_voxels = voxels.size();
            stats.volume = float (voxels.size() * grid->spacing(0) * grid->spacing(1) * grid->spacing(2));
          }

          return stats;
        }



        std::string AlongTractProfile::as_csv () const
        {
          std::string out = "node,mean,sd,n\n";
          for (size_t i = 0; i != mean.size(); ++i)
            out += str(i) + "," + str(mean[i],6) + "," + str(stdev[i],6) + "," + str(count[i]) + "\n";
          return out;
        }



        AlongTractProfile compute_along_tract_profile (const vector<Streamline<float>>& tracks,
                                                       MR::Image<float>& scalar,
                                                       size_t num_nodes,
                                                       const std::string& scalar_name)
        {
          AlongTractProfile profile;
          profile.scalar_name = scalar_name;
          if (!num_nodes || tracks.empty())
            return profile;

          profile.mean.assign (num_nodes, 0.0f);
          profile.stdev.assign (num_nodes, 0.0f);
          profile.count.assign (num_nodes, 0);
          vector<double> sum (num_nodes, 0.0), sum_sq (num_nodes, 0.0);

          auto interp = MR::Interp::make_linear (scalar);

          // Orientation matters: node 0 must mean the same end for every
          // streamline, or the profile averages the two ends together.
          Recognition::FixedTrack reference, fixed;
          size_t longest = 0, reference_index = 0;
          for (size_t i = 0; i != tracks.size(); ++i) {
            if (tracks[i].size() > longest) {
              longest = tracks[i].size();
              reference_index = i;
            }
          }
          Recognition::to_fixed (tracks[reference_index], num_nodes, reference);
          if (!reference.rows())
            return profile;

          for (const auto& tck : tracks) {
            Recognition::to_fixed (tck, num_nodes, fixed);
            if (!fixed.rows())
              continue;
            if (Recognition::is_flipped (reference, fixed))
              fixed = fixed.colwise().reverse().eval();

            for (size_t n = 0; n != num_nodes; ++n) {
              const Eigen::Vector3f p = fixed.row(n).transpose();
              if (!interp.scanner (p))
                continue;
              const float value = interp.value();
              if (!std::isfinite (value))
                continue;
              sum[n] += value;
              sum_sq[n] += double(value) * value;
              ++profile.count[n];
            }
          }

          for (size_t n = 0; n != num_nodes; ++n) {
            if (!profile.count[n])
              continue;
            const double m = sum[n] / profile.count[n];
            profile.mean[n] = float (m);
            profile.stdev[n] = float (std::sqrt (std::max (0.0, sum_sq[n]/profile.count[n] - m*m)));
          }
          return profile;
        }


      }
    }
  }
}
