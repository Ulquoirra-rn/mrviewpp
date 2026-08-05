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

#ifndef __gui_mrview_tool_atlas_h__
#define __gui_mrview_tool_atlas_h__

#include "gui/mrview/tool/base.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        // Atlas tool: load an integer label volume + a lookup table (LUT), give
        // every region its own flat LUT colour, and read out the region name under
        // the crosshair. A region list allows jumping to a region's centroid.
        //
        // Rendering does not reuse mrview's intensity-windowed colourmaps (those
        // interpolate between labels and produce garbage at region boundaries).
        // Instead the label volume is converted to *compact region indices*,
        // uploaded as a nearest-neighbour-sampled R32F 3D texture, and a dedicated
        // shader looks each index up in a small RGB palette texture. That keeps
        // every region a solid colour and lets the shader give the crosshair and
        // hovered regions full opacity while the rest are dimmed, which is what the
        // niivue atlas view does.
        class Atlas : public Base
        { MEMALIGN(Atlas)
            Q_OBJECT

          public:

            class Item;
            class Model;
            class Shader;
            class Regions;

            Atlas (Dock* parent);
            ~Atlas ();

            void draw (const Projection& projection, bool is_3D, int axis, int slice) override;

            RegionProvider* region_provider () override;

            static void add_commandline_options (MR::App::OptionList& options);
            bool process_commandline_option (const MR::App::ParsedOption& opt) override;

            std::string session_key () const override { return "atlases"; }
            void get_session (nlohmann::json&) const override;
            void set_session (const nlohmann::json&) override;

          private slots:
            void atlas_open_slot ();
            void atlas_close_slot ();
            void hide_all_slot ();
            void toggle_shown_slot (const QModelIndex&, const QModelIndex&);
            void selection_changed_slot (const QItemSelection&, const QItemSelection&);
            void opacity_slot (int);
            void dim_slot (int);
            void focus_changed_slot ();
            void hover_changed_slot ();
            void region_activated_slot (QListWidgetItem*);
            void region_highlight_slot (QListWidgetItem*, QListWidgetItem*);

          protected:
            Model* atlas_list_model;
            QListView* atlas_list_view;
            QPushButton* hide_all_button;
            QSlider* opacity_slider;
            QSlider* dim_slider;
            QLabel* focus_region_label;
            QLabel* hover_region_label;
            QListWidget* region_list;
            bool syncing_region_list;
            std::unique_ptr<Regions> regions;

            Item* current_item ();
            void populate_region_list ();
            // Both the crosshair region and the region under the mouse are drawn
            // fully opaque; the crosshair one persists until the focus moves.
            void set_focus_region (size_t index);
            void set_hover_region (size_t index);
            void select_in_region_list (size_t index);
            QString describe_region (size_t index);
            void update_region_labels ();
            void dropEvent (QDropEvent* event) override;
        };

      }
    }
  }
}

#endif
