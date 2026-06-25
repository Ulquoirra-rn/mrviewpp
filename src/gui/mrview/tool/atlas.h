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

        // Atlas tool: load an integer label volume + a lookup table (LUT), colour
        // each region with its LUT colour, and read out the region name under the
        // crosshair. A region list allows jumping to a region's centroid.
        //
        // Coloured display reuses mrview's existing RGB-volume slice renderer: the
        // label image is baked into a scratch RGB volume (one colour per label) at
        // load time, so no new slice shader is needed. The original label image and
        // LUT are retained for name lookup at the focus point.
        class Atlas : public Base
        { MEMALIGN(Atlas)
            Q_OBJECT

          public:

            class Item;
            class Model;

            Atlas (Dock* parent);

            void draw (const Projection& projection, bool is_3D, int axis, int slice) override;

          private slots:
            void atlas_open_slot ();
            void atlas_close_slot ();
            void hide_all_slot ();
            void toggle_shown_slot (const QModelIndex&, const QModelIndex&);
            void selection_changed_slot (const QItemSelection&, const QItemSelection&);
            void opacity_slot (int);
            void focus_changed_slot ();
            void region_activated_slot (QListWidgetItem*);

          protected:
            Model* atlas_list_model;
            QListView* atlas_list_view;
            QPushButton* hide_all_button;
            QSlider* opacity_slider;
            QLabel* region_label;
            QListWidget* region_list;

            Item* current_item ();
            void populate_region_list ();
            void dropEvent (QDropEvent* event) override;
        };

      }
    }
  }
}

#endif
