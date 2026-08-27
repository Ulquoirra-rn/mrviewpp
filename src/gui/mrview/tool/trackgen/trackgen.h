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

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include <QTemporaryDir>

#include "timer.h"

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

            //! Docked on the left, opposite the Tracts panel it hands tracts to.
            /*! The two are one workflow and were briefly one panel, which fixed the
             *  problem that made merging tempting - handing a tract over raised Tracts
             *  over this on every run - but cost the tract list its own height. Two
             *  docks on opposite edges settle both: neither can raise over the other,
             *  because they are not in the same stack. */
            static Qt::DockWidgetArea preferred_dock_area () { return Qt::LeftDockWidgetArea; }

            static void add_commandline_options (MR::App::OptionList& options);
            bool process_commandline_option (const MR::App::ParsedOption& opt) override;

            //! Atlas bundles offered as regions, so a manual run can seed from one.
            RegionProvider* region_provider () override;

            std::string session_key () const override { return "trackgen"; }
            void get_session (nlohmann::json&) const override;
            void set_session (const nlohmann::json&) override;

          private slots:
            void refresh_sources_slot ();
            void mode_changed_slot (int);
            void quality_changed_slot (int);
            void bundle_item_changed_slot (QListWidgetItem*);
            void recognition_changed_slot ();
            void clear_selection_slot ();
            void find_sources_slot ();
            void browse_atlas_slot ();
            void bundle_search_activated (const QString&);
            void bundle_filter_changed_slot (const QString&);
            void show_rejected_slot ();
            void atlas_registration_changed ();
            void category_changed_slot (int);
            void algorithm_changed_slot (int);
            void autotrack_param_edited ();
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
            //! Bundles of the shown category, each checkable.
            /*! A list rather than a drop-down because a run can reconstruct several
             *  bundles, and a drop-down cannot show more than one choice at a time. */
            QListWidget* bundle_list;
            QComboBox* category_combo;
            QLineEdit* bundle_filter;
            QCompleter* bundle_completer;
            QStringListModel* bundle_search_model;

            //! One selectable bundle: which group it is in, and how to reach it.
            struct BundleEntry { NOMEMALIGN
              std::string category;   //!< subfolder, or "loaded" for a user file
              std::string name;       //!< as shown in the bundle drop-down
              std::string data;       //!< a .tck path, or "builtin:<name>"
            };
            vector<BundleEntry> catalogue;
            //! Bundles ticked for the next run, by name.
            /*! Held here rather than read off the list widget, so a tick survives a
             *  change of category or of the search filter - which is the whole point
             *  of being able to pick several. */
            std::set<std::string> checked_bundles;
            QLabel* selection_label;
            QPushButton* clear_selection_button;
            void refresh_bundles_in_category (const QString& preferred);
            //! Re-tick the visible rows from checked_bundles, and update the summary.
            void sync_bundle_checks ();
            //! Names of every ticked bundle, in catalogue order.
            vector<std::string> selected_bundle_names () const;
            QPushButton* atlas_button;
            //! Guided hides everything a clinical run does not need to touch.
            QComboBox* mode_combo;
            QComboBox* quality_combo;
            //! Widgets shown in one mode only; apply_mode() does the hiding.
            vector<QWidget*> guided_only, expert_only;
            void apply_mode ();
            bool guided () const;
            //! Force the settings Guided hides but a run still needs.
            /*! Called from the constructor as well as the mode switch, because the
             *  constructor sets the mode before connecting its signal. */
            void apply_guided_defaults ();
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
            //! Seeding budget, as a multiple of the streamline target.
            /*! Measured on the test FOD: a thin bundle such as OR_L yields under 1%
             *  of seeds, so at 10x it returned 79 candidates and stopped - too few
             *  for the run to mean anything, whatever the matching threshold. At 60x
             *  the same bundle gave 535 in 23 s. 40 is the compromise; easy bundles
             *  reach their target long before it and pay nothing. */
            size_t max_seeds_factor = 40;

            vector<Assignment> assignments;

            // Worker thread state; see the equivalent pattern in
            // roi_editor/roi.cpp's grow_cut_slot().
            std::thread worker;
            TrackGenState state;
            vector<MR::DWI::Tractography::Streamline<float>> results;
            MR::DWI::Tractography::Properties result_properties;
            QTimer* poll_timer;
            std::string pending_name;
            //! Whether this run's tract needs a distinct solid colour.
            /*! Decided when Generate is pressed, i.e. before the live preview is
             *  listed under the same name - otherwise the name test would match
             *  the run's own preview and every first run would come out solid. */
            bool pending_solid_colour = false;
            //! Streamlines generated before recognition; 0 in manual mode.
            uint64_t bundle_generated = 0;
            //! Matching distance the last auto-track run actually applied.
            float bundle_effective_distance = 0.0f;
            //! Candidates the last run's distance metric threw away.
            /*! Kept so they can be listed as a tract of their own: seeing what the
             *  threshold removed is the only way to tell a threshold that is too
             *  strict from a bundle that genuinely is not there. */
            vector<MR::DWI::Tractography::Streamline<float>> rejected_results;
            QPushButton* show_rejected_button;

            //! One bundle of a batch, with the settings that belong to that bundle.
            /*! Matching thresholds are per bundle (suggest_thresholds_for_bundle and
             *  bundle_overrides), so a batch has to carry each bundle's own rather
             *  than whatever the panel happened to be showing when Generate was
             *  pressed. */
            struct BundleJob { NOMEMALIGN
              std::string name, bundle_path;
              AutotrackParams params;
            };
            vector<BundleJob> jobs;
            //! Index of the job the worker is on, for the progress bar.
            std::atomic<size_t> jobs_started { 0 }, jobs_done { 0 };

            //! A finished bundle, waiting to be listed on the GUI thread.
            /*! The worker cannot touch the Tractography tool, so results are queued
             *  here and drained by poll_progress_slot - which means a tract appears as
             *  soon as its bundle is done rather than when the whole batch is. */
            struct FinishedBundle { NOMEMALIGN
              std::string name;
              vector<MR::DWI::Tractography::Streamline<float>> tracks, rejected, candidates;
              std::map<std::string, std::string> properties;
              uint64_t generated = 0;
              float effective_distance = 0.0f;
              std::string error;
              MR::DWI::Tractography::Recognition::RefineReport report;
              AutotrackParams params;
            };
            std::mutex finished_mutex;
            vector<FinishedBundle> finished;
            //! List whatever the worker has finished; safe to call at any time.
            void drain_finished ();
            //! Bundles listed so far in this batch, and how many streamlines in all.
            size_t batch_listed = 0, batch_streamlines = 0;

            //! Everything the last auto-track run produced, before matching.
            /*! Matching is milliseconds and tracking is minutes, so changing only the
             *  shape-distance settings should not have to track again. Kept with the
             *  settings that produced them, so it is checkable that nothing else
             *  changed in the meantime. */
            struct LastRun { NOMEMALIGN
              std::string source_path, bundle_data, bundle_path, name;
              AutotrackParams params;
              vector<MR::DWI::Tractography::Streamline<float>> candidates;
              bool solid_colour = false;
              bool valid () const { return candidates.size() && bundle_path.size(); }
            };
            //! Per bundle, so a re-match works on any of a batch's results.
            std::map<std::string, LastRun> last_runs;
            LastRun* last_run_for (const std::string& name);
            //! True if only the matching settings differ, so a re-match is equivalent.
            static bool same_tracking_setup (const AutotrackParams&, const AutotrackParams&);
            //! Re-filter one bundle's cached candidates; false if there are none.
            /*! Only reachable from the Generate button now, which is the one caller
             *  for which the panel and the bundle are certainly the same thing. */
            bool rematch_cached_candidates (const std::string& name,
                                            bool use_panel_thresholds = false);

            // --- Recognition: settings for the next run ---
            // Post-hoc adjustment of a finished tract is a property of that tract and
            // lives in the Tracts list (Tractography's Refine section), so nothing
            // here acts on a result - it only decides what the next run produces.
            QCheckBox* competitive_box;
            QComboBox* prune_combo;
            QSpinBox* strictness_spin;
            //! Read-only: competitors are chosen per bundle, so there is no one list.
            QLabel* neighbours_label;
            void update_neighbours_label ();
            //! Refine options as the Recognition controls currently read.
            MR::DWI::Tractography::Recognition::RefineOptions collect_refine_options () const;
            //! Load the competitor bundles for \a name, on the GUI thread.
            /*! The atlas cache belongs to the GUI thread and evicts under pressure,
             *  so the worker gets copies rather than references. */
            //! Point \a params at the population map for \a name, if the atlas has one.
            /*! Coverage is partial - the HCP1065 release has 67 maps against 102
             *  bundles - so this quietly leaves the fields empty when there is none,
             *  and the run falls back on the shape distance alone. */
            void attach_population_map (AutotrackParams& params, const std::string& name);

            vector<MR::DWI::Tractography::Recognition::Competitor> collect_competitors (
                const std::string& name);
            //! Kick off the neighbour index in the background, once.
            void ensure_neighbour_index ();
            std::thread footprint_worker;
            std::atomic<bool> footprint_running { false };

            // Built-in atlas bundles, aligned by Window::atlas_registration().
            std::unique_ptr<QTemporaryDir> builtin_dir;
            std::map<std::string, std::string> builtin_cache;
            vector<std::string> user_bundle_paths;
            //! Live preview of the streamlines accepted so far.
            void update_preview (uint64_t selected);
            //! Take the preview back out of the Tracts list, for a run that failed
            //  or whose candidates all missed the bundle.
            void discard_preview ();
            QCheckBox* preview_box;
            Tractogram* preview_tractogram = nullptr;
            uint64_t preview_count = 0;
            MR::Timer preview_clock;
            //! Wall-clock of the current run, shown while it runs and when it ends.
            MR::Timer run_clock;

            //! Peak images derived from an FOD for FACT, keyed by FOD path.
            std::unique_ptr<QTemporaryDir> peaks_dir;
            std::map<std::string, std::string> peaks_cache;
            void (*saved_exception_display) (const MR::Exception&, int) = nullptr;
            void (*saved_report_to_user) (const std::string&, int) = nullptr;
            //! Run counter, so manual tracts are named tract_1, tract_2, ...
            size_t tract_iteration = 0;
            std::string result_error;
            // ProgressBar's display hooks are process-wide globals that mrview
            // points at Qt widgets; they are muted while the worker runs.
            void (*saved_progress_display) (const MR::ProgressBar&) = nullptr;
            void (*saved_progress_done) (const MR::ProgressBar&) = nullptr;

            //! Rebuild the bundle list, keeping user-loaded and built-in entries apart.
            void refresh_bundle_combo ();
            //! Write a built-in (already aligned) bundle to a file autotrack can open.
            /*! The tracking and recognition code paths all take a path, so rather
             *  than threading in-memory streamlines through them, the selected
             *  bundle is written once to a temporary file and cached. */
            std::string materialise_builtin_bundle (const std::string& name);

            //! Engine index of the selected algorithm (not the combo row).
            int current_algorithm () const;

            //! Make sure an FOD source is available, asking for one if it is not.
            /*! With nothing loaded, opens the ODF display tool and its file
             *  chooser rather than telling the user to go and do that. Returns
             *  false if they cancelled, i.e. there is still no source. */
            bool ensure_fod_source ();
            //! Open the ODF tool's file chooser and load an FOD from it.
            /*! Returns true if a new source was loaded. The ODF tool is raised to
             *  make it clear where the image went, then this panel is raised back:
             *  the ODF tool was opened to load from, not to work in. */
            bool load_fod_source ();
            //! Raise the dock this panel lives in, whichever tool now hosts it.
            void raise_host_panel ();
            //! Size the matching thresholds to the selected bundle.
            /*! A sparsely sampled bundle needs a looser threshold than a densely
             *  sampled one; see suggest_bundle_thresholds(). */
            void suggest_thresholds_for_bundle ();
            float suggested_distance = 0.0f, suggested_qb = 0.0f, suggested_dilate = 0.0f;
            //! Values the user typed, per bundle, so an edit does not follow them
            //  to every other bundle.
            struct BundleParams { NOMEMALIGN
              float distance = NaN, qb = NaN, dilate = NaN;
            };
            std::map<std::string, BundleParams> bundle_overrides;
            bool applying_suggestion = false;

            //! Default matching distance for a recognition metric, in mm.
            static float metric_default_distance (int metric_index);
            //! Multiplier on the suggested distance for a tracking algorithm.
            static float algorithm_distance_factor (int algorithm);

            //! Path of the first ticked atlas bundle; empty means manual mode.
            /*! Kept as a single value for the session and command-line paths, which
             *  name one bundle; a run uses selected_bundle_names() instead. */
            std::string current_bundle () const;
            //! Resolve a combo entry's data to a bundle file on disk.
            std::string bundle_path_for (const std::string& data) const;
            //! Grid for a region derived here: the FOD's if set, else the viewed image.
            MR::Header region_grid () const;
            class BundleRegions;
            std::unique_ptr<BundleRegions> bundle_regions;
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
