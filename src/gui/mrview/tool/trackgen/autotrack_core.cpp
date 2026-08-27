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

#include "gui/mrview/tool/trackgen/autotrack_core.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>

#include "algo/loop.h"
#include "transform.h"
#include "file/path.h"
#include "filter/dilate.h"

#include "dwi/tractography/file.h"
#include "dwi/tractography/properties.h"
#include "dwi/tractography/roi.h"
#include "dwi/tractography/seeding/basic.h"
#include "dwi/tractography/seeding/list.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        using namespace MR::DWI::Tractography;
        using namespace MR::DWI::Tractography::Recognition;


        namespace
        {
          MR::Header binary_grid (const MR::Header& grid)
          {
            MR::Header H (grid);
            H.ndim() = 3;
            H.datatype() = MR::DataType::Bit;
            return H;
          }

          float min_spacing (const MR::Header& H)
          {
            return std::min ({ float (H.spacing(0)), float (H.spacing(1)), float (H.spacing(2)) });
          }

          void mark (MR::Image<bool>& mask, const transform_type& scanner2voxel, const Eigen::Vector3f& p)
          {
            const Eigen::Vector3d v = scanner2voxel * p.cast<double>();
            const ssize_t i = std::lround (v[0]), j = std::lround (v[1]), k = std::lround (v[2]);
            if (i < 0 || j < 0 || k < 0 ||
                i >= mask.size(0) || j >= mask.size(1) || k >= mask.size(2))
              return;
            mask.index(0) = i; mask.index(1) = j; mask.index(2) = k;
            mask.value() = true;
          }
        }



        MR::Image<bool> rasterise_streamlines (const vector<Streamline<float>>& tracks,
                                               const MR::Header& grid)
        {
          MR::Header H = binary_grid (grid);
          auto mask = MR::Image<bool>::scratch (H, "streamline territory");
          const MR::Transform T (H);
          // Step along each segment at half a voxel so no voxel is skipped
          // between two widely spaced vertices.
          const float step = 0.5f * min_spacing (H);

          for (const auto& tck : tracks) {
            if (tck.empty())
              continue;
            mark (mask, T.scanner2voxel, tck.front());
            for (size_t i = 1; i < tck.size(); ++i) {
              const Eigen::Vector3f& a = tck[i-1];
              const Eigen::Vector3f& b = tck[i];
              const float length = (b - a).norm();
              const size_t n = std::max<size_t> (1, size_t (std::ceil (length / step)));
              for (size_t s = 1; s <= n; ++s)
                mark (mask, T.scanner2voxel, a + (float(s)/float(n)) * (b - a));
            }
          }
          return mask;
        }



        void dilate_mask (MR::Image<bool>& mask, float radius_mm)
        {
          if (radius_mm <= 0.0f)
            return;
          const unsigned int npass = std::max<unsigned int> (1,
              (unsigned int) std::lround (radius_mm / min_spacing (MR::Header (mask))));
          MR::Filter::Dilate dilate (mask);
          dilate.set_npass (npass);
          auto out = MR::Image<bool>::scratch (MR::Header (mask), "dilated");
          dilate (mask, out);
          mask = out;
        }



        void endpoint_clouds (const vector<Streamline<float>>& tracks,
                              vector<Eigen::Vector3f>& cloud_a,
                              vector<Eigen::Vector3f>& cloud_b)
        {
          // The same clouds the post-tracking endpoint gate needs, so there is one
          // implementation of "which end is which" rather than two that could drift.
          MR::DWI::Tractography::Recognition::endpoint_clouds (tracks, cloud_a, cloud_b);
        }



        namespace
        {
          MR::Image<bool> cloud_to_mask (const vector<Eigen::Vector3f>& cloud,
                                         const MR::Header& grid,
                                         float dilate_mm)
          {
            MR::Header H = binary_grid (grid);
            auto mask = MR::Image<bool>::scratch (H, "endpoint region");
            const MR::Transform T (H);
            for (const auto& p : cloud)
              mark (mask, T.scanner2voxel, p);
            dilate_mask (mask, dilate_mm);
            return mask;
          }

          bool mask_is_empty (MR::Image<bool>& mask)
          {
            for (auto l = MR::Loop (0, 3) (mask); l; ++l)
              if (mask.value())
                return false;
            return true;
          }
        }



        vector<Streamline<float>> read_bundle (const std::string& path)
        {
          vector<Streamline<float>> out;
          Properties props;
          Reader<float> reader (path, props);
          Streamline<float> tck;
          while (reader (tck))
            out.push_back (tck);
          return out;
        }



        void autotrack_bundle (const std::string& bundle_path,
                               const std::string& fod_path,
                               const MR::Header& grid,
                               const AutotrackParams& params,
                               AutotrackResult& result,
                               TrackGenState& state,
                               vector<Streamline<float>>* live,
                               vector<Streamline<float>>* rejected)
        {
          if (rejected)
            rejected->clear();
          result.name = Path::basename (bundle_path);
          const size_t dot = result.name.find_last_of ('.');
          if (dot != std::string::npos)
            result.name = result.name.substr (0, dot);
          result.tracks.clear();
          result.generated = result.attempted = 0;
          result.error.clear();

          // --- the atlas bundle ---
          const vector<Streamline<float>> atlas = read_bundle (bundle_path);
          result.atlas_streamlines = atlas.size();
          if (atlas.empty())
            throw Exception ("atlas bundle \"" + bundle_path + "\" is empty");

          // --- tracking territory ---
          auto territory = rasterise_streamlines (atlas, grid);
          if (mask_is_empty (territory))
            throw Exception ("bundle does not overlap the FOD image; are they in the same space?");
          dilate_mask (territory, params.dilate_mm);

          // --- endpoint inclusion regions ---
          MR::Image<bool> end_a, end_b;
          bool have_endpoints = false;
          if (params.use_endpoint_includes) {
            vector<Eigen::Vector3f> cloud_a, cloud_b;
            endpoint_clouds (atlas, cloud_a, cloud_b);
            if (cloud_a.size() && cloud_b.size()) {
              end_a = cloud_to_mask (cloud_a, grid, params.endpoint_dilate_mm);
              end_b = cloud_to_mask (cloud_b, grid, params.endpoint_dilate_mm);
              have_endpoints = !mask_is_empty (end_a) && !mask_is_empty (end_b);
            }
          }

          if (state.cancel)
            return;

          // --- track within the territory ---
          // Track straight into the caller's buffer when it wants to watch: the
          // write kernel appends under state.results_mutex, so a reader holding
          // that lock always sees a consistent set.
          vector<Streamline<float>> private_buffer;
          vector<Streamline<float>>& generated = live ? *live : private_buffer;
          {
            std::lock_guard<std::mutex> lock (state.results_mutex);
            generated.clear();
          }
          {
            Properties properties;
            for (const auto& kv : params.scalars)
              properties[kv.first] = kv.second;
            properties["max_num_tracks"] = str (params.select);
            if (params.max_seeds_factor)
              properties["max_num_seeds"] = str (params.select * params.max_seeds_factor);

            if (params.seeds_per_voxel)
              properties.seeds.add (new Seeding::Random_per_voxel (territory, result.name + " territory",
                                                                   params.seeds_per_voxel));
            else
              properties.seeds.add (new Seeding::SeedMask (territory, result.name + " territory"));
            if (params.restrict_to_territory)
              properties.mask.add (ROI (territory, result.name + " territory"));
            if (have_endpoints) {
              properties.include.add (ROI (end_a, result.name + " end A"));
              properties.include.add (ROI (end_b, result.name + " end B"));
            }

            run_tracking (params.algorithm, fod_path, properties, generated, state, params.nthreads);

            // Capture what the algorithm settled on (method, step size, angle,
            // thresholds, ...) so the saved .tck is as self-describing as one
            // written by tckgen.
            for (const auto& kv : properties)
              result.effective_properties[kv.first] = kv.second;
          }
          result.generated = generated.size();
          result.attempted = state.streamlines;

          // Hitting the seed cap means the settings are asking for more streamlines
          // than this territory can supply; silently returning fewer would look
          // like the bundle simply has few streamlines.
          if (params.max_seeds_factor && state.seeds >= params.select * params.max_seeds_factor
              && generated.size() < params.select) {
            WARN ("bundle \"" + result.name + "\": stopped after " + str(state.seeds)
                  + " seeding attempts with only " + str(generated.size()) + " of "
                  + str(params.select) + " streamlines; loosen the territory, lower the "
                  "minimum length, or reduce the target count");
          }

          if (state.cancel)
            return;

          // --- keep the streamlines that look like the atlas bundle ---
          vector<Streamline<float>> kept, discarded;
          autotrack_match (atlas, params, generated, kept, discarded, result.effective_distance,
                           &result.refine_report);
          if (rejected)
            *rejected = std::move (discarded);

          if (live) {
            // Whoever is displaying *live now sees the rejects vanish.
            std::lock_guard<std::mutex> lock (state.results_mutex);
            *live = std::move (kept);
          } else {
            result.tracks = std::move (kept);
          }
        }



        void autotrack_match (const vector<Streamline<float>>& atlas,
                              const AutotrackParams& params,
                              const vector<Streamline<float>>& candidates,
                              vector<Streamline<float>>& kept,
                              vector<Streamline<float>>& rejected,
                              float& effective_distance,
                              Recognition::RefineReport* report)
        {
          // Everything this used to do inline now lives in
          // dwi/tractography/recognition/refine.cpp, so the same five stages run
          // here and under tckrefine - which is what let the competitive stage be
          // measured against held-out atlas halves rather than argued about.
          Recognition::RefineReport local;
          Recognition::RefineReport& r = report ? *report : local;

          std::unique_ptr<Recognition::PopulationMap> population;
          if (params.population_map.size()) {
            try {
              population.reset (new Recognition::PopulationMap (params.population_map,
                                                                params.population_to_map));
            } catch (Exception& e) {
              // Without it the other stages still run; saying nothing would leave the
              // user wondering why a bundle behaved differently from its neighbour.
              WARN ("could not read the population map \"" + params.population_map
                    + "\": " + e[0]);
            }
          }

          Recognition::refine_bundle (atlas, params.match, params.refine,
                                      params.competitors, candidates, kept, rejected, r,
                                      nullptr, population.get());
          effective_distance = r.applied_distance;
        }



        vector<std::string> list_atlas_bundles (const std::string& directory)
        {
          vector<std::string> out;
          if (!Path::is_dir (directory))
            return out;
          Path::Dir dir (directory);
          std::string entry;
          while ((entry = dir.read_name()).size()) {
            if (Path::has_suffix (entry, ".tck"))
              out.push_back (Path::join (directory, entry));
          }
          std::sort (out.begin(), out.end());
          return out;
        }


      }
    }
  }
}
