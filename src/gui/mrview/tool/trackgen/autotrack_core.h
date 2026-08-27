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

#ifndef __gui_mrview_tool_trackgen_autotrack_core_h__
#define __gui_mrview_tool_trackgen_autotrack_core_h__

#include "header.h"
#include "image.h"
#include "types.h"

#include "dwi/tractography/streamline.h"
#include "dwi/tractography/recognition/bundle_matcher.h"
#include "dwi/tractography/recognition/refine.h"

#include "gui/mrview/tool/trackgen/runner.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        // Atlas-driven automatic bundle reconstruction, in the style of DSI
        // Studio's AutoTrack.
        //
        // The atlas is a set of .tck bundles already in the subject's space, so
        // there is no registration step. For each bundle we derive a tracking
        // territory from where the atlas bundle runs, track generously inside it,
        // and then keep only the streamlines whose shape matches the atlas
        // bundle (see dwi/tractography/recognition/).
        //
        // Tracking the territory alone is not enough - many unrelated tracts
        // cross any given bundle - which is why the recognition step, and the
        // optional endpoint inclusion regions, do the real work.

        struct AutotrackParams { NOMEMALIGN
          int algorithm = 2;                    //!< index into trackgen_algorithms
          size_t select = 10000;                //!< streamlines to generate per bundle
          size_t seeds_per_voxel = 0;           //!< 0 = random seeding within the territory
          size_t nthreads = 0;
          //! Cap on seeding attempts per bundle, as a multiple of `select`.
          /*! Without this a bundle that yields nothing (wrong space, too tight a
           *  territory, contradictory endpoint regions) would consume tckgen's
           *  implicit budget of 1000x the target before giving up - minutes of
           *  apparent hang per bad bundle, which makes a batch unusable.
           *
           *  Tracking, not recognition, is what costs time here: a tight territory
           *  combined with a high minimum length makes most seeds fail, so the
           *  budget is what bounds the wait. Raise it only if you are willing to
           *  wait proportionally longer. */
          size_t max_seeds_factor = 40;
          float dilate_mm = 4.0f;               //!< grow the territory by this much
          float endpoint_dilate_mm = 6.0f;
          bool use_endpoint_includes = true;
          //! Confine tracking to the bundle territory.
          /*! On, the territory acts as a tracking mask, so streamlines are
           *  truncated where they leave it - which keeps results tight to the
           *  atlas but shortens them. Off, streamlines run their natural length
           *  and only the shape match decides, which suits an atlas built by a
           *  different pipeline. */
          bool restrict_to_territory = true;
          std::map<std::string, std::string> scalars;   //!< extra tracking properties
          MR::DWI::Tractography::Recognition::BundleMatcher::Params match;
          //! The gates applied after tracking, beyond the shape distance itself.
          MR::DWI::Tractography::Recognition::RefineOptions refine;
          //! Bundles allowed to claim a streamline away from this one.
          /*! Only consulted when refine.competitive is set. Carried by value
           *  because the worker runs off the GUI thread and the atlas cache these
           *  come from belongs to the GUI thread. */
          vector<MR::DWI::Tractography::Recognition::Competitor> competitors;

          //! Population probability map for this bundle; empty when the atlas has none.
          /*! A path rather than an open image, for the same reason competitors are
           *  carried by value: this crosses onto the worker thread. */
          std::string population_map;
          //! Takes a point in subject space into the map's space.
          /*! The maps are in atlas space and the streamlines are in the subject's, so
           *  this is the inverse of the atlas fit. Sampling through the inverse beats
           *  resampling the map: it is exact, and it costs nothing to set up. */
          transform_type population_to_map = transform_type::Identity();
        };


        struct AutotrackResult { NOMEMALIGN
          std::string name;
          vector<MR::DWI::Tractography::Streamline<float>> tracks;   //!< recognised
          size_t atlas_streamlines = 0;
          size_t generated = 0;      //!< before recognition
          size_t attempted = 0;      //!< streamlines the engine attempted
          //! The matching distance actually applied, after the per-run adjustment
          //  for how far this bundle's candidates fell from the registered atlas.
          float effective_distance = 0.0f;
          //! What each stage of the post-tracking filtering removed.
          MR::DWI::Tractography::Recognition::RefineReport refine_report;
          std::string error;         //!< non-empty if this bundle failed
          //! The tracking parameters actually used, for the output .tck header.
          /*! Properties is not copyable (Seeding::List owns its seeders), so the
           *  effective settings are handed back as plain key/value pairs. */
          std::map<std::string, std::string> effective_properties;
        };


        //! Mark every voxel the streamlines pass through.
        /*! Vertices are interpolated to sub-voxel spacing first, so a streamline
         *  never skips a voxel between two of its vertices. */
        MR::Image<bool> rasterise_streamlines (
            const vector<MR::DWI::Tractography::Streamline<float>>&,
            const MR::Header& grid);

        //! Dilate a binary mask by approximately \a radius_mm.
        void dilate_mask (MR::Image<bool>& mask, float radius_mm);

        //! Split a bundle's endpoints into its two terminal clouds.
        /*! Streamlines are first oriented consistently against the longest one;
         *  without that, "first vertex" and "last vertex" would be arbitrary and
         *  the two clouds would each be a mixture of both ends. */
        void endpoint_clouds (const vector<MR::DWI::Tractography::Streamline<float>>&,
                              vector<Eigen::Vector3f>& cloud_a,
                              vector<Eigen::Vector3f>& cloud_b);

        //! Reconstruct one bundle. Blocks; pure CPU, no GL and no Qt.
        /*! If \a live is given, tracking appends its candidates there instead of to
         *  a private buffer, and the recognised subset replaces its contents once
         *  matching is done - so a GUI watching that vector under
         *  TrackGenState::results_mutex sees streamlines arrive as they are selected
         *  and the rejects disappear at the end. AutotrackResult::tracks is then
         *  left empty, since \a live already holds the result. */
        void autotrack_bundle (const std::string& bundle_path,
                               const std::string& fod_path,
                               const MR::Header& grid,
                               const AutotrackParams&,
                               AutotrackResult&,
                               TrackGenState&,
                               vector<MR::DWI::Tractography::Streamline<float>>* live = nullptr,
                               //! If given, receives the candidates the matching step
                               //  threw away, so they can be inspected rather than lost.
                               vector<MR::DWI::Tractography::Streamline<float>>* rejected = nullptr);

        //! Keep the candidates that match \a atlas, discarding the rest.
        /*! The recognition half of autotrack_bundle(), on its own so it can be re-run
         *  against candidates that are already in hand: changing the matching distance
         *  is milliseconds of work, where re-tracking to reach the same candidates is
         *  minutes. \a effective_distance receives the threshold actually applied,
         *  which may be looser than the one asked for - see the p10 rule inside. */
        void autotrack_match (const vector<MR::DWI::Tractography::Streamline<float>>& atlas,
                              const AutotrackParams&,
                              const vector<MR::DWI::Tractography::Streamline<float>>& candidates,
                              vector<MR::DWI::Tractography::Streamline<float>>& kept,
                              vector<MR::DWI::Tractography::Streamline<float>>& rejected,
                              float& effective_distance,
                              //! If given, receives what each stage removed.
                              MR::DWI::Tractography::Recognition::RefineReport* report = nullptr);

        //! Read the streamlines of one atlas bundle from disk.
        vector<MR::DWI::Tractography::Streamline<float>> read_bundle (const std::string& path);

        //! List the .tck files in a directory, sorted by name.
        vector<std::string> list_atlas_bundles (const std::string& directory);

      }
    }
  }
}

#endif
