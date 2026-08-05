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

#include "dwi/tractography/properties.h"
#include "dwi/tractography/streamline.h"

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

            std::string session_key () const override { return "tracts"; }
            void get_session (nlohmann::json&) const override;
            void set_session (const nlohmann::json&) override;

            QPushButton* hide_all_button;
            bool do_crop_to_slab;
            bool use_lighting;
            bool use_threshold_scalarfile;
            bool not_3D;
            float slab_thickness;
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
            void clear_rules_slot ();
            void regions_changed_slot ();
            void reapply_rules_slot ();
            void statistics_slot ();
            void profile_slot ();

          protected:
            QCheckBox* edit_enable_box;
            QPushButton *select_region_button, *invert_button, *select_all_button;
            QPushButton *keep_button, *delete_button, *split_button;
            QPushButton *stats_button, *profile_button;
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




