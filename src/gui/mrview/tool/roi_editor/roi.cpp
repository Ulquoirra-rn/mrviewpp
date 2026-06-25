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

#include <string>
#include <limits>

#include "gui/mrview/tool/roi_editor/roi.h"

#include "header.h"
#include "image.h"
#include "datatype.h"
#include "progressbar.h"
#include "gui/cursor.h"
#include "gui/mrview/gui_image.h"
#include "gui/projection.h"
#include "gui/dialog/file.h"


namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {




        ROI::ROI (Dock* parent) :
            Base (parent),
            in_insert_mode (false),
            grow_active (false),
            grow_image_id (nullptr)
        {

          VBoxLayout* main_box = new VBoxLayout (this);
          HBoxLayout* layout = new HBoxLayout;
          layout->setContentsMargins (0, 0, 0, 0);
          layout->setSpacing (0);

          QPushButton* button = new QPushButton (this);
          button->setToolTip (tr ("New ROI"));
          button->setIcon (QIcon (":/new.svg"));
          connect (button, SIGNAL (clicked()), this, SLOT (new_slot ()));
          layout->addWidget (button, 1);

          button = new QPushButton (this);
          button->setToolTip (tr ("Open ROI"));
          button->setIcon (QIcon (":/open.svg"));
          connect (button, SIGNAL (clicked()), this, SLOT (open_slot ()));
          layout->addWidget (button, 1);

          save_button = new QPushButton (this);
          save_button->setToolTip (tr ("Save ROI"));
          save_button->setIcon (QIcon (":/save.svg"));
          save_button->setEnabled (false);
          connect (save_button, SIGNAL (clicked()), this, SLOT (save_slot ()));
          layout->addWidget (save_button, 1);

          close_button = new QPushButton (this);
          close_button->setToolTip (tr ("Close ROI"));
          close_button->setIcon (QIcon (":/close.svg"));
          close_button->setEnabled (false);
          connect (close_button, SIGNAL (clicked()), this, SLOT (close_slot ()));
          layout->addWidget (close_button, 1);

          hide_all_button = new QPushButton (this);
          hide_all_button->setToolTip (tr ("Hide all ROIs"));
          hide_all_button->setIcon (QIcon (":/hide.svg"));
          hide_all_button->setCheckable (true);
          connect (hide_all_button, SIGNAL (clicked()), this, SLOT (hide_all_slot ()));
          layout->addWidget (hide_all_button, 1);

          main_box->addLayout (layout, 0);

          list_view = new QListView (this);
          list_view->setSelectionMode (QAbstractItemView::SingleSelection);
          list_view->setHorizontalScrollBarPolicy (Qt::ScrollBarAlwaysOff);
          list_view->setTextElideMode (Qt::ElideLeft);
          list_view->setDragEnabled (true);
          list_view->setDragDropMode (QAbstractItemView::InternalMove);
          list_view->setAcceptDrops (true);
          list_view->viewport()->setAcceptDrops (true);
          list_view->setDropIndicatorShown (true);

          list_model = new ROI_Model (this);
          list_view->setModel (list_model);
          connect (list_model, SIGNAL (rowsInserted(const QModelIndex&, int, int)), this, SLOT (model_rows_changed ()));

          main_box->addWidget (list_view, 1);

          QPushButton* grow_cut_button = new QPushButton (tr ("Grow-cut from ROIs"), this);
          grow_cut_button->setToolTip (tr ("Use the painted ROIs as seeds (one region per ROI) and grow-cut "
                                           "segment the current image; results are added as new ROIs"));
          connect (grow_cut_button, SIGNAL (clicked()), this, SLOT (grow_cut_slot ()));
          main_box->addWidget (grow_cut_button, 0);

          HBoxLayout* region_grow_layout = new HBoxLayout;
          QPushButton* region_grow_button = new QPushButton (tr ("Region grow (3D)"), this);
          region_grow_button->setToolTip (tr ("Grow the selected ROI seed in 3D to all connected voxels whose "
                                              "intensity is within the tolerance of the seed's mean intensity"));
          connect (region_grow_button, SIGNAL (clicked()), this, SLOT (region_grow_slot ()));
          region_grow_layout->addWidget (region_grow_button, 1);
          region_grow_layout->addWidget (new QLabel (tr ("tol %")), 0);
          tolerance_button = new AdjustButton (this);
          tolerance_button->setToolTip (tr ("Region-grow intensity tolerance, as a percentage of the image intensity range"));
          tolerance_button->setMin (0.0f);
          tolerance_button->setMax (100.0f);
          tolerance_button->setRate (0.5f);
          tolerance_button->setValue (10.0f);
          region_grow_layout->addWidget (tolerance_button, 0);
          main_box->addLayout (region_grow_layout, 0);

          GridLayout* grid_layout = new GridLayout;

          draw_button = new QToolButton (this);
          draw_button->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
          QAction* action = new QAction (QIcon (":/draw.svg"), tr ("Edit"), this);
          action->setShortcut (tr ("E"));
          action->setToolTip (tr ("Add/remove voxels to/from ROI\n\nUse left mouse button to add voxels,\nright mouse button to erase"));
          action->setCheckable (true);
          action->setEnabled (false);
          connect (action, SIGNAL (toggled(bool)), this, SLOT (draw_slot ()));
          draw_button->setDefaultAction (action);
          grid_layout->addWidget (draw_button, 0, 0, Qt::AlignCenter);

          undo_button = new QToolButton (this);
          undo_button->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
          action = new QAction (QIcon (":/undo.svg"), tr ("Undo"), this);
          action->setShortcut (tr ("Ctrl+Z"));
          action->setToolTip (tr ("Undo last edit"));
          action->setCheckable (false);
          action->setEnabled (false);
          connect (action, SIGNAL (triggered()), this, SLOT (undo_slot ()));
          undo_button->setDefaultAction (action);
          grid_layout->addWidget (undo_button, 0, 1, Qt::AlignRight);

          redo_button = new QToolButton (this);
          redo_button->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
          action = new QAction (QIcon (":/redo.svg"), tr ("Redo"), this);
          action->setShortcut (tr ("Ctrl+Y"));
          action->setToolTip (tr ("Redo last edit"));
          action->setCheckable (false);
          action->setEnabled (false);
          connect (action, SIGNAL (triggered()), this, SLOT (redo_slot ()));
          redo_button->setDefaultAction (action);
          grid_layout->addWidget (redo_button, 0, 2, Qt::AlignLeft);

          main_box->addLayout (grid_layout, 0);

          QGroupBox* group_box = new QGroupBox ("Edit mode");

          grid_layout = new GridLayout;
          group_box->setLayout (grid_layout);

          edit_mode_group = new QActionGroup (this);
          edit_mode_group->setExclusive (true);
          edit_mode_group->setEnabled (false);
          connect (edit_mode_group, SIGNAL (triggered (QAction*)), this, SLOT (select_edit_mode (QAction*)));

          brush_button = new QToolButton (this);
          brush_button->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
          brush_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
          action = new QAction (QIcon (":/brush.svg"), tr ("Brush"), this);
          action->setShortcut (tr ("Ctrl+B"));
          action->setToolTip (tr ("Edit ROI using a brush"));
          action->setCheckable (true);
          action->setChecked (true);
          edit_mode_group->addAction (action);
          brush_button->setDefaultAction (action);
          grid_layout->addWidget (brush_button, 0, 0, 1, 2);

          QLabel* label = new QLabel (tr("size:"));
          grid_layout->addWidget (label, 1, 0, Qt::AlignRight);

          brush_size_button = new AdjustButton (this);
          brush_size_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
          brush_size_button->setToolTip (tr ("Brush size (in mm)"));
          brush_size_button->setEnabled (true);
          grid_layout->addWidget (brush_size_button, 1, 1);

          fill_button = new QToolButton (this);
          fill_button->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
          fill_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
          action = new QAction (QIcon (":/fill.svg"), tr ("Fill"), this);
          action->setShortcut (tr ("Ctrl+F"));
          action->setToolTip (tr ("Fill ROI slice"));
          action->setCheckable (true);
          action->setChecked (false);
          edit_mode_group->addAction (action);
          fill_button->setDefaultAction (action);
          grid_layout->addWidget (fill_button, 0, 2, 1, 2);

          rectangle_button = new QToolButton (this);
          rectangle_button->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
          rectangle_button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
          action = new QAction (QIcon (":/rectangle.svg"), tr ("Rectangle"), this);
          action->setShortcut (tr ("Ctrl+R"));
          action->setToolTip (tr ("Edit ROI using a rectangle"));
          action->setCheckable (true);
          action->setChecked (false);
          edit_mode_group->addAction (action);
          rectangle_button->setDefaultAction (action);
          grid_layout->addWidget (rectangle_button, 1, 2, 1, 2);

          grow_mode_button = new QToolButton (this);
          grow_mode_button->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
          grow_mode_button->setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Preferred);
          action = new QAction (QIcon (":/fill.svg"), tr ("Region grow"), this);
          action->setToolTip (tr ("Scroll over the image to grow a 2D region from the voxel under the cursor "
                                  "(scroll = adjust tolerance); left-click to commit it into the selected ROI"));
          action->setCheckable (true);
          action->setChecked (false);
          edit_mode_group->addAction (action);
          grow_mode_button->setDefaultAction (action);
          grid_layout->addWidget (grow_mode_button, 2, 0, 1, 4);

          main_box->addWidget (group_box, 0);

          layout = new HBoxLayout;
          layout->setContentsMargins (0, 0, 0, 0);
          layout->setSpacing (0);

          slice_copy_group = new QActionGroup (this);
          slice_copy_group->setEnabled (false);
          connect (slice_copy_group, SIGNAL (triggered (QAction*)), this, SLOT (slice_copy_slot (QAction*)));

          layout->addWidget (new QLabel ("Copy from slice: "), 1, Qt::AlignRight);

          copy_from_above_button = new QToolButton (this);
          copy_from_above_button->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
          action = new QAction (QIcon (":/copy_from_above.svg"), tr ("Above"), this);
          action->setToolTip (tr ("Copy data from the slice above into this slice"));
          action->setCheckable (false);
          action->setChecked (false);
          slice_copy_group->addAction (action);
          copy_from_above_button->setDefaultAction (action);
          layout->addWidget (copy_from_above_button, 1, Qt::AlignRight);

          copy_from_below_button = new QToolButton (this);
          copy_from_below_button->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
          action = new QAction (QIcon (":/copy_from_below.svg"), tr ("Below"), this);
          action->setToolTip (tr ("Copy data from the slice below into this slice"));
          action->setCheckable (false);
          action->setChecked (false);
          slice_copy_group->addAction (action);
          copy_from_below_button->setDefaultAction (action);
          layout->addWidget (copy_from_below_button, 1, Qt::AlignLeft);

          main_box->addLayout (layout, 0);

          layout = new HBoxLayout;
          layout->setContentsMargins (0, 0, 0, 0);
          layout->setSpacing (0);

          colour_button = new QColorButton;
          colour_button->setEnabled (false);
          connect (colour_button, SIGNAL (clicked()), this, SLOT (colour_changed()));
          layout->addWidget (colour_button, 0);

          opacity_slider = new QSlider (Qt::Horizontal);
          opacity_slider->setToolTip (tr("ROI opacity"));
          opacity_slider->setRange (1,1000);
          opacity_slider->setSliderPosition (int (1000));
          connect (opacity_slider, SIGNAL (valueChanged (int)), this, SLOT (opacity_changed(int)));
          opacity_slider->setEnabled (false);
          layout->addWidget (opacity_slider, 1);

          main_box->addLayout (layout, 0);

          connect (list_view->selectionModel(),
              SIGNAL(selectionChanged(const QItemSelection &, const QItemSelection &)),
              SLOT (update_selection()));

          connect (&window(), SIGNAL (imageChanged()), this, SLOT (update_selection()));

          connect (list_model, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
              this, SLOT (toggle_shown_slot (const QModelIndex&, const QModelIndex&)));

          HBoxLayout* checkall_layout = new HBoxLayout;
          QPushButton* check_all_button = new QPushButton (tr ("Check all"), this);
          check_all_button->setToolTip (tr ("Show every ROI by checking its box"));
          connect (check_all_button, &QPushButton::clicked, this, [this]{ list_model->check_all(); updateGL(); });
          checkall_layout->addWidget (check_all_button, 1);
          QPushButton* uncheck_all_button = new QPushButton (tr ("Uncheck all"), this);
          uncheck_all_button->setToolTip (tr ("Hide every ROI by unchecking its box"));
          connect (uncheck_all_button, &QPushButton::clicked, this, [this]{ list_model->uncheck_all(); updateGL(); });
          checkall_layout->addWidget (uncheck_all_button, 1);
          main_box->addLayout (checkall_layout, 0);

          update_selection();
        }






        ROI::~ROI()
        {
          for (int i = 0; i != list_model->rowCount(); ++i) {
            QModelIndex index = list_model->index (i, 0);
            ROI_Item* roi = list_model->get (index);
            if (!roi->saved) {
              if (QMessageBox::question (&window(), tr("ROI not saved"),
                    qstr ("Image " + roi->get_filename() + " has been modified. Do you want to save it?"),
                    QMessageBox::Yes|QMessageBox::No) == QMessageBox::Yes)
                save (roi);
            }
          }
        }





        void ROI::new_slot ()
        {
          assert (window().image());
          MR::Header H (window().image()->header());
          list_model->create (std::move (H));
          list_view->selectionModel()->clear();
          list_view->selectionModel()->select (list_model->index (list_model->rowCount()-1, 0, QModelIndex()), QItemSelectionModel::Select);
          updateGL ();
          in_insert_mode = false;
        }







        void ROI::open_slot ()
        {
          vector<std::string> names = Dialog::File::get_images (this, "Select ROI images to open", &current_folder);
          if (names.empty())
            return;
          vector<std::unique_ptr<MR::Header>> list;
          for (size_t n = 0; n < names.size(); ++n)
            list.push_back (make_unique<MR::Header> (MR::Header::open (names[n])));

          load (list);
          in_insert_mode = false;
        }





        void ROI::dropEvent (QDropEvent* event)
        {
          static constexpr int max_files = 32;

          const QMimeData* mimeData = event->mimeData();
          if (mimeData->hasUrls()) {
            vector<std::unique_ptr<MR::Header>> list;
            QList<QUrl> urlList = mimeData->urls();
            for (int i = 0; i < urlList.size() && i < max_files; ++i) {
              try {
                list.push_back (make_unique<MR::Header> (MR::Header::open (urlList.at (i).path().toUtf8().constData())));
              }
              catch (Exception& e) {
                e.display();
              }
            }
            if (list.size()) {
                load (list);
                in_insert_mode = false;
            }
            event->acceptProposedAction();
          }
        }





        void ROI::save (ROI_Item* roi)
        {
          vector<GLubyte> data (roi->header().size(0) * roi->header().size(1) * roi->header().size(2));
          {
            GL::Context::Grab context;
            GL::assert_context_is_current();
            roi->texture().bind();
            gl::PixelStorei (gl::PACK_ALIGNMENT, 1);
            gl::GetTexImage (gl::TEXTURE_3D, 0, gl::RED_INTEGER, gl::UNSIGNED_BYTE, (void*) (&data[0]));
            GL::assert_context_is_current();
          }

          try {
            MR::Header header (roi->header());
            header.ndim() = 3;
            header.datatype() = DataType::Bit;
            std::string name = GUI::Dialog::File::get_save_image_name (&window(), "Select name of ROI to save", roi->get_filename(), &current_folder);
            if (name.size()) {
              auto out = MR::Image<bool>::create (name, header);
              roi->save (out, data.data());
            }
          }
          catch (Exception& E) {
            E.display();
          }
          in_insert_mode = false;
        }



        // Grow-cut segmentation (Vezhnevets & Konouchine, 2005) driven from the
        // painted ROIs: each ROI becomes one seed region, the current image is the
        // intensity, and the grown regions are added back as new ROIs. Same
        // cellular-automaton as the mrgrowcut command.
        void ROI::grow_cut_slot ()
        {
          if (!window().image()) {
            QMessageBox::warning (this, "Grow-cut", "Load an image first to use as the intensity for grow-cut.");
            return;
          }

          // Gather the painted ROIs as seed regions.
          vector<ROI_Item*> seeds;
          for (size_t i = 0; i < list_model->items.size(); ++i) {
            ROI_Item* r = dynamic_cast<ROI_Item*> (list_model->items[i].get());
            if (r)
              seeds.push_back (r);
          }
          if (seeds.size() < 2) {
            QMessageBox::warning (this, "Grow-cut",
                "Paint at least two ROIs first — one per region to segment (e.g. one foreground, one background).");
            return;
          }

          // Working grid = the current image.
          MR::Image<cfloat> in (window().image()->image);
          const ssize_t nx = in.size(0), ny = in.size(1), nz = in.size(2);
          const size_t N = size_t(nx) * ny * nz;
          auto idx = [&] (ssize_t x, ssize_t y, ssize_t z) { return size_t(x) + nx * (size_t(y) + ny * z); };

          // Read intensities (first volume) into a flat buffer + find the range.
          vector<float> intensity (N);
          float vmin = std::numeric_limits<float>::infinity();
          float vmax = -std::numeric_limits<float>::infinity();
          if (in.ndim() > 3) in.index(3) = 0;
          for (ssize_t z = 0; z != nz; ++z) { in.index(2) = z;
            for (ssize_t y = 0; y != ny; ++y) { in.index(1) = y;
              for (ssize_t x = 0; x != nx; ++x) { in.index(0) = x;
                const cfloat cv = in.value();
                const float v = cv.real();
                intensity[idx(x,y,z)] = v;
                if (std::isfinite (v)) { vmin = std::min (vmin, v); vmax = std::max (vmax, v); }
              }
            }
          }
          const float range = (vmax > vmin) ? (vmax - vmin) : 1.0f;

          // Read each ROI texture and assign its label to painted voxels.
          vector<uint32_t> label (N, 0);
          vector<float> strength (N, 0.0f);
          size_t mismatched = 0;
          {
            GL::Context::Grab context;
            GL::assert_context_is_current();
            for (size_t s = 0; s < seeds.size(); ++s) {
              ROI_Item* r = seeds[s];
              if (r->header().size(0) != nx || r->header().size(1) != ny || r->header().size(2) != nz) {
                ++mismatched;
                continue;
              }
              vector<GLubyte> data (N);
              r->texture().bind();
              gl::PixelStorei (gl::PACK_ALIGNMENT, 1);
              gl::GetTexImage (gl::TEXTURE_3D, 0, gl::RED_INTEGER, gl::UNSIGNED_BYTE, (void*) (&data[0]));
              const uint32_t lab = uint32_t (s + 1);
              for (size_t i = 0; i < N; ++i) {
                if (data[i]) { label[i] = lab; strength[i] = 1.0f; }
              }
            }
          }
          if (mismatched)
            WARN (str(mismatched) + " ROI(s) skipped for grow-cut (dimensions do not match the current image)");

          // 6-connected synchronous cellular automaton.
          const int dx[6] = { 1, -1, 0, 0, 0, 0 };
          const int dy[6] = { 0, 0, 1, -1, 0, 0 };
          const int dz[6] = { 0, 0, 0, 0, 1, -1 };
          vector<uint32_t> next_label (label);
          vector<float> next_strength (strength);
          {
            ProgressBar progress ("performing grow-cut segmentation");
            bool changed = true;
            for (int iter = 0; iter != 1000 && changed; ++iter) {
              changed = false;
              for (ssize_t z = 0; z != nz; ++z) {
                for (ssize_t y = 0; y != ny; ++y) {
                  for (ssize_t x = 0; x != nx; ++x) {
                    const size_t p = idx(x,y,z);
                    uint32_t best_label = label[p];
                    float best_strength = strength[p];
                    const float cp = intensity[p];
                    for (int n = 0; n != 6; ++n) {
                      const ssize_t qx = x+dx[n], qy = y+dy[n], qz = z+dz[n];
                      if (qx < 0 || qy < 0 || qz < 0 || qx >= nx || qy >= ny || qz >= nz)
                        continue;
                      const size_t q = idx(qx,qy,qz);
                      if (strength[q] <= best_strength)
                        continue;
                      const float g = 1.0f - std::abs (cp - intensity[q]) / range;
                      const float attack = g * strength[q];
                      if (attack > best_strength) {
                        best_strength = attack;
                        best_label = label[q];
                      }
                    }
                    next_label[p] = best_label;
                    next_strength[p] = best_strength;
                    if (best_label != label[p])
                      changed = true;
                  }
                }
              }
              label.swap (next_label);
              strength.swap (next_strength);
              ++progress;
            }
          }

          // Add one new ROI per seed region, holding that region's grown mask.
          // (Appended after the seed ROIs, which are removed further below.)
          const size_t n_seeds = seeds.size();
          const size_t n_regions = seeds.size();
          for (size_t s = 0; s < n_regions; ++s) {
            const uint32_t lab = uint32_t (s + 1);
            MR::Header H (window().image()->header());
            list_model->create (std::move (H));
            ROI_Item* out = dynamic_cast<ROI_Item*> (list_model->items.back().get());
            if (!out)
              continue;
            GL::Context::Grab context;
            GL::assert_context_is_current();
            out->bind();
            gl::PixelStorei (gl::UNPACK_ALIGNMENT, 1);
            vector<GLubyte> slice (size_t(nx) * ny);
            for (ssize_t z = 0; z != nz; ++z) {
              for (ssize_t y = 0; y != ny; ++y)
                for (ssize_t x = 0; x != nx; ++x)
                  slice[size_t(x) + nx*y] = (label[idx(x,y,z)] == lab) ? 1 : 0;
              out->upload_data ({ { 0, 0, z } }, { { nx, ny, 1 } }, reinterpret_cast<void*> (&slice[0]));
            }
          }

          // Replace the seeds with the segmentation: remove the original seed
          // ROIs (the first n_seeds rows; results were appended after them).
          for (size_t s = 0; s < n_seeds; ++s) {
            QModelIndex first = list_model->index (0, 0);
            list_model->remove_item (first);
          }

          updateGL();
        }



        // Seeded region growing (ITK-SNAP-style "region grow"): grow the selected
        // ROI seed to all 6-connected voxels whose intensity is within a tolerance
        // of the seed's mean intensity. The grown region replaces the seed ROI.
        void ROI::region_grow_slot ()
        {
          if (!window().image()) {
            QMessageBox::warning (this, "Region grow", "Load an image first to use as the intensity for region growing.");
            return;
          }
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          if (indices.size() != 1) {
            QMessageBox::warning (this, "Region grow", "Select a single ROI to use as the seed.");
            return;
          }
          ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0]));
          if (!roi)
            return;

          MR::Image<cfloat> in (window().image()->image);
          const ssize_t nx = in.size(0), ny = in.size(1), nz = in.size(2);
          if (roi->header().size(0) != nx || roi->header().size(1) != ny || roi->header().size(2) != nz) {
            QMessageBox::warning (this, "Region grow", "The selected ROI does not match the current image dimensions.");
            return;
          }
          const size_t N = size_t(nx) * ny * nz;
          auto idx = [&] (ssize_t x, ssize_t y, ssize_t z) { return size_t(x) + nx * (size_t(y) + ny * z); };

          // Intensities (first volume) + range.
          vector<float> intensity (N);
          float vmin = std::numeric_limits<float>::infinity();
          float vmax = -std::numeric_limits<float>::infinity();
          if (in.ndim() > 3) in.index(3) = 0;
          for (ssize_t z = 0; z != nz; ++z) { in.index(2) = z;
            for (ssize_t y = 0; y != ny; ++y) { in.index(1) = y;
              for (ssize_t x = 0; x != nx; ++x) { in.index(0) = x;
                const cfloat cv = in.value();
                const float v = cv.real();
                intensity[idx(x,y,z)] = v;
                if (std::isfinite (v)) { vmin = std::min (vmin, v); vmax = std::max (vmax, v); }
              }
            }
          }
          const float range = (vmax > vmin) ? (vmax - vmin) : 1.0f;

          // Read the seed mask.
          vector<GLubyte> seed (N);
          {
            GL::Context::Grab context;
            GL::assert_context_is_current();
            roi->texture().bind();
            gl::PixelStorei (gl::PACK_ALIGNMENT, 1);
            gl::GetTexImage (gl::TEXTURE_3D, 0, gl::RED_INTEGER, gl::UNSIGNED_BYTE, (void*) (&seed[0]));
          }

          // Mean intensity over the seed voxels.
          double sum = 0.0; size_t cnt = 0;
          for (size_t i = 0; i < N; ++i)
            if (seed[i]) { sum += intensity[i]; ++cnt; }
          if (!cnt) {
            QMessageBox::warning (this, "Region grow", "Paint a seed in the selected ROI first.");
            return;
          }
          const float mean = float (sum / double(cnt));

          const float pct = std::isfinite (tolerance_button->value()) ? tolerance_button->value() : 10.0f;
          const float tol = (pct / 100.0f) * range;

          // 6-connected flood fill from the seed voxels.
          const int dx[6] = { 1, -1, 0, 0, 0, 0 };
          const int dy[6] = { 0, 0, 1, -1, 0, 0 };
          const int dz[6] = { 0, 0, 0, 0, 1, -1 };
          vector<char> in_region (N, 0);
          vector<size_t> queue;
          queue.reserve (cnt);
          for (size_t i = 0; i < N; ++i)
            if (seed[i]) { in_region[i] = 1; queue.push_back (i); }
          for (size_t head = 0; head < queue.size(); ++head) {
            const size_t p = queue[head];
            const ssize_t z = ssize_t (p / (size_t(nx) * ny));
            const ssize_t rem = ssize_t (p % (size_t(nx) * ny));
            const ssize_t y = rem / nx;
            const ssize_t x = rem % nx;
            for (int n = 0; n != 6; ++n) {
              const ssize_t qx = x+dx[n], qy = y+dy[n], qz = z+dz[n];
              if (qx < 0 || qy < 0 || qz < 0 || qx >= nx || qy >= ny || qz >= nz)
                continue;
              const size_t q = idx (qx, qy, qz);
              if (!in_region[q] && std::abs (intensity[q] - mean) <= tol) {
                in_region[q] = 1;
                queue.push_back (q);
              }
            }
          }

          // Write the grown region back into the seed ROI.
          {
            GL::Context::Grab context;
            GL::assert_context_is_current();
            roi->bind();
            gl::PixelStorei (gl::UNPACK_ALIGNMENT, 1);
            vector<GLubyte> slice (size_t(nx) * ny);
            for (ssize_t z = 0; z != nz; ++z) {
              for (ssize_t y = 0; y != ny; ++y)
                for (ssize_t x = 0; x != nx; ++x)
                  slice[size_t(x) + nx*y] = in_region[idx(x,y,z)] ? 1 : 0;
              roi->upload_data ({ { 0, 0, z } }, { { nx, ny, 1 } }, reinterpret_cast<void*> (&slice[0]));
            }
          }
          roi->saved = false;

          updateGL();
        }



        // Build the 2D grown region on the active slice (if with_region) and upload
        // base | region to the ROI texture; with_region == false restores the base.
        void ROI::apply_grow (ROI_Item* roi, bool with_region)
        {
          const ssize_t nx = roi->header().size(0), ny = roi->header().size(1), nz = roi->header().size(2);
          auto idx = [&] (ssize_t x, ssize_t y, ssize_t z) { return size_t(x) + nx * (size_t(y) + ny * z); };
          const int axis = grow_axis;
          const ssize_t slice = grow_slice;
          const int u = (axis == 0) ? 1 : 0;
          const int v = (axis == 2) ? 1 : 2;
          const ssize_t su = roi->header().size(u), sv = roi->header().size(v);
          auto full = [&] (ssize_t i, ssize_t j) { ssize_t c[3]; c[axis] = slice; c[u] = i; c[v] = j; return idx (c[0], c[1], c[2]); };

          vector<char> reg;
          if (with_region) {
            reg.assign (size_t(su) * sv, 0);
            vector<std::pair<ssize_t,ssize_t>> q;
            reg[grow_seed_u + su * grow_seed_v] = 1;
            q.push_back ({ grow_seed_u, grow_seed_v });
            const int di[4] = { 1, -1, 0, 0 };
            const int dj[4] = { 0, 0, 1, -1 };
            for (size_t h = 0; h < q.size(); ++h) {
              const ssize_t ci = q[h].first, cj = q[h].second;
              for (int k = 0; k < 4; ++k) {
                const ssize_t ni = ci + di[k], nj = cj + dj[k];
                if (ni < 0 || nj < 0 || ni >= su || nj >= sv)
                  continue;
                char& cell = reg[ni + su * nj];
                if (cell)
                  continue;
                if (std::abs (grow_intensity[full(ni,nj)] - grow_seed_value) <= grow_tol) {
                  cell = 1;
                  q.push_back ({ ni, nj });
                }
              }
            }
          }

          vector<GLubyte> buf (size_t(su) * sv);
          for (ssize_t j = 0; j < sv; ++j)
            for (ssize_t i = 0; i < su; ++i) {
              const GLubyte base = grow_base[full(i,j)];
              const GLubyte r = (with_region && reg[i + su * j]) ? 1 : 0;
              buf[i + su * j] = (base || r) ? 1 : 0;
            }

          const std::array<ssize_t,3> off { { axis==0 ? slice : 0, axis==1 ? slice : 0, axis==2 ? slice : 0 } };
          const std::array<ssize_t,3> sz  { { axis==0 ? ssize_t(1) : nx, axis==1 ? ssize_t(1) : ny, axis==2 ? ssize_t(1) : nz } };
          GL::Context::Grab context;
          GL::assert_context_is_current();
          roi->bind();
          gl::PixelStorei (gl::UNPACK_ALIGNMENT, 1);
          roi->upload_data (off, sz, reinterpret_cast<void*> (buf.data()));
        }



        bool ROI::mouse_wheel_event (int /*delta_x*/, int delta_y)
        {
          if (!grow_mode_button->isChecked() || delta_y == 0 || !window().image())
            return false;

          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          if (indices.size() != 1)
            return false;
          ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0]));
          if (!roi)
            return false;

          const Projection* proj = window().get_current_mode()->get_current_projection();
          if (!proj)
            return false;

          MR::Image<cfloat> img (window().image()->image);
          const ssize_t nx = img.size(0), ny = img.size(1), nz = img.size(2);
          if (roi->header().size(0) != nx || roi->header().size(1) != ny || roi->header().size(2) != nz)
            return false;
          auto idx = [&] (ssize_t x, ssize_t y, ssize_t z) { return size_t(x) + nx * (size_t(y) + ny * z); };

          // Voxel under the cursor.
          const Eigen::Vector3f origin = proj->screen_to_model (window().mouse_position(), window().focus());
          const int axis = normal2axis (proj->screen_normal(), *roi);
          const auto voxf = roi->scanner2voxel() * origin;
          const ssize_t sx = std::lround (voxf[0]), sy = std::lround (voxf[1]), sz = std::lround (voxf[2]);
          if (sx < 0 || sy < 0 || sz < 0 || sx >= nx || sy >= ny || sz >= nz)
            return false;

          const int u = (axis == 0) ? 1 : 0;
          const int v = (axis == 2) ? 1 : 2;
          const ssize_t coord[3] = { sx, sy, sz };
          const ssize_t slice = coord[axis];
          const ssize_t seed_u = coord[u], seed_v = coord[v];

          const bool new_gesture = (!grow_active || axis != grow_axis || slice != grow_slice ||
                                    seed_u != grow_seed_u || seed_v != grow_seed_v);
          if (new_gesture) {
            if (grow_active)
              apply_grow (roi, false);   // discard previous uncommitted preview

            const size_t N = size_t(nx) * ny * nz;
            if (grow_image_id != (const void*) window().image() || grow_intensity.size() != N) {
              grow_intensity.resize (N);
              if (img.ndim() > 3) img.index(3) = 0;
              float vmin = std::numeric_limits<float>::infinity();
              float vmax = -std::numeric_limits<float>::infinity();
              for (ssize_t z = 0; z != nz; ++z) { img.index(2) = z;
                for (ssize_t y = 0; y != ny; ++y) { img.index(1) = y;
                  for (ssize_t x = 0; x != nx; ++x) { img.index(0) = x;
                    const cfloat cv = img.value();
                    const float val = cv.real();
                    grow_intensity[idx(x,y,z)] = val;
                    if (std::isfinite (val)) { vmin = std::min (vmin, val); vmax = std::max (vmax, val); }
                  }
                }
              }
              grow_range = (vmax > vmin) ? (vmax - vmin) : 1.0f;
              grow_image_id = (const void*) window().image();
            }

            grow_base.resize (N);
            {
              GL::Context::Grab context;
              GL::assert_context_is_current();
              roi->texture().bind();
              gl::PixelStorei (gl::PACK_ALIGNMENT, 1);
              gl::GetTexImage (gl::TEXTURE_3D, 0, gl::RED_INTEGER, gl::UNSIGNED_BYTE, (void*) (&grow_base[0]));
            }

            grow_axis = axis; grow_slice = slice; grow_seed_u = seed_u; grow_seed_v = seed_v;
            grow_seed_value = grow_intensity[idx(sx,sy,sz)];
            const float pct = std::isfinite (tolerance_button->value()) ? tolerance_button->value() : 10.0f;
            grow_tol = (pct / 100.0f) * grow_range;
            grow_active = true;
          }
          else {
            const float factor = (delta_y > 0) ? 1.15f : (1.0f / 1.15f);
            grow_tol *= factor;
            if (grow_tol < 1e-6f * grow_range)
              grow_tol = 1e-6f * grow_range;
          }

          apply_grow (roi, true);
          updateGL();
          return true;
        }





        int ROI::normal2axis (const Eigen::Vector3f& normal, const ROI_Item& roi) const
        {
          float x_dot_n = abs ((roi.image2scanner().rotation().cast<float>() * Eigen::Vector3f { 1.0f, 0.0f, 0.0f }).dot (normal));
          float y_dot_n = abs ((roi.image2scanner().rotation().cast<float>() * Eigen::Vector3f { 0.0f, 1.0f, 0.0f }).dot (normal));
          float z_dot_n = abs ((roi.image2scanner().rotation().cast<float>() * Eigen::Vector3f { 0.0f, 0.0f, 1.0f }).dot (normal));
          if (x_dot_n > y_dot_n)
            return x_dot_n > z_dot_n ? 0 : 2;
          else
            return y_dot_n > z_dot_n ? 1 : 2;
        }





        void ROI::save_slot ()
        {
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          assert (indices.size() == 1);
          ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0]));
          save (roi);
        }






        void ROI::load (vector<std::unique_ptr<MR::Header>>& list)
        {
          list_model->load (list);
          list_view->selectionModel()->clear();
          list_view->selectionModel()->select (list_model->index (list_model->rowCount()-1, 0, QModelIndex()), QItemSelectionModel::Select);
          updateGL ();
        }







        void ROI::close_slot ()
        {
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          assert (indices.size() == 1);
          ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0]));
          if (!roi->saved) {
            size_t ret = QMessageBox::warning (this, tr("ROI not saved"),
                qstr("ROI " + roi->get_filename() + " has been modified. Do you want to save it?"),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
            if (ret == QMessageBox::Cancel)
              return;
            else if (ret == QMessageBox::Save)
              save_slot();
          }

          list_model->remove_item (indices.first());
          updateGL();
          in_insert_mode = false;
        }







        void ROI::draw_slot ()
        {
          if (draw_button->isChecked())
            grab_focus ();
          else
            release_focus ();
        }







        void ROI::undo_slot ()
        {
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          if (indices.size() != 1) {
            WARN ("FIXME: shouldn't be here!");
            return;
          }
          ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0]));

          roi->undo();
          update_undo_redo();
          updateGL();
          in_insert_mode = false;
        }







        void ROI::redo_slot ()
        {
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          if (indices.size() != 1) {
            WARN ("FIXME: shouldn't be here!");
            return;
          }
          ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0]));

          roi->redo();
          update_undo_redo();
          updateGL();
          in_insert_mode = false;
        }







        void ROI::slice_copy_slot (QAction* action)
        {
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          if (indices.size() != 1) {
            WARN ("FIXME: shouldn't be here!");
            return;
          }

          ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0]));

          const Projection* proj = window().get_current_mode()->get_current_projection();
          if (!proj) return;
          const Eigen::Vector3f current_origin = proj->screen_to_model (window().mouse_position(), window().focus());
          current_axis = normal2axis (proj->screen_normal(), *roi);
          current_slice = std::lround ((roi->scanner2voxel().cast<float>() * current_origin)[current_axis]);

          roi->start (ROI_UndoEntry (*roi, current_axis, current_slice));

          const int source_slice = current_slice + ((action == copy_from_above_button->defaultAction()) ? 1 : -1);
          if (source_slice < 0 || source_slice >= roi->header().size (current_axis))
            return;

          ROI_UndoEntry source (*roi, current_axis, source_slice);
          roi->current().copy (*roi, source);
          updateGL();
          in_insert_mode = false;
        }








        void ROI::select_edit_mode (QAction*)
        {
          brush_size_button->setEnabled (brush_button->isChecked());
          // Discard any uncommitted region-grow preview when leaving grow mode.
          if (grow_active && !grow_mode_button->isChecked()) {
            QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
            if (indices.size() == 1)
              if (ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0])))
                apply_grow (roi, false);
            grow_active = false;
            updateGL();
          }
        }







        void ROI::hide_all_slot ()
        {
          updateGL();
          in_insert_mode = false;
        }






        void ROI::draw (const Projection& projection, bool is_3D, int, int)
        {
          GL::assert_context_is_current();
          if (is_3D) return;

          if (!is_3D) {
            // set up OpenGL environment:
            gl::Enable (gl::BLEND);
            gl::Disable (gl::DEPTH_TEST);
            gl::DepthMask (gl::FALSE_);
            gl::ColorMask (gl::TRUE_, gl::TRUE_, gl::TRUE_, gl::TRUE_);
            gl::BlendFunc (gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
            gl::BlendEquation (gl::FUNC_ADD);
          }

          for (int i = 0; i < list_model->rowCount(); ++i) {
            if (list_model->items[i]->show && !hide_all_button->isChecked()) {
              ROI_Item* roi = dynamic_cast<ROI_Item*>(list_model->items[i].get());
              //if (is_3D)
              //window.get_current_mode()->overlays_for_3D.push_back (image);
              //else
              roi->render (shader, projection, projection.depth_of (window().focus()));
            }
          }

          if (!is_3D) {
            // restore OpenGL environment:
            gl::Disable (gl::BLEND);
            gl::Enable (gl::DEPTH_TEST);
            gl::DepthMask (gl::TRUE_);
          }
          GL::assert_context_is_current();
        }







        void ROI::toggle_shown_slot (const QModelIndex& index, const QModelIndex& index2)
        {
          if (index.row() == index2.row()) {
            list_view->setCurrentIndex(index);
          }
          else {
            for (size_t i = 0; i < list_model->items.size(); ++i) {
              if (list_model->items[i]->show) {
                list_view->setCurrentIndex (list_model->index (i, 0));
                break;
              }
            }
          }
          updateGL();
          in_insert_mode = false;
        }






        void ROI::update_slot ()
        {
          updateGL();
        }






        void ROI::colour_changed ()
        {
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i) {
            ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[i]));
            QColor c = colour_button->color();
            roi->colour = { { GLubyte (c.red()), GLubyte (c.green()), GLubyte (c.blue()) } };
          }
          updateGL();
        }







        void ROI::opacity_changed (int)
        {
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i) {
            ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[i]));
            roi->alpha = opacity_slider->value() / 1.0e3f;
          }
          window().updateGL();
          in_insert_mode = false;
        }





        void ROI::model_rows_changed ()
        {
          updateGL ();
        }





        void ROI::update_undo_redo ()
        {
          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();

          if (indices.size()) {
            ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0]));
            undo_button->defaultAction()->setEnabled (roi->has_undo());
            redo_button->defaultAction()->setEnabled (roi->has_redo());
          }
          else {
            undo_button->defaultAction()->setEnabled (false);
            redo_button->defaultAction()->setEnabled (false);
          }
        }






        void ROI::update_selection ()
        {
          if (!window().image()) {
            setEnabled (false);
            return;
          }
          else
            setEnabled (true);

          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          bool enable = window().image() && indices.size();

          opacity_slider->setEnabled (enable);
          save_button->setEnabled (enable);
          close_button->setEnabled (enable);
          draw_button->defaultAction()->setEnabled (enable);
          colour_button->setEnabled (enable);
          edit_mode_group->setEnabled (enable);
          slice_copy_group->setEnabled (enable);
          brush_size_button->setEnabled (enable && brush_button->isChecked());

          update_undo_redo();

          if (!indices.size()) {
            draw_button->defaultAction()->setChecked (false);
            return;
          }

          ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0]));
          colour_button->setColor (QColor (roi->colour[0], roi->colour[1], roi->colour[2]));
          opacity_slider->setValue (1.0e3f * roi->alpha);

          brush_size_button->setMin (roi->min_brush_size);
          brush_size_button->setMax (roi->max_brush_size);
          brush_size_button->setRate (0.1f * roi->min_brush_size);
          brush_size_button->setValue (roi->brush_size);
        }









        bool ROI::mouse_press_event ()
        {
          // In region-grow mode a left-click commits the current live preview into
          // the selected ROI (and consumes the click so it does not paint).
          if (grow_mode_button->isChecked()) {
            if (window().mouse_buttons() == Qt::LeftButton) {
              if (grow_active) {
                QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
                if (indices.size() == 1)
                  if (ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0])))
                    roi->saved = false;
                grow_active = false;
              }
              return true;
            }
            return false;
          }

          if (in_insert_mode || window().modifiers() != Qt::NoModifier)
            return false;

          if (window().mouse_buttons() != Qt::LeftButton && window().mouse_buttons() != Qt::RightButton)
            return false;

          in_insert_mode = true;
          insert_mode_value = (window().mouse_buttons() == Qt::LeftButton);
          update_cursor();

          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          if (indices.size() != 1) {
            WARN ("FIXME: shouldn't be here!");
            return false;
          }

          const Projection* proj = window().get_current_mode()->get_current_projection();
          if (!proj)
            return false;
          current_origin = proj->screen_to_model (window().mouse_position(), window().focus());
          window().set_focus (current_origin);
          prev_pos = current_origin;


          // figure out the closest ROI axis, and lock to it:
          ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0]));
          current_axis = normal2axis (proj->screen_normal(), *roi);

          // figure out current slice in ROI:
          current_slice = std::lround ((roi->scanner2voxel() * current_origin)[current_axis]);

          // floating-point version of slice location to keep it consistent on
          // mouse move:
          Eigen::Vector3f slice_axis { 0.0, 0.0, 0.0 };
          slice_axis[current_axis] = current_axis == 2 ? 1.0 : -1.0;
          slice_axis = roi->image2scanner().rotation().cast<float>() * slice_axis;
          current_slice_loc = current_origin.dot (slice_axis);

          const Eigen::Quaternionf orient (roi->image2scanner().rotation());
          window().set_snap_to_image (false);
          window().set_orientation (orient);
          window().set_plane (current_axis);

          roi->start (ROI_UndoEntry (*roi, current_axis, current_slice));


          if (brush_button->isChecked()) {
            if (brush_size_button->isMin())
              roi->current().draw_line (*roi, prev_pos, current_origin, insert_mode_value);
            else
              roi->current().draw_circle (*roi, current_origin, insert_mode_value, brush_size_button->value());
          } else if (rectangle_button->isChecked()) {
            roi->current().draw_rectangle (*roi, current_origin, current_origin, insert_mode_value);
          } else if (fill_button->isChecked()) {
            roi->current().draw_fill (*roi, current_origin, insert_mode_value);
          }


          updateGL();

          return true;
        }






        bool ROI::mouse_move_event ()
        {
          if (!in_insert_mode)
            return false;

          QModelIndexList indices = list_view->selectionModel()->selectedIndexes();
          if (!indices.size()) {
            WARN ("FIXME: shouldn't be here!");
            return false;
          }
          ROI_Item* roi = dynamic_cast<ROI_Item*> (list_model->get (indices[0]));

          const Projection* proj = window().get_current_mode()->get_current_projection();
          if (!proj)
            return false;

          Eigen::Vector3f pos = proj->screen_to_model (window().mouse_position(), window().focus());
          Eigen::Vector3f slice_axis (0.0, 0.0, 0.0);
          slice_axis[current_axis] = current_axis == 2 ? 1.0 : -1.0;
          slice_axis = roi->image2scanner().rotation().cast<float>() * slice_axis;
          float l = (current_slice_loc - pos.dot (slice_axis)) / proj->screen_normal().dot (slice_axis);
          window().set_focus (window().focus() + l * proj->screen_normal());
          const Eigen::Vector3f pos_adj = pos + l * proj->screen_normal();

          if (brush_button->isChecked()) {
            if (brush_size_button->isMin())
              roi->current().draw_line (*roi, prev_pos, pos_adj, insert_mode_value);
            else {
              const float diameter = brush_size_button->value();
              roi->current().draw_thick_line (*roi, prev_pos, pos_adj, insert_mode_value, diameter);
              roi->current().draw_circle (*roi, pos_adj, insert_mode_value, diameter);
            }
          } else if (rectangle_button->isChecked()) {
            roi->current().draw_rectangle (*roi, current_origin, pos_adj, insert_mode_value);
          } else if (fill_button->isChecked()) {
            // Do nothing
          }

          updateGL();
          prev_pos = pos_adj;
          return true;
        }






        bool ROI::mouse_release_event ()
        {
          in_insert_mode = false;
          update_cursor();
          update_undo_redo();
          return true;
        }






        QCursor* ROI::get_cursor ()
        {
          if (!draw_button->isChecked())
            return nullptr;
          if (in_insert_mode && !insert_mode_value)
            return &Cursor::erase;
          return &Cursor::draw;
        }







        void ROI::add_commandline_options (MR::App::OptionList& options)
        {
          using namespace MR::App;
          options
            + OptionGroup ("ROI editor tool options")

            + Option ("roi.load", "Loads the specified image on the ROI editor tool.").allow_multiple()
            +   Argument ("image").type_image_in()

            + Option ("roi.opacity", "Sets the overlay opacity to floating value [0-1].").allow_multiple()
            +   Argument ("value").type_float (0.0, 1.0)

            + Option ("roi.colour", "Sets the colour of the ROI overlay").allow_multiple()
            +   Argument ("R,G,B").type_sequence_float();

        }






        bool ROI::process_commandline_option (const MR::App::ParsedOption& opt)
        {
          if (opt.opt->is ("roi.load")) {
            vector<std::unique_ptr<MR::Header>> list;
            try { list.push_back (make_unique<MR::Header> (MR::Header::open (opt[0]))); }
            catch (Exception& e) { e.display(); }
            load (list);
            return true;
          }

          if (opt.opt->is ("roi.opacity")) {
            try {
              float value = opt[0];
              opacity_slider->setSliderPosition(int(1.e3f*value));
            }
            catch (Exception& e) { e.display(); }
            return true;
          }

          if (opt.opt->is ("roi.colour")) {
            try {
              auto values = parse_floats (opt[0]);
              if (values.size() != 3)
                throw Exception ("must provide exactly three comma-separated values to the -roi.colour option");
              const float max_value = std::max ({ values[0], values[1], values[2] });
              if (std::min ({ values[0], values[1], values[2] }) < 0.0 || max_value > 255)
                throw Exception ("values provided to -roi.colour must be either between 0.0 and 1.0, or between 0 and 255");
              const float multiplier = max_value <= 1.0 ? 255.0 : 1.0;
              QColor colour (int(values[0] * multiplier), int(values[1]*multiplier), int(values[2]*multiplier));
              colour_button->setColor (colour);
              colour_changed();
            }
            catch (Exception& e) { e.display(); }
            return true;
          }

          return false;
        }



      }
    }
  }
}




