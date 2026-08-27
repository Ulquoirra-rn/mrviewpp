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

#ifndef __gui_mrview_tool_tractography_h__
#define __gui_mrview_tool_tractography_h__

#include <QTemporaryDir>

#include "dwi/tractography/properties.h"
#include "dwi/tractography/streamline.h"

#include <set>

#include "gui/mrview/tool/base.h"
#include "gui/color_button.h"
#include "gui/projection.h"
#include "gui/mrview/adjust_button.h"
#include "gui/mrview/combo_box_error.h"
#include "gui/mrview/tool/tractography/track_scalar_file.h"

namespace MR
{
  namespace GUI
  {
    namespace GL {
      class Lighting;
    }

    class LightingDock;

    namespace MRView
    {
      namespace Tool
      {
        // Hosted inside the Tracts panel; only ever held by pointer here, so the
        // full definition stays in tractography.cpp and out of tractogram.h, which
        // includes this file.


        extern const char* tractogram_geometry_types[];

        class Tractography : public Base
        { MEMALIGN(Tractography)
            Q_OBJECT

          public:

            class Model;

            Tractography (Dock* parent);

            virtual ~Tractography ();

            void draw (const Projection& transform, bool is_3D, int axis, int slice) override;
            void draw_colourbars () override;
            size_t visible_number_colourbars () override;
            bool crop_to_slab () const { return (do_crop_to_slab && not_3D); }

            static void add_commandline_options (MR::App::OptionList& options);
            virtual bool process_commandline_option (const MR::App::ParsedOption& opt) override;

            //! Re-read the selected tract into the panel's controls.
            /*! The generator stamps a tract's provenance *after* handing it over, and
             *  the panel had already decided from a tract that did not have one yet
             *  that there was nothing to refine. Called once the hand-over is
             *  complete, so the Refine group is live as soon as the tract is listed
             *  rather than after the next click on the list. */
            void refresh_tract_controls ();

            // Adopt streamlines generated in-process (Track generation tool).
            // Model is only visible inside tractography.cpp, so this is the seam.
            Tractogram* add_tractogram_from_memory (
                const vector<MR::DWI::Tractography::Streamline<float>>&,
                const MR::DWI::Tractography::Properties&,
                const std::string& display_name,
                uint64_t total_attempted = 0,
                //! Give it a distinct solid colour rather than directional colouring.
                /*! Wanted when several related tractograms need telling apart (one
                 *  per atlas bundle, a split-off selection); not wanted for a plain
                 *  tracking run, where directional colouring is more informative. */
                bool solid_colour = true);

            //! Remove a tractogram this tool owns, whoever added it.
            /*! Used by the View tool when an atlas bundle is unticked. Silently
             *  ignores a tractogram that is not in the list, since the user may
             *  have closed it by hand in the meantime. */
            void remove_tractogram (Tractogram* tractogram);

            //! Adopt a tract atlas bundle: rendered, but not listed.
            /*! The built-in atlas is browsed from the View tool, so its bundles must
             *  not clutter this tool's list - but something has to draw them, and
             *  this is where the GL context and the render loop live. They are drawn
             *  exactly like listed tractograms and are subject to "hide all". */
            Tractogram* add_atlas_bundle (
                const vector<MR::DWI::Tractography::Streamline<float>>&,
                const MR::DWI::Tractography::Properties&,
                const std::string& display_name);
            void remove_atlas_bundle (Tractogram*);
            //! Give a listed tractogram the next distinct solid colour.
            void apply_distinct_colour (Tractogram*);

            //! Colour a bundle: directional when \a colour is invalid, else solid.
            void set_atlas_bundle_colour (Tractogram*, const QColor& colour);

            //! True if this tool still holds \a tractogram.
            /*! Only ever compares pointer values, so it is safe to ask about one
             *  the user has since closed. */
            bool contains (const Tractogram* tractogram) const;

            //! True if a listed tractogram is already showing under this name.
            /*! Track generation uses this to decide colouring: directional is the
             *  most informative, but two runs of the same name are indistinguishable
             *  that way, so the second gets a distinct solid colour. */
            bool has_tractogram_named (const std::string& name) const;
            //! The listed tract with this display name, or null.
            /*! So a caller re-running a filter can update the tract in place instead
             *  of listing a second copy of it - which is what the strictness slider
             *  does on every drag. */
            Tractogram* find_tractogram_named (const std::string& name);
            //! Make this tract the list selection; false if it is not listed.
            /*! So another tool can hand work over to the actions on this one rather
             *  than keeping its own copy of them. */
            bool select_tractogram_named (const std::string& name);
            //! Run "Cluster into bundles..." on the current selection.
            void cluster_selected ();
            //! Turn on editing for the current selection and raise this tool.
            void begin_manual_editing ();

            //! Atlas bundles this tool renders on the View tool's behalf.
            /*! Not in the list model, so they never appear in the tract list, but
             *  drawn by the same loop and hidden by the same "hide all". */
            vector<std::unique_ptr<Tractogram>> atlas_bundles;

            //! The hosted generator's atlas bundles, offered as tracking regions.
            /*! Forwarded because registering a tool is what normally wires this up,
             *  and the generator is no longer registered. Tractography has no regions
             *  of its own, so nothing is displaced. */

            std::string session_key () const override { return "tracts"; }
            void get_session (nlohmann::json&) const override;
            void set_session (const nlohmann::json&) override;

            QPushButton* hide_all_button;
            bool do_crop_to_slab;
            bool use_lighting;
            bool use_threshold_scalarfile;
            bool not_3D;
            //! Spill files this session has written, for cleaning up after itself.
            /*! Only these are ever deleted. A previous session's files are left
             *  alone until they are restored, and so adopted, by this one. */
            mutable std::set<std::string> session_spill_files;
            float slab_thickness;
            //! Set by hand, so it no longer follows the main image's voxel size.
            bool slab_thickness_user_set = false;
            float default_slab_thickness () const;
            float line_opacity;
            Model* tractogram_list_model;
            QListView* tractogram_list_view;

            GL::Lighting* lighting;


          private slots:
            void tractogram_open_slot ();
            void tractogram_close_slot ();
            void tractogram_export_slot ();
            void toggle_shown_slot (const QModelIndex&, const QModelIndex&);
            void hide_all_slot ();
            void on_slab_thickness_slot ();
            void main_image_changed_slot ();
            void on_crop_to_slab_slot (bool is_checked);
            void on_use_lighting_slot (bool is_checked);
            void on_lighting_settings ();
            void opacity_slot (int opacity);
            void line_thickness_slot (int thickness);
            void right_click_menu_slot (const QPoint& pos);
            void colour_track_by_direction_slot ();
            void colour_track_by_ends_slot ();
            void randomise_track_colour_slot ();
            void set_track_colour_slot ();
            void colour_by_scalar_file_slot ();
            void endpoints_overlay_slot ();
            void colour_mode_selection_slot (int);
            void colour_button_slot();
            void geom_type_selection_slot (int);
            void selection_changed_slot (const QItemSelection &, const QItemSelection &);
            void edit_enable_slot (bool);
            void select_by_region_slot ();
            void invert_selection_slot ();
            void select_all_streamlines_slot ();
            void keep_selection_slot ();
            void delete_selection_slot ();
            void split_selection_slot ();
            void split_rejected_slot ();
            void cluster_tracts_slot ();
            void refine_tracts_slot ();
            void refine_setting_changed ();
            void strictness_moved_slot (int);
            void apply_refine_slot ();
            void refine_neighbours_slot ();
            void refine_revert_slot ();
            void combine_tracts_slot ();
            void clear_rules_slot ();
            void regions_changed_slot ();
            void reapply_rules_slot ();
            void statistics_slot ();
            void profile_slot ();

          protected:
            //! Right-click entry for the rejected set; label and state follow the selection.
            QAction* rejected_action;
            //! Merge entry; needs at least two tracts selected.
            QAction* combine_action;

            //! Turn editing on for \a t if it is not already, reporting failure.
            bool ensure_editing (Tractogram*);

            //! Holds the endpoint maps handed to the Overlay tool.
            /*! They are written to disk rather than built as scratch images: an
             *  overlay reads its data as cfloat, and a scratch buffer of any other
             *  datatype would be reinterpreted rather than converted (see
             *  Image<T>::Buffer::get_data_pointer). A file also gives the overlay a
             *  real name and lets it survive a session save. */
            std::unique_ptr<QTemporaryDir> endpoint_dir;

            //! The generator, hosted here rather than in a dock of its own.

            QCheckBox* edit_enable_box;

            // --- Refine: properties of the selected tract ---
            Section *refine_section, *edit_section;
            QSlider* refine_strictness;
            QLabel *refine_count_label, *refine_status_label;
            QCheckBox* refine_competitive;
            QComboBox* refine_prune;
            QPushButton *refine_neighbours_button, *refine_revert_button;
            QTimer* refine_timer;
            //! True while the controls are being loaded from a tract, so that setting
            //  them does not look like the user changing them.
            bool refine_loading = false;
            // What the slider's current landmark means, and what actually gets
            // applied - the slider itself counts landmarks, not percent.
            int refine_strictness_percent = 0;
            //! Show the selected tract's refinement, or say why there is none.
            void update_refine_controls ();
            //! Re-derive one tract's result from the candidates it already holds.
            void apply_refinement (Tractogram*);
            QPushButton *select_region_button, *invert_button, *select_all_button;
            QPushButton *keep_button, *delete_button, *split_button;
            QLabel* edit_status_label;
            QListWidget* rule_list;
            QPushButton* clear_rules_button;
            // Coalesces bursts of region edits into one re-evaluation.
            QTimer* rule_refresh_timer;

            //! Tractograms currently selected in the list.
            vector<Tractogram*> selected_tractograms ();
            void update_edit_controls ();
            void refresh_rule_list ();
            //! Dim every region acting as "avoids"; restore the rest.
            void apply_region_opacities (const vector<RegionRef>& released = vector<RegionRef>());

            AdjustButton* slab_entry;
            QMenu* track_option_menu;

            ComboBoxWithErrorMsg *colour_combobox;
            QColorButton *colour_button;

            ComboBoxWithErrorMsg *geom_type_combobox;

            QLabel* thickness_label;
            QSlider* thickness_slider;

            TrackScalarFileOptions *scalar_file_options;
            LightingDock *lighting_dock;

            QGroupBox* slab_group_box;
            QGroupBox* lighting_group_box;
            QPushButton* lighting_button;

            QSlider* opacity_slider;

            void dropEvent (QDropEvent* event) override;
            void update_scalar_options();
            void add_tractogram (vector<std::string>& list);
            void select_last_added_tractogram();
            bool process_commandline_option_tsf_check_tracto_loaded ();
            bool process_commandline_option_tsf_option (const MR::App::ParsedOption&, uint, vector<default_type>& range);
            void update_geometry_type_gui();
        };
      }
    }
  }
}

#endif




