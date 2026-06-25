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

#include <map>

#include "header.h"
#include "image.h"
#include "transform.h"
#include "colourmap.h"
#include "connectome/lut.h"

#include "gui/mrview/window.h"
#include "gui/mrview/mode/base.h"
#include "gui/mrview/mode/slice.h"
#include "gui/mrview/gui_image.h"
#include "gui/mrview/tool/atlas.h"
#include "gui/mrview/tool/list_model_base.h"
#include "gui/dialog/file.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        // A single loaded atlas: a scratch RGB volume (one colour per label,
        // displayed through mrview's RGB slice renderer) plus the retained label
        // image + names + per-label centroids for cursor read-out and jumping.
        class Atlas::Item : public Image
        { MEMALIGN(Atlas::Item)
          public:
            Item (MR::Header&& rgb_header, const std::string& label_path, const MR::Connectome::LUT& lut) :
                Image (std::move (rgb_header))
            {
              auto labels_img = MR::Image<float>::open (label_path);
              const MR::Transform T (labels_img);
              scanner2voxel = T.scanner2voxel;

              std::map<uint32_t, std::array<float,3>> colours;
              for (const auto& entry : lut) {
                const uint32_t idx = entry.first;
                names[idx] = entry.second.get_name();
                const auto& c = entry.second.get_colour();
                colours[idx] = { c[0]/255.0f, c[1]/255.0f, c[2]/255.0f };
              }

              const ssize_t nx = labels_img.size(0), ny = labels_img.size(1), nz = labels_img.size(2);
              std::map<uint32_t, std::pair<Eigen::Vector3d, size_t>> accum;

              for (ssize_t z = 0; z != nz; ++z) { labels_img.index(2) = z; image.index(2) = z;
                for (ssize_t y = 0; y != ny; ++y) { labels_img.index(1) = y; image.index(1) = y;
                  for (ssize_t x = 0; x != nx; ++x) { labels_img.index(0) = x; image.index(0) = x;
                    const uint32_t lab = uint32_t (std::round (labels_img.value()));
                    std::array<float,3> rgb { 0.0f, 0.0f, 0.0f };
                    if (lab != 0) {
                      const auto it = colours.find (lab);
                      if (it != colours.end())
                        rgb = it->second;
                      else  // label with no LUT entry: deterministic visible colour
                        rgb = { ((lab*97)%256)/255.0f, ((lab*57)%256)/255.0f, ((lab*131)%256)/255.0f };
                      auto& a = accum[lab];
                      a.first += T.voxel2scanner * Eigen::Vector3d (x, y, z);
                      a.second += 1;
                    }
                    for (ssize_t c = 0; c != 3; ++c) {
                      image.index(3) = c;
                      image.value() = cfloat (rgb[c], 0.0f);
                    }
                  }
                }
              }
              image.index(3) = 0;

              for (const auto& kv : accum) {
                if (kv.second.second)
                  centroids[kv.first] = (kv.second.first / double (kv.second.second)).cast<float>();
              }

              labels = std::move (labels_img);
            }

            // Region name at a scanner-space point (empty if outside / background).
            std::string region_at (const Eigen::Vector3f& world)
            {
              const Eigen::Vector3d v = scanner2voxel * world.cast<double>();
              const ssize_t ix = std::lround (v[0]), iy = std::lround (v[1]), iz = std::lround (v[2]);
              if (ix < 0 || iy < 0 || iz < 0 ||
                  ix >= labels.size(0) || iy >= labels.size(1) || iz >= labels.size(2))
                return std::string();
              labels.index(0) = ix; labels.index(1) = iy; labels.index(2) = iz;
              const uint32_t lab = uint32_t (std::round (labels.value()));
              if (!lab)
                return std::string();
              const auto it = names.find (lab);
              return it == names.end() ? ("label " + str(lab)) : it->second;
            }

            Mode::Slice::Shader slice_shader;
            std::map<uint32_t, std::string> names;
            std::map<uint32_t, Eigen::Vector3f> centroids;
            std::string label_path, lut_path;   // retained for session save

          private:
            MR::Image<float> labels;
            transform_type scanner2voxel;
        };



        class Atlas::Model : public ListModelBase
        { MEMALIGN(Atlas::Model)
          public:
            Model (QObject* parent) : ListModelBase (parent) { }

            void add_item (const std::string& label_path, const std::string& lut_path) {
              Item* item = nullptr;
              try {
                MR::Connectome::LUT lut (lut_path);
                MR::Header label_hdr = MR::Header::open (label_path);
                MR::Header rgb (label_hdr);
                rgb.ndim() = 4;
                rgb.size(3) = 3;
                rgb.spacing(3) = 1.0;
                rgb.stride(3) = 4;
                rgb.datatype() = MR::DataType::Float32;
                rgb.datatype().set_byte_order_native();
                MR::Header scratch = MR::Header::scratch (rgb, "atlas RGB");

                item = new Item (std::move (scratch), label_path, lut);
                item->label_path = label_path;
                item->lut_path = lut_path;
                item->set_allowed_features (true, true, false);
                item->set_colourmap (ColourMap::index ("RGB"));
                item->set_use_transparency (true);
                item->alpha = 0.5f;
                item->set_windowing (0.0f, 1.0f);
                item->transparent_intensity = 0.001f;
                item->opaque_intensity = 0.05f;
                item->show = true;

                beginInsertRows (QModelIndex(), items.size(), items.size());
                items.push_back (std::unique_ptr<Displayable> (item));
                endInsertRows();
              } catch (Exception& e) {
                delete item;
                e.display();
              }
            }

            Item* get (QModelIndex& index) {
              return dynamic_cast<Item*> (items[index.row()].get());
            }
        };



        Atlas::Atlas (Dock* parent) :
            Base (parent)
        {
          VBoxLayout* main_box = new VBoxLayout (this);

          HBoxLayout* hlayout = new HBoxLayout;
          hlayout->setContentsMargins (0, 0, 0, 0);
          hlayout->setSpacing (0);

          QPushButton* open_button = new QPushButton (this);
          open_button->setToolTip (tr ("Open atlas (label volume + lookup table)"));
          open_button->setIcon (QIcon (":/open.svg"));
          connect (open_button, SIGNAL (clicked()), this, SLOT (atlas_open_slot ()));
          hlayout->addWidget (open_button, 1);

          QPushButton* close_button = new QPushButton (this);
          close_button->setToolTip (tr ("Close atlas"));
          close_button->setIcon (QIcon (":/close.svg"));
          connect (close_button, SIGNAL (clicked()), this, SLOT (atlas_close_slot ()));
          hlayout->addWidget (close_button, 1);

          hide_all_button = new QPushButton (this);
          hide_all_button->setToolTip (tr ("Hide all atlases"));
          hide_all_button->setIcon (QIcon (":/hide.svg"));
          hide_all_button->setCheckable (true);
          connect (hide_all_button, SIGNAL (clicked()), this, SLOT (hide_all_slot ()));
          hlayout->addWidget (hide_all_button, 1);

          main_box->addLayout (hlayout, 0);

          atlas_list_view = new QListView (this);
          atlas_list_view->setSelectionMode (QAbstractItemView::SingleSelection);
          atlas_list_view->setTextElideMode (Qt::ElideLeft);
          atlas_list_model = new Model (this);
          atlas_list_view->setModel (atlas_list_model);
          main_box->addWidget (atlas_list_view, 1);

          connect (atlas_list_model, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
                   this, SLOT (toggle_shown_slot (const QModelIndex&, const QModelIndex&)));
          connect (atlas_list_view->selectionModel(),
                   SIGNAL (selectionChanged (const QItemSelection&, const QItemSelection&)),
                   this, SLOT (selection_changed_slot (const QItemSelection&, const QItemSelection&)));

          QGroupBox* display_box = new QGroupBox (tr ("Display"));
          main_box->addWidget (display_box);
          VBoxLayout* display_layout = new VBoxLayout;
          display_box->setLayout (display_layout);
          display_layout->addWidget (new QLabel (tr ("opacity")));
          opacity_slider = new QSlider (Qt::Horizontal);
          opacity_slider->setRange (0, 1000);
          opacity_slider->setSliderPosition (500);
          connect (opacity_slider, SIGNAL (valueChanged (int)), this, SLOT (opacity_slot (int)));
          display_layout->addWidget (opacity_slider);

          QGroupBox* region_box = new QGroupBox (tr ("Region under cursor"));
          main_box->addWidget (region_box);
          VBoxLayout* region_box_layout = new VBoxLayout;
          region_box->setLayout (region_box_layout);
          region_label = new QLabel ("—");
          region_label->setWordWrap (true);
          region_box_layout->addWidget (region_label);

          QGroupBox* list_box = new QGroupBox (tr ("Regions (double-click to jump)"));
          main_box->addWidget (list_box, 1);
          VBoxLayout* list_box_layout = new VBoxLayout;
          list_box->setLayout (list_box_layout);
          region_list = new QListWidget (this);
          connect (region_list, SIGNAL (itemActivated (QListWidgetItem*)),
                   this, SLOT (region_activated_slot (QListWidgetItem*)));
          connect (region_list, SIGNAL (itemDoubleClicked (QListWidgetItem*)),
                   this, SLOT (region_activated_slot (QListWidgetItem*)));
          list_box_layout->addWidget (region_list);

          connect (&window(), SIGNAL (focusChanged()), this, SLOT (focus_changed_slot ()));
        }



        Atlas::Item* Atlas::current_item ()
        {
          QModelIndexList indices = atlas_list_view->selectionModel()->selectedIndexes();
          if (!indices.empty())
            return atlas_list_model->get (indices[0]);
          if (atlas_list_model->items.size())
            return dynamic_cast<Item*> (atlas_list_model->items[0].get());
          return nullptr;
        }



        void Atlas::draw (const Projection& projection, bool is_3D, int, int)
        {
          GL::assert_context_is_current();
          if (!is_3D) {
            gl::Enable (gl::BLEND);
            gl::Disable (gl::DEPTH_TEST);
            gl::DepthMask (gl::FALSE_);
            gl::ColorMask (gl::TRUE_, gl::TRUE_, gl::TRUE_, gl::TRUE_);
            gl::BlendFunc (gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
            gl::BlendEquation (gl::FUNC_ADD);
          }

          for (int i = 0; i < atlas_list_model->rowCount(); ++i) {
            if (atlas_list_model->items[i]->show && !hide_all_button->isChecked()) {
              Item* atlas = dynamic_cast<Item*> (atlas_list_model->items[i].get());
              if (is_3D)
                window().get_current_mode()->overlays_for_3D.push_back (atlas);
              else
                atlas->render3D (atlas->slice_shader, projection, projection.depth_of (window().focus()));
            }
          }

          if (!is_3D) {
            gl::Disable (gl::BLEND);
            gl::Enable (gl::DEPTH_TEST);
            gl::DepthMask (gl::TRUE_);
          }
          GL::assert_context_is_current();
        }



        void Atlas::atlas_open_slot ()
        {
          const std::string label_path = Dialog::File::get_image (this, "Select atlas label volume");
          if (label_path.empty())
            return;
          const std::string lut_path = Dialog::File::get_file (this,
              "Select lookup table (FreeSurfer / ITK-SNAP / AAL / MRtrix / basic)");
          if (lut_path.empty())
            return;

          atlas_list_model->add_item (label_path, lut_path);
          atlas_list_view->selectionModel()->select (
              atlas_list_model->index (atlas_list_model->rowCount()-1, 0),
              QItemSelectionModel::ClearAndSelect);
          populate_region_list();
          window().updateGL();
        }



        void Atlas::atlas_close_slot ()
        {
          QModelIndexList indexes = atlas_list_view->selectionModel()->selectedIndexes();
          while (!indexes.empty()) {
            atlas_list_model->remove_item (indexes.first());
            indexes = atlas_list_view->selectionModel()->selectedIndexes();
          }
          populate_region_list();
          window().updateGL();
        }



        void Atlas::hide_all_slot ()
        {
          window().updateGL();
        }



        void Atlas::toggle_shown_slot (const QModelIndex& index, const QModelIndex&)
        {
          atlas_list_view->setCurrentIndex (index);
          window().updateGL();
        }



        void Atlas::selection_changed_slot (const QItemSelection&, const QItemSelection&)
        {
          populate_region_list();
        }



        void Atlas::opacity_slot (int value)
        {
          const float alpha = value / 1000.0f;
          for (size_t i = 0; i < atlas_list_model->items.size(); ++i) {
            Item* atlas = dynamic_cast<Item*> (atlas_list_model->items[i].get());
            if (atlas)
              atlas->alpha = alpha;
          }
          window().updateGL();
        }



        void Atlas::focus_changed_slot ()
        {
          Item* atlas = current_item();
          if (!atlas) {
            region_label->setText ("—");
            return;
          }
          const std::string name = atlas->region_at (window().focus());
          region_label->setText (name.empty() ? "—" : qstr (name));
        }



        void Atlas::region_activated_slot (QListWidgetItem* item)
        {
          if (!item)
            return;
          Item* atlas = current_item();
          if (!atlas)
            return;
          const uint32_t lab = uint32_t (item->data (Qt::UserRole).toUInt());
          const auto it = atlas->centroids.find (lab);
          if (it == atlas->centroids.end())
            return;
          window().set_focus (it->second);
          window().updateGL();
        }



        void Atlas::populate_region_list ()
        {
          region_list->clear();
          Item* atlas = current_item();
          if (!atlas)
            return;
          // Only list regions actually present in the volume (those with a centroid).
          for (const auto& kv : atlas->centroids) {
            const uint32_t lab = kv.first;
            const auto it = atlas->names.find (lab);
            const std::string name = it == atlas->names.end() ? ("label " + str(lab)) : it->second;
            QListWidgetItem* qitem = new QListWidgetItem (qstr (name), region_list);
            qitem->setData (Qt::UserRole, QVariant (uint (lab)));
          }
        }



        void Atlas::get_session (nlohmann::json& node) const
        {
          node = nlohmann::json::array();
          for (size_t i = 0; i < atlas_list_model->items.size(); ++i) {
            const Item* atlas = dynamic_cast<const Item*> (atlas_list_model->items[i].get());
            if (atlas)
              node.push_back ({ { "labels", atlas->label_path }, { "lut", atlas->lut_path } });
          }
        }



        void Atlas::set_session (const nlohmann::json& node)
        {
          if (!node.is_array())
            return;
          for (const auto& entry : node) {
            if (entry.find ("labels") == entry.end() || entry.find ("lut") == entry.end())
              continue;
            atlas_list_model->add_item (entry["labels"].get<std::string>(),
                                        entry["lut"].get<std::string>());
          }
          if (atlas_list_model->rowCount())
            atlas_list_view->selectionModel()->select (
                atlas_list_model->index (atlas_list_model->rowCount()-1, 0),
                QItemSelectionModel::ClearAndSelect);
          populate_region_list();
        }



        void Atlas::dropEvent (QDropEvent*)
        {
          // Atlas loading needs a label volume + a LUT, so drag-and-drop (which
          // supplies only file paths) is intentionally not wired up here.
        }


      }
    }
  }
}
