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

#ifndef __gui_mrview_tool_trackgen_h__
#define __gui_mrview_tool_trackgen_h__

#include <thread>

#include "progressbar.h"
#include "dwi/tractography/properties.h"
#include "dwi/tractography/streamline.h"

#include "gui/mrview/tool/base.h"
#include "gui/mrview/adjust_button.h"
#include "gui/mrview/tool/trackgen/runner.h"
#include "gui/mrview/tool/trackgen/autotrack_core.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        // Generate streamlines from the FOD image loaded in the ODF tool.
        //
        // Two modes, chosen by whether an atlas bundle is selected:
        //
        //  - no bundle selected: manual tracking. Any region offered by the
        //    Overlay / Atlas / ROI-editor tools can act as a seed, include,
        //    ordered-include, exclude or mask ROI.
        //
        //  - a bundle selected: atlas auto-tracking. The bundle's own footprint
        //    becomes the tracking territory and its two ends become inclusion
        //    regions, then the result is filtered down to the streamlines whose
        //    shape matches the bundle (see autotrack_core.h). Selecting a bundle
        //    is what puts the tool in this mode, so the manual region table is
        //    hidden and the recognition settings appear in its place.
        //
        // Either way this runs MRtrix's own tracking engine in-process (see
        // runner.h); it does not shell out to tckgen. Results are held in RAM and
        // handed to the Tractography tool, and only written to disk if you ask.
        class TrackGen : public Base
        { MEMALIGN(TrackGen)
            Q_OBJECT

          public:
            TrackGen (Dock* parent);
            ~TrackGen ();

            static void add_commandline_options (MR::App::OptionList& options);
            bool process_commandline_option (const MR::App::ParsedOption& opt) override;

            std::string session_key () const override { return "trackgen"; }
            void get_session (nlohmann::json&) const override;
            void set_session (const nlohmann::json&) override;

          private slots:
            void refresh_sources_slot ();
            void browse_atlas_slot ();
            void bundle_changed_slot (int);
            void metric_changed_slot (int);
            void add_region_slot ();
            void remove_region_slot ();
            void generate_slot ();
            void cancel_slot ();
            void poll_progress_slot ();
            void generation_finished ();

          protected:
            // One row of the region table: a reference plus the role it plays.
            struct Assignment { NOMEMALIGN
              RegionRef region;
              int role;     // index into role_names
            };

            QComboBox* source_combo;
            QComboBox* algorithm_combo;
            QComboBox* bundle_combo;
            QPushButton* atlas_button;
            QGroupBox* region_box;
            QGroupBox* autotrack_box;
            QTableWidget* region_table;
            QPushButton *add_region_button, *remove_region_button;
            QPushButton *generate_button, *cancel_button;
            QProgressBar* progress_bar;
            QLabel* status_label;

            AdjustButton *select_button, *step_button, *angle_button;
            AdjustButton *minlength_button, *maxlength_button, *cutoff_button;
            QSpinBox *seeds_per_voxel_button, *nthreads_button;
            QCheckBox *rk4_box, *stop_box, *unidirectional_box;

            // Auto-tracking settings; only meaningful with a bundle selected.
            AdjustButton *dilate_button, *endpoint_dilate_button;
            AdjustButton *mdf_button, *qb_button;
            QCheckBox* endpoint_includes_box;
            QCheckBox* restrict_box;
            QComboBox* metric_combo;
            size_t max_seeds_factor = 10;

            vector<Assignment> assignments;

            // Worker thread state; see the equivalent pattern in
            // roi_editor/roi.cpp's grow_cut_slot().
            std::thread worker;
            TrackGenState state;
            vector<MR::DWI::Tractography::Streamline<float>> results;
            MR::DWI::Tractography::Properties result_properties;
            QTimer* poll_timer;
            std::string pending_name;
            //! Streamlines generated before recognition; 0 in manual mode.
            uint64_t bundle_generated = 0;
            //! Run counter, so manual tracts are named tract_1, tract_2, ...
            size_t tract_iteration = 0;
            std::string result_error;
            // ProgressBar's display hooks are process-wide globals that mrview
            // points at Qt widgets; they are muted while the worker runs.
            void (*saved_progress_display) (const MR::ProgressBar&) = nullptr;
            void (*saved_progress_done) (const MR::ProgressBar&) = nullptr;

            //! Path of the selected atlas bundle; empty means manual mode.
            std::string current_bundle () const;
            void load_atlas_bundles (const vector<std::string>& paths);
            void update_mode ();
            AutotrackParams collect_autotrack_params () const;

            void restore_progress_hooks ();
            bool add_region_by_name (const std::string& name, int role);
            void run_and_save (const std::string& path);
            void rebuild_region_table ();
            void apply_region_opacities ();
            void set_controls_enabled (bool);
        };

      }
    }
  }
}

#endif
