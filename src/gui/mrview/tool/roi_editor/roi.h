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

#ifndef __gui_mrview_tool_roi_editor_roi_h__
#define __gui_mrview_tool_roi_editor_roi_h__

#include <atomic>
#include <thread>

#include "memory.h"
#include "transform.h"
#include "types.h"

#include "gui/mrview/mode/base.h"
#include "gui/mrview/tool/base.h"
#include "gui/mrview/mode/slice.h"
#include "gui/color_button.h"
#include "gui/mrview/adjust_button.h"

#include "gui/mrview/tool/roi_editor/item.h"
#include "gui/mrview/tool/roi_editor/model.h"
#include "gui/mrview/tool/roi_editor/undoentry.h"


namespace MR
{

  class Header;

  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {


        class ROI : public Base
        { MEMALIGN(ROI)
            Q_OBJECT

          public:
            ROI (Dock* parent);
            ~ROI();

            void draw (const Projection& projection, bool is_3D, int axis, int slice) override;

#ifndef MRTRIX_WASM
            // Reading an ROI back off the GPU needs glGetTexImage, which WebGL2
            // does not have, so ROIs are not offered as regions in WASM builds.
            RegionProvider* region_provider () override;

            //! Create a new empty ROI on the main image's grid and select it.
            /*! Returns its RegionRef so a caller can use it immediately; throws if
             *  there is no main image to take a grid from. */
            RegionRef create_region ();
#endif

            static void add_commandline_options (MR::App::OptionList& options);
            virtual bool process_commandline_option (const MR::App::ParsedOption& opt) override;

            virtual bool mouse_press_event () override;
            virtual bool mouse_move_event () override;
            virtual bool mouse_release_event () override;
            virtual bool mouse_wheel_event (int delta_x, int delta_y) override;
            virtual QCursor* get_cursor () override;

          private slots:
            void new_slot ();
            void open_slot ();
            void save_slot ();
            void close_slot ();
            void draw_slot ();
            void undo_slot ();
            void redo_slot ();
            void hide_all_slot ();
            void slice_copy_slot (QAction*);
            void select_edit_mode (QAction*);
            void toggle_shown_slot (const QModelIndex&, const QModelIndex&);
            void update_selection ();
            void update_slot ();
            void colour_changed ();
            void opacity_changed (int unused);
            void model_rows_changed ();
            void grow_cut_slot ();
            void region_grow_slot ();

          protected:
             class Regions;
             std::unique_ptr<Regions> regions;

             QPushButton *hide_all_button, *close_button, *save_button;
             QPushButton *grow_cut_button, *region_grow_button;
             QToolButton *draw_button, *undo_button, *redo_button;
             QToolButton *brush_button, *rectangle_button, *fill_button, *grow_mode_button;
             QToolButton *copy_from_above_button, *copy_from_below_button;
             QActionGroup *edit_mode_group, *slice_copy_group;
             ROI_Model* list_model;
             QListView* list_view;
             QColorButton* colour_button;
             QSlider *opacity_slider;
             AdjustButton *brush_size_button;
             AdjustButton *slab_thickness_button;
             AdjustButton *tolerance_button;
             int current_axis, current_slice;
             bool in_insert_mode, insert_mode_value;
             Eigen::Vector3f current_origin, prev_pos;
             float current_slice_loc;

             Mode::Slice::Shader shader;

             int slab_slices (const ROI_Item&, int axis) const;
             void announce_regions_changed ();
             void update_undo_redo ();
             void updateGL() {
               window().get_current_mode()->update_overlays = true;
               window().updateGL();
             }

             void load (vector<std::unique_ptr<MR::Header>>& list);
             void save (ROI_Item*);
             void write_roi_mask (ROI_Item*, const std::string& path);
             void save_label_map (const vector<ROI_Item*>&, const std::string& path);

             // --- background (non-blocking) segmentation ---
             // The heavy grow-cut / region-grow compute runs on seg_thread so the
             // viewer stays usable; GL reads/uploads stay on the GUI thread.
             QProgressBar* seg_progress;
             QLabel* seg_status;
             std::thread seg_thread;
             std::atomic<bool> seg_running { false };
             std::atomic<bool> seg_cancel { false };
             void set_seg_controls_enabled (bool on);
             void seg_show_progress (const QString& msg);
             void seg_finish (const QString& msg);

             // Interactive scroll-driven 2D region-grow state:
             bool grow_active;
             const void* grow_image_id;
             int grow_axis;
             ssize_t grow_slice, grow_seed_u, grow_seed_v;
             float grow_seed_value, grow_tol, grow_range;
             vector<float> grow_intensity;
             vector<GLubyte> grow_base;
             void apply_grow (ROI_Item* roi, bool with_region);

             int normal2axis (const Eigen::Vector3f&, const ROI_Item&) const;

             void dropEvent (QDropEvent* event) override;
        };


      }
    }
  }
}

#endif



