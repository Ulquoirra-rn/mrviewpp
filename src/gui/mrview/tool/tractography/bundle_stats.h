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

#ifndef __gui_mrview_tool_bundle_stats_h__
#define __gui_mrview_tool_bundle_stats_h__

#include "header.h"
#include "image.h"
#include "types.h"

#include "dwi/tractography/streamline.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        // Per-bundle morphometry and along-tract profiles. Pure CPU, no Qt and
        // no GL, so it can be exercised without the GUI.

        struct BundleStats { NOMEMALIGN
          size_t count = 0;
          float total_length = 0.0f;
          float mean_length = 0.0f, median_length = 0.0f, stdev_length = 0.0f;
          float min_length = 0.0f, max_length = 0.0f;
          float mean_span = 0.0f;        //!< straight-line endpoint separation
          float mean_curvature = 0.0f;   //!< mean turning angle per mm, in deg/mm
          float volume = 0.0f;           //!< voxels occupied x voxel volume, mm^3
          size_t volume_voxels = 0;

          std::string as_text (const std::string& name) const;
          static std::string csv_header ();
          std::string as_csv_row (const std::string& name) const;
        };


        //! Morphometry of a bundle. \a grid is used for the volume estimate only.
        BundleStats compute_bundle_stats (
            const vector<MR::DWI::Tractography::Streamline<float>>&,
            const MR::Header* grid = nullptr);


        struct AlongTractProfile { NOMEMALIGN
          vector<float> mean;    //!< one entry per node
          vector<float> stdev;
          vector<size_t> count;  //!< contributing streamlines per node
          std::string scalar_name;

          std::string as_csv () const;
        };


        //! Sample a scalar image along each streamline and average across the bundle.
        /*! Streamlines are resampled to \a num_nodes points by arc length first, so
         *  node i means the same relative position along every streamline. */
        AlongTractProfile compute_along_tract_profile (
            const vector<MR::DWI::Tractography::Streamline<float>>&,
            MR::Image<float>& scalar,
            size_t num_nodes,
            const std::string& scalar_name);

      }
    }
  }
}

#endif
