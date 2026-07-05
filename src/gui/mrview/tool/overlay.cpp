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

#include "gui/mrview/tool/overlay.h"

#include <limits>
#include <QMessageBox>

#include "mrtrix.h"
#include "image.h"
#include "algo/loop.h"
#include "file/path.h"
#include "gui/gui.h"
#include "gui/mrview/colour_palette.h"
#include "gui/mrview/gui_image.h"
#include "gui/mrview/window.h"
#include "gui/mrview/mode/slice.h"
#include "gui/dialog/file.h"
#include "gui/mrview/tool/list_model_base.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {



        class Overlay::Item : public Image { MEMALIGN(Overlay::Item)
          public:
            Item (MR::Header&& H) : Image (std::move (H)) { }
            Mode::Slice::Shader slice_shader;
        };


        class Overlay::Model : public ListModelBase
        { MEMALIGN(Overlay::Model)
          public:
            Model (QObject* parent) :
              ListModelBase (parent) { }

            size_t colour_counter = 0;
            void add_items (vector<std::unique_ptr<MR::Header>>& list);

            Item* get_image (QModelIndex& index) {
              return dynamic_cast<Item*>(items[index.row()].get());
            }
        };


        void Overlay::Model::add_items (vector<std::unique_ptr<MR::Header>>& list)
        {
          beginInsertRows (QModelIndex(), items.size(), items.size()+list.size());
          for (size_t i = 0; i < list.size(); ++i) {
            const std::string base = Path::basename (list[i]->name());
            // If an overlay with the same name is already loaded, give this one a
            // distinct solid colour so they can be told apart.
            bool duplicate = false;
            for (auto& item : items) {
              Image* existing = dynamic_cast<Image*> (item.get());
              if (existing && Path::basename (existing->image.name()) == base) { duplicate = true; break; }
            }
            Item* overlay = new Item (std::move (*list[i]));
            overlay->set_allowed_features (true, true, false);
            if (!overlay->colourmap)
              overlay->colourmap = 1;
            if (duplicate) {
              overlay->colourmap = 7;   // "Colour" solid-colour map
              overlay->set_colour (distinct_colour (colour_counter++));
            }
            overlay->alpha = 1.0f;
            overlay->set_use_transparency (true);
            items.push_back (std::unique_ptr<Displayable> (overlay));
          }
          endInsertRows();
        }




        Overlay::Overlay (Dock* parent) :
          Base (parent) {
            VBoxLayout* main_box = new VBoxLayout (this);
            HBoxLayout* layout = new HBoxLayout;
            layout->setContentsMargins (0, 0, 0, 0);
            layout->setSpacing (0);

            QPushButton* button = new QPushButton (this);
            button->setToolTip (tr ("Open overlay image"));
            button->setIcon (QIcon (":/open.svg"));
            connect (button, SIGNAL (clicked()), this, SLOT (image_open_slot ()));
            layout->addWidget (button, 1);

            button = new QPushButton (this);
            button->setToolTip (tr ("Close overlay image"));
            button->setIcon (QIcon (":/close.svg"));
            connect (button, SIGNAL (clicked()), this, SLOT (image_close_slot ()));
            layout->addWidget (button, 1);

            hide_all_button = new QPushButton (this);
            hide_all_button->setToolTip (tr ("Hide all overlays"));
            hide_all_button->setIcon (QIcon (":/hide.svg"));
            hide_all_button->setCheckable (true);
            connect (hide_all_button, SIGNAL (clicked()), this, SLOT (hide_all_slot ()));
            layout->addWidget (hide_all_button, 1);

            QPushButton* export_button = new QPushButton (this);
            export_button->setToolTip (tr ("Export selected overlay to file (current threshold baked in)"));
            export_button->setIcon (QIcon (":/save.svg"));
            connect (export_button, SIGNAL (clicked()), this, SLOT (image_export_slot ()));
            layout->addWidget (export_button, 1);

            main_box->addLayout (layout, 0);

            image_list_view = new QListView (this);
            image_list_view->setSelectionMode (QAbstractItemView::ExtendedSelection);
            image_list_view->setHorizontalScrollBarPolicy (Qt::ScrollBarAlwaysOff);
            image_list_view->setTextElideMode (Qt::ElideLeft);
            image_list_view->setDragEnabled (true);
            image_list_view->setDragDropMode (QAbstractItemView::InternalMove);
            image_list_view->setAcceptDrops (true);
            image_list_view->viewport()->setAcceptDrops (true);
            image_list_view->setDropIndicatorShown (true);

            image_list_model = new Model (this);
            image_list_view->setModel (image_list_model);

            image_list_view->setContextMenuPolicy (Qt::CustomContextMenu);
            connect (image_list_view, SIGNAL (customContextMenuRequested (const QPoint&)),
                     this, SLOT (right_click_menu_slot (const QPoint&)));

            main_box->addWidget (image_list_view, 1);

            // Volume selecter
            volume_box = new QGroupBox ("Volume indices (dimension: index)");
            main_box->addWidget (volume_box);
            volume_index_layout = new GridLayout;
            volume_box->setLayout (volume_index_layout);


            QGroupBox* group_box = new QGroupBox (tr("Colour map and scaling"));
            main_box->addWidget (group_box);
            HBoxLayout* hlayout = new HBoxLayout;
            group_box->setLayout (hlayout);

            colourmap_button = new ColourMapButton(this, *this);
            hlayout->addWidget (colourmap_button);

            min_value = new AdjustButton (this);
            connect (min_value, SIGNAL (valueChanged()), this, SLOT (values_changed()));
            hlayout->addWidget (min_value);

            max_value = new AdjustButton (this);
            connect (max_value, SIGNAL (valueChanged()), this, SLOT (values_changed()));
            hlayout->addWidget (max_value);


            QGroupBox* threshold_box = new QGroupBox (tr("Thresholds"));
            main_box->addWidget (threshold_box);
            VBoxLayout* threshold_vlayout = new VBoxLayout;
            threshold_box->setLayout (threshold_vlayout);
            hlayout = new HBoxLayout;
            threshold_vlayout->addLayout (hlayout);

            lower_threshold_check_box = new QCheckBox (this);
            connect (lower_threshold_check_box, SIGNAL (stateChanged(int)), this, SLOT (lower_threshold_changed(int)));
            hlayout->addWidget (lower_threshold_check_box);
            lower_threshold = new AdjustButton (this, 0.1);
            lower_threshold->setEnabled (false);
            connect (lower_threshold, SIGNAL (valueChanged()), this, SLOT (lower_threshold_value_changed()));
            hlayout->addWidget (lower_threshold);

            upper_threshold_check_box = new QCheckBox (this);
            hlayout->addWidget (upper_threshold_check_box);
            upper_threshold = new AdjustButton (this, 0.1);
            upper_threshold->setEnabled (false);
            connect (upper_threshold_check_box, SIGNAL (stateChanged(int)), this, SLOT (upper_threshold_changed(int)));
            connect (upper_threshold, SIGNAL (valueChanged()), this, SLOT (upper_threshold_value_changed()));
            hlayout->addWidget (upper_threshold);

            // A single threshold slider for the lower threshold (synced with the
            // exact-value field above, which still shows/edits the precise number).
            lower_threshold_slider = new QSlider (Qt::Horizontal);
            lower_threshold_slider->setRange (0, 1000);
            lower_threshold_slider->setToolTip (tr ("Threshold"));
            lower_threshold_slider->setEnabled (false);
            connect (lower_threshold_slider, SIGNAL (valueChanged(int)), this, SLOT (lower_threshold_slider_slot(int)));
            threshold_vlayout->addWidget (lower_threshold_slider);


            opacity_slider = new QSlider (Qt::Horizontal);
            opacity_slider->setRange (1,1000);
            opacity_slider->setSliderPosition (int (1000));
            connect (opacity_slider, SIGNAL (valueChanged (int)), this, SLOT (opacity_changed(int)));
            main_box->addWidget (new QLabel ("opacity"), 0);
            main_box->addWidget (opacity_slider, 0);

            interpolate_check_box = new InterpolateCheckBox (tr ("interpolate"));
            interpolate_check_box->setTristate (true);
            interpolate_check_box->setCheckState (Qt::Checked);
            connect (interpolate_check_box, SIGNAL (clicked ()), this, SLOT (interpolate_changed ()));
            main_box->addWidget (interpolate_check_box, 0);

            connect (image_list_view->selectionModel(),
                SIGNAL(selectionChanged(const QItemSelection &, const QItemSelection &)),
                SLOT (selection_changed_slot(const QItemSelection &, const QItemSelection &)) );

            connect (image_list_model, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
                     this, SLOT (toggle_shown_slot (const QModelIndex&, const QModelIndex&)));

            HBoxLayout* checkall_layout = new HBoxLayout;
            QPushButton* check_all_button = new QPushButton (tr ("Check all"), this);
            check_all_button->setObjectName ("batchbtn");
            check_all_button->setToolTip (tr ("Show every overlay by checking its box"));
            connect (check_all_button, &QPushButton::clicked, this, [this]{ image_list_model->check_all(); updateGL(); });
            checkall_layout->addWidget (check_all_button, 1);
            QPushButton* uncheck_all_button = new QPushButton (tr ("Uncheck all"), this);
            uncheck_all_button->setObjectName ("batchbtn");
            uncheck_all_button->setToolTip (tr ("Hide every overlay by unchecking its box"));
            connect (uncheck_all_button, &QPushButton::clicked, this, [this]{ image_list_model->uncheck_all(); updateGL(); });
            checkall_layout->addWidget (uncheck_all_button, 1);
            main_box->addLayout (checkall_layout, 0);

            update_selection();
          }


        void Overlay::image_open_slot ()
        {
          vector<std::string> overlay_names = Dialog::File::get_images (this, "Select overlay images to open", &current_folder);
          if (overlay_names.empty())
            return;
          vector<std::unique_ptr<MR::Header>> list;
          for (size_t n = 0; n < overlay_names.size(); ++n) {
            try {
              list.push_back (make_unique<MR::Header> (MR::Header::open (overlay_names[n])));
            } catch (Exception& e) {
              e.display();
            }
          }
          add_images (list);
        }





        void Overlay::add_images (vector<std::unique_ptr<MR::Header>>& list)
        {
          size_t previous_size = image_list_model->rowCount();
          image_list_model->add_items (list);

          QModelIndex first = image_list_model->index (previous_size, 0, QModelIndex());
          QModelIndex last = image_list_model->index (image_list_model->rowCount()-1, 0, QModelIndex());
          image_list_view->selectionModel()->select (QItemSelection (first, last), QItemSelectionModel::ClearAndSelect);
        }



        void Overlay::get_session (nlohmann::json& node) const
        {
          vector<std::string> files;
          for (size_t i = 0; i < image_list_model->items.size(); ++i) {
            const Image* im = dynamic_cast<const Image*> (image_list_model->items[i].get());
            if (im)
              files.push_back (im->header().name());
          }
          node = files;
        }



        void Overlay::set_session (const nlohmann::json& node)
        {
          if (!node.is_array())
            return;
          vector<std::unique_ptr<MR::Header>> headers;
          for (const auto& f : node) {
            try {
              headers.push_back (make_unique<MR::Header> (MR::Header::open (f.get<std::string>())));
            } catch (Exception& e) {
              e.display();
            }
          }
          if (headers.size())
            add_images (headers);
        }




        void Overlay::dropEvent (QDropEvent* event)
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
            if (list.size())
              add_images (list);
            event->acceptProposedAction();
          }
        }




        void Overlay::image_close_slot ()
        {
          QModelIndexList indexes = image_list_view->selectionModel()->selectedIndexes();
          GL::Context::Grab context;
          GL::assert_context_is_current();
          while (indexes.size()) {
          GL::assert_context_is_current();
            image_list_model->remove_item (indexes.first());
          GL::assert_context_is_current();
            indexes = image_list_view->selectionModel()->selectedIndexes();
          GL::assert_context_is_current();
          }
          GL::assert_context_is_current();
          updateGL();
        }


        namespace {

          // Basename without its image extension (handles .nii.gz etc.).
          std::string overlay_stem (const std::string& name) {
            std::string s = Path::basename (name);
            const size_t dot = s.find_last_of ('.');
            if (dot != std::string::npos) {
              std::string stem = s.substr (0, dot);
              const size_t dot2 = stem.find_last_of ('.');
              if (s.substr (dot) == ".gz" && dot2 != std::string::npos)
                stem = stem.substr (0, dot2);
              s = stem;
            }
            return s;
          }

          // Write one overlay to a Float32 image with its threshold baked to NaN.
          void write_thresholded_overlay (Image* overlay, const std::string& path)
          {
            const bool dl = overlay->use_discard_lower();
            const bool du = overlay->use_discard_upper();
            const float lo = overlay->lessthan, hi = overlay->greaterthan;
            MR::Image<cfloat> in (overlay->image);
            MR::Header header (overlay->header());
            header.datatype() = MR::DataType::Float32;
            header.datatype().set_byte_order_native();
            header.keyval()["mrview_threshold_lower"] = dl ? str(lo) : "none";
            header.keyval()["mrview_threshold_upper"] = du ? str(hi) : "none";
            auto out = MR::Image<float>::create (path, header);
            const float nan = std::numeric_limits<float>::quiet_NaN();
            for (auto l = MR::Loop(in) (in, out); l; ++l) {
              cfloat cv = in.value();
              float v = cv.real();
              if ((dl && v < lo) || (du && v > hi))
                v = nan;
              out.value() = v;
            }
          }

          // Stack N same-grid overlays into one 4D image (threshold baked per volume).
          void write_overlays_4d (const vector<Image*>& overlays, const std::string& path)
          {
            Image* first = overlays[0];
            for (Image* o : overlays)
              for (size_t a = 0; a != 3; ++a)
                if (o->header().size(a) != first->header().size(a))
                  throw Exception ("selected overlays have different dimensions; cannot stack into one 4D image");

            MR::Header header (first->header());
            header.ndim() = 4;
            header.size(3) = overlays.size();
            header.spacing(3) = 1.0;
            header.stride(3) = 4;
            header.datatype() = MR::DataType::Float32;
            header.datatype().set_byte_order_native();
            auto out = MR::Image<float>::create (path, header);

            const float nan = std::numeric_limits<float>::quiet_NaN();
            for (size_t v = 0; v != overlays.size(); ++v) {
              Image* o = overlays[v];
              const bool dl = o->use_discard_lower();
              const bool du = o->use_discard_upper();
              const float lo = o->lessthan, hi = o->greaterthan;
              MR::Image<cfloat> in (o->image);
              out.index(3) = v;
              for (auto l = MR::Loop(0, 3) (in, out); l; ++l) {
                cfloat cv = in.value();
                float val = cv.real();
                if ((dl && val < lo) || (du && val > hi))
                  val = nan;
                out.value() = val;
              }
            }
          }
        }


        void Overlay::image_export_slot ()
        {
          QModelIndexList indexes = image_list_view->selectionModel()->selectedIndexes();
          vector<Item*> overlays;
          for (QModelIndex idx : indexes)
            if (Item* o = image_list_model->get_image (idx))
              overlays.push_back (o);

          if (overlays.empty()) {
            QMessageBox::information (this, "Export overlay", "Please select one or more overlays to export.");
            return;
          }

          try {
            if (overlays.size() == 1) {
              const std::string suggested = overlay_stem (overlays[0]->image.name()) + "_thresholded.nii.gz";
              const std::string fname = Dialog::File::get_save_image_name (this, "Export overlay", suggested);
              if (fname.empty()) return;
              write_thresholded_overlay (overlays[0], fname);
              QMessageBox::information (this, "Export overlay", qstr ("Overlay exported to:\n" + fname));
              return;
            }

            const Dialog::File::MultiSaveChoice choice =
                Dialog::File::ask_multi_save_mode (this, str(overlays.size()) + " overlays",
                    { ".nii.gz", ".nii", ".mif", ".mif.gz", ".mih", ".nrrd" });
            if (choice.mode == Dialog::File::MultiSaveMode::Cancel)
              return;

            if (choice.mode == Dialog::File::MultiSaveMode::SingleFile) {
              const std::string suggested = "overlays_4d.nii.gz";
              const std::string fname = Dialog::File::get_save_image_name (this, "Export overlays as 4D image", suggested);
              if (fname.empty()) return;
              write_overlays_4d (vector<Image*> (overlays.begin(), overlays.end()), fname);
              QMessageBox::information (this, "Export overlays",
                  qstr (str(overlays.size()) + " overlays exported as 4D image:\n" + fname));
            }
            else {
              std::string folder = Dialog::File::get_folder (this, "Select folder for exported overlays");
              if (folder.empty()) return;
              for (Item* o : overlays)
                write_thresholded_overlay (o, Path::join (folder, overlay_stem (o->image.name()) + "_thresholded" + choice.extension));
              QMessageBox::information (this, "Export overlays",
                  qstr (str(overlays.size()) + " overlays exported to:\n" + folder));
            }
          }
          catch (Exception& E) {
            E.display();
            QMessageBox::critical (this, "Export overlay", qstr ("Failed to export overlay:\n" + std::string (E[0])));
          }
        }


        void Overlay::hide_all_slot ()
        {
          updateGL();
        }


        void Overlay::draw (const Projection& projection, bool is_3D, int, int)
        {
          GL::assert_context_is_current();
          if (!is_3D) {
            // set up OpenGL environment:
            gl::Enable (gl::BLEND);
            gl::Disable (gl::DEPTH_TEST);
            gl::DepthMask (gl::FALSE_);
            gl::ColorMask (gl::TRUE_, gl::TRUE_, gl::TRUE_, gl::TRUE_);
            gl::BlendFunc (gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
            gl::BlendEquation (gl::FUNC_ADD);
          }

          bool need_to_update = false;
          for (int i = 0; i < image_list_model->rowCount(); ++i) {
            if (image_list_model->items[i]->show && !hide_all_button->isChecked()) {
              Overlay::Item* image = dynamic_cast<Overlay::Item*>(image_list_model->items[i].get());
              need_to_update |= !std::isfinite (image->intensity_min());
              image->transparent_intensity = image->opaque_intensity = image->intensity_min();
              if (is_3D)
                window().get_current_mode()->overlays_for_3D.push_back (image);
              else
                image->render3D (image->slice_shader, projection, projection.depth_of (window().focus()));
            }
          }

          if (need_to_update)
            update_selection();

          if (!is_3D) {
            // restore OpenGL environment:
            gl::Disable (gl::BLEND);
            gl::Enable (gl::DEPTH_TEST);
            gl::DepthMask (gl::TRUE_);
          }
          GL::assert_context_is_current();
        }


        size_t Overlay::visible_number_colourbars () {
           size_t total_visible(0);

           if(!hide_all_button->isChecked()) {
             for (size_t i = 0, N = image_list_model->rowCount(); i < N; ++i) {
               Image* image  = dynamic_cast<Image*>(image_list_model->items[i].get());
               if (image && image->show && !ColourMap::maps[image->colourmap].special)
                 total_visible += 1;
             }
           }

           return total_visible;
        }


        void Overlay::draw_colourbars ()
        {
          if(hide_all_button->isChecked())
            return;

          for (size_t i = 0, N = image_list_model->rowCount(); i < N; ++i) {
            if (image_list_model->items[i]->show)
              image_list_model->items[i]->request_render_colourbar(*this);
          }
        }


        int Overlay::draw_tool_labels (int position, int start_line_num, const Projection& transform) const
        {
          if(hide_all_button->isChecked()) return 0;

          int num_of_new_lines = 0;

          for (size_t i = 0, N = image_list_model->rowCount(); i < N; ++i) {

            Image* image = dynamic_cast<Image*>(image_list_model->items[i].get());
            if (image && image->show) {
              std::string value_str = Path::basename(image->get_filename()) + " ";
              cfloat value;
              if (image->interpolate()) {
                value_str += "interp value: ";
                value = image->trilinear_value (window().focus());
              } else {
                value_str += "voxel value: ";
                value = image->nearest_neighbour_value (window().focus());
              }
              if (std::isnan(abs(value)))
                value_str += "?";
              else
                value_str += str(value);
              transform.render_text (value_str, position, start_line_num + num_of_new_lines);
              num_of_new_lines += 1;
            }
          }

          return num_of_new_lines;
        }


        void Overlay::selected_colourmap (size_t index, const ColourMapButton&)
        {
            QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
            for (size_t i = 0, N = indices.size(); i < N; ++i) {
              Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
              overlay->set_colourmap (index);
            }
            updateGL();
        }

        void Overlay::selected_custom_colour(const QColor& colour, const ColourMapButton&)
        {
            QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
            for (size_t i = 0, N = indices.size(); i < N; ++i) {
              Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
              std::array<GLubyte, 3> c_colour{{GLubyte(colour.red()), GLubyte(colour.green()), GLubyte(colour.blue())}};
              overlay->set_colour(c_colour);
            }
            updateGL();
        }

        void Overlay::toggle_show_colour_bar(bool visible, const ColourMapButton&)
        {
            QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
            for (size_t i = 0, N = indices.size(); i < N; ++i) {
              Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
              overlay->show_colour_bar = visible;
            }
            updateGL();
        }


        void Overlay::toggle_invert_colourmap(bool invert, const ColourMapButton&)
        {
            QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
            for (size_t i = 0, N = indices.size(); i < N; ++i) {
              Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
              overlay->set_invert_scale(invert);
            }
            updateGL();
        }


        void Overlay::reset_colourmap(const ColourMapButton&)
        {
            QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
            Displayable* overlay = nullptr;
            for (size_t i = 0, N = indices.size(); i < N; ++i) {
              overlay = dynamic_cast<Displayable*> (image_list_model->get_image (indices[i]));
              overlay->reset_windowing();
            }

            // Reset the min/max adjust button fields of last selected overlay
            if(overlay) {
             min_value->setValue(overlay->intensity_min());
             max_value->setValue(overlay->intensity_max());
            }

            updateGL();
        }


        void Overlay::render_image_colourbar (const Image& image)
        {
            float min_value = image.use_discard_lower() ?
                        image.scaling_min_thresholded() :
                        image.scaling_min();

            float max_value = image.use_discard_upper() ?
                        image.scaling_max_thresholded() :
                        image.scaling_max();

            window().colourbar_renderer.render (image.colourmap, image.scale_inverted(),
                                                min_value, max_value,
                                                image.scaling_min(), image.display_range,
                                                Eigen::Vector3f { image.colour[0] / 255.0f, image.colour[1] / 255.0f, image.colour[2] / 255.0f });
        }


        void Overlay::toggle_shown_slot (const QModelIndex& index, const QModelIndex& index2)
        {
          if (index.row() == index2.row()) {
            image_list_view->setCurrentIndex(index);
          } else {
            for (size_t i = 0; i < image_list_model->items.size(); ++i) {
              if (image_list_model->items[i]->show) {
                image_list_view->setCurrentIndex (image_list_model->index (i, 0));
                break;
              }
            }
          }
          updateGL();
        }


        void Overlay::onSetVolumeIndex ()
        {
          QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
          if (indices.size() != 1) return;
          Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[0]));
          if (overlay->header().ndim() < 4) return;
          assert (overlay->header().ndim() == size_t(volume_index_layout->count()+3));

          for (int i = 0; i < volume_index_layout->count(); ++i) {
            auto* box = dynamic_cast<SpinBox*> (volume_index_layout->itemAt(i)->widget());
            if (overlay->header().ndim() <= size_t(i+3))
              break;
            overlay->image.index(i+3) = box->value();
          }
          if (overlay->show)
            updateGL();
        }


        void Overlay::update_slot (int)
        {
          updateGL();
        }



        void Overlay::values_changed ()
        {
          QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i) {
            Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
            overlay->set_windowing (min_value->value(), max_value->value());
          }
          updateGL();
        }


        void Overlay::lower_threshold_changed (int)
        {
          QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i) {
            Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
            overlay->lessthan = lower_threshold->value();
            overlay->set_use_discard_lower (lower_threshold_check_box->isChecked());
          }
          lower_threshold->setEnabled (indices.size() && lower_threshold_check_box->isChecked());
          updateGL();
        }


        void Overlay::upper_threshold_changed (int)
        {
          QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i) {
            Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
            overlay->greaterthan = upper_threshold->value();
            overlay->set_use_discard_upper (upper_threshold_check_box->isChecked());
          }
          upper_threshold->setEnabled (indices.size() && upper_threshold_check_box->isChecked());
          updateGL();
        }



        void Overlay::lower_threshold_value_changed ()
        {
          if (lower_threshold_check_box->isChecked()) {
            QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
            for (int i = 0; i < indices.size(); ++i) {
              Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
              overlay->lessthan = lower_threshold->value();
            }
          }
          updateGL();
        }



        void Overlay::upper_threshold_value_changed ()
        {
          if (upper_threshold_check_box->isChecked()) {
            QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
            for (int i = 0; i < indices.size(); ++i) {
              Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
              overlay->greaterthan = upper_threshold->value();
            }
          }
          updateGL();
        }



        // Map a threshold slider position [0,1000] to a value over [thr_lo, thr_hi]
        // and push it into the matching exact-value field (which applies it). The
        // slider auto-enables its threshold so dragging takes effect immediately.
        void Overlay::lower_threshold_slider_slot (int pos)
        {
          if (image_list_view->selectionModel()->selectedIndexes().empty() || thr_hi <= thr_lo)
            return;
          const float value = thr_lo + (pos / 1000.0f) * (thr_hi - thr_lo);
          lower_threshold->setValue (value);   // AdjustButton::setValue does NOT emit valueChanged
          if (!lower_threshold_check_box->isChecked())
            lower_threshold_check_box->setChecked (true);   // -> lower_threshold_changed applies the value
          else
            lower_threshold_value_changed();                // already enabled: apply the new value
        }



        // Sync the slider position from the lower-threshold value (over [thr_lo, thr_hi]).
        void Overlay::sync_threshold_sliders ()
        {
          auto v2p = [&] (float v) -> int {
            if (thr_hi <= thr_lo || !std::isfinite (v)) return 0;
            const int p = int (std::round (1000.0f * (v - thr_lo) / (thr_hi - thr_lo)));
            return std::max (0, std::min (1000, p));
          };
          lower_threshold_slider->blockSignals (true);
          lower_threshold_slider->setValue (v2p (lower_threshold->value()));
          lower_threshold_slider->blockSignals (false);
        }


        void Overlay::opacity_changed (int)
        {
          QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i) {
            Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
            overlay->alpha = opacity_slider->value() / 1.0e3f;
          }
          window().updateGL();
        }

        void Overlay::interpolate_changed ()
        {
          QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i) {
            Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
            overlay->set_interpolate (interpolate_check_box->isChecked());
          }
          window().updateGL();
        }


        void Overlay::selection_changed_slot (const QItemSelection &, const QItemSelection &)
        {
          update_selection();
        }


        void Overlay::right_click_menu_slot (const QPoint& pos)
        {
          QModelIndex index = image_list_view->indexAt (pos);
          if (!index.isValid())
            return;
          const QPoint globalPos = image_list_view->mapToGlobal (pos);
          image_list_view->selectionModel()->select (index, QItemSelectionModel::Select);
          Item* overlay = image_list_model->get_image (index);

          // For a 4D overlay, offer to split it into one coloured overlay per volume.
          if (overlay && overlay->header().ndim() >= 4 && overlay->header().size(3) > 1) {
            QMenu menu (this);
            QAction* load_vols = menu.addAction (tr ("Load all volumes (separate colours)"));
            menu.addSeparator();
            QAction* cmap = menu.addAction (tr ("Colour map / options…"));
            QAction* chosen = menu.exec (globalPos);
            if (chosen == load_vols)
              load_all_volumes (overlay);
            else if (chosen == cmap)
              colourmap_button->open_menu (globalPos);
            return;
          }

          colourmap_button->open_menu (globalPos);
        }


        void Overlay::load_all_volumes (Item* overlay)
        {
          if (!overlay || overlay->header().ndim() < 4)
            return;
          const size_t nvol = overlay->header().size(3);
          const std::string fname = overlay->image.name();

          vector<std::unique_ptr<MR::Header>> headers;
          try {
            for (size_t v = 0; v != nvol; ++v)
              headers.push_back (make_unique<MR::Header> (MR::Header::open (fname)));
          }
          catch (Exception& e) {
            e.display();
            return;
          }

          const size_t first = image_list_model->rowCount();
          add_images (headers);

          // Pin each new overlay to one volume and give it a distinct solid colour.
          for (size_t v = 0; v != nvol; ++v) {
            QModelIndex idx = image_list_model->index (first + v, 0, QModelIndex());
            Item* o = image_list_model->get_image (idx);
            if (!o)
              continue;
            if (o->image.ndim() > 3)
              o->image.index(3) = v;
            o->colourmap = 7;   // "Colour" solid-colour map
            o->set_colour (distinct_colour (v));
            o->set_filename (overlay_stem (fname) + " [vol " + str(v) + "]");
          }
          updateGL();
        }



        void Overlay::update_selection ()
        {
          QModelIndexList indices = image_list_view->selectionModel()->selectedIndexes();
          while (volume_index_layout->count())
            delete volume_index_layout->takeAt (volume_index_layout->count()-1)->widget();
          colourmap_button->setEnabled (indices.size());
          max_value->setEnabled (indices.size());
          min_value->setEnabled (indices.size());
          lower_threshold_check_box->setEnabled (indices.size());
          upper_threshold_check_box->setEnabled (indices.size());
          lower_threshold->setEnabled (indices.size());
          upper_threshold->setEnabled (indices.size());
          lower_threshold_slider->setEnabled (indices.size());
          opacity_slider->setEnabled (indices.size());
          interpolate_check_box->setEnabled (indices.size());

          if (!indices.size()) {
            max_value->setValue (NAN);
            min_value->setValue (NAN);
            lower_threshold->setValue (NAN);
            upper_threshold->setValue (NAN);
            updateGL();
            return;
          }

          float rate = 0.0f, min_val = 0.0f, max_val = 0.0f;
          float lower_threshold_val = 0.0f, upper_threshold_val = 0.0f;
          float opacity = 0.0f;
          int num_lower_threshold = 0, num_upper_threshold = 0;
          int colourmap_index = -2;
          int num_interp = 0;
          int num_inverted = 0;
          for (int i = 0; i < indices.size(); ++i) {
            Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[i]));
            if (colourmap_index != int(overlay->colourmap)) {
              if (colourmap_index == -2)
                colourmap_index = overlay->colourmap;
              else
                colourmap_index = -1;
            }
            num_inverted += overlay->scale_inverted();
            rate += overlay->scaling_rate();
            min_val += overlay->scaling_min();
            max_val += overlay->scaling_max();
            num_lower_threshold += overlay->use_discard_lower();
            num_upper_threshold += overlay->use_discard_upper();
            opacity += overlay->alpha;
            if (overlay->interpolate())
              ++num_interp;
            if (!std::isfinite (overlay->lessthan))
              overlay->lessthan = overlay->intensity_min();
            if (!std::isfinite (overlay->greaterthan))
              overlay->greaterthan = overlay->intensity_max();
            lower_threshold_val += overlay->lessthan;
            upper_threshold_val += overlay->greaterthan;
          }

          rate /= indices.size();
          min_val /= indices.size();
          max_val /= indices.size();
          lower_threshold_val /= indices.size();
          upper_threshold_val /= indices.size();
          opacity /= indices.size();

          if (indices.size() == 1) {
            Image* overlay = dynamic_cast<Image*> (image_list_model->get_image (indices[0]));

            // volume_box->setVisible(overlay->header().ndim() > 3); // causes shift in FOV due to resizing of tool pane
            for (size_t d = 3; d < overlay->image.ndim(); ++d) {
              SpinBox* vol_index = new SpinBox (this);
              vol_index->setMinimum (0);
              vol_index->setMaximum (overlay->image.size(d) - 1);
              vol_index->setPrefix (qstr(str(d+1) + ": "));
              vol_index->setValue (overlay->image.index(d));
              vol_index->setEnabled (overlay->image.size(d) > 1);
              volume_index_layout->addWidget (vol_index, volume_index_layout->count()/3, volume_index_layout->count()%3);
              connect (vol_index, SIGNAL (valueChanged(int)), this, SLOT (onSetVolumeIndex()));
            }
          }
          if (volume_index_layout->count() == 0) {
            if (indices.size() != 1)
              volume_index_layout->addWidget (new QLabel ("Requires single image selected"));
            else
              volume_index_layout->addWidget (new QLabel ("No volumes to select"));
          }

          colourmap_button->set_colourmap_index(colourmap_index);
          colourmap_button->set_scale_inverted (num_inverted > indices.size()/2);
          opacity_slider->setValue (1.0e3f * opacity);
          if (num_interp == 0)
            interpolate_check_box->setCheckState (Qt::Unchecked);
          else if (num_interp == indices.size())
            interpolate_check_box->setCheckState (Qt::Checked);
          else
            interpolate_check_box->setCheckState (Qt::PartiallyChecked);

          min_value->setRate (rate);
          max_value->setRate (rate);
          min_value->setValue (min_val);
          max_value->setValue (max_val);

          lower_threshold->setValue (lower_threshold_val);
          lower_threshold_check_box->setCheckState (num_lower_threshold ?
              ( num_lower_threshold == indices.size() ?
                Qt::Checked :
                Qt::PartiallyChecked ) :
              Qt::Unchecked);
          lower_threshold->setRate (rate);

          upper_threshold->setValue (upper_threshold_val);
          upper_threshold_check_box->setCheckState (num_upper_threshold ?
              ( num_upper_threshold == indices.size() ?
                Qt::Checked :
                Qt::PartiallyChecked ) :
              Qt::Unchecked);
          upper_threshold->setRate (rate);

          // Slider span follows the colour-map window; sync slider knobs to values.
          thr_lo = min_val;
          thr_hi = max_val;
          sync_threshold_sliders();
        }




        void Overlay::add_commandline_options (MR::App::OptionList& options)
        {
          using namespace MR::App;
          options
            + OptionGroup ("Overlay tool options")

            + Option ("overlay.load", "Loads the specified image on the overlay tool.").allow_multiple()
            +   Argument ("image").type_image_in()

            + Option ("overlay.opacity", "Sets the overlay opacity to floating value [0-1].").allow_multiple()
            +   Argument ("value").type_float (0.0, 1.0)

            + Option ("overlay.colourmap", "Sets the colourmap of the overlay as indexed in the colourmap dropdown menu.").allow_multiple()
            +   Argument ("index").type_integer()

            + Option ("overlay.colour", "Specify a manual colour for the overlay, as three comma-separated values").allow_multiple()
            +   Argument ("R,G,B").type_sequence_float()

            + Option ("overlay.intensity", "Set the intensity windowing of the overlay").allow_multiple()
            +   Argument ("Min,Max").type_sequence_float()

            + Option ("overlay.threshold_min", "Set the lower threshold value of the overlay").allow_multiple()
            +   Argument ("value").type_float()

            + Option ("overlay.threshold_max", "Set the upper threshold value of the overlay").allow_multiple()
            +   Argument ("value").type_float()

            + Option ("overlay.no_threshold_min", "Disable the lower threshold for the overlay").allow_multiple()
            + Option ("overlay.no_threshold_max", "Disable the upper threshold for the overlay").allow_multiple()

            + Option ("overlay.interpolation", "Enable or disable overlay image interpolation.").allow_multiple()
            +   Argument ("value").type_bool();

        }

        bool Overlay::process_commandline_option (const MR::App::ParsedOption& opt)
        {
          if (opt.opt->is ("overlay.load")) {
            vector<std::unique_ptr<MR::Header>> list;
            try { list.push_back (make_unique<MR::Header> (MR::Header::open (opt[0]))); }
            catch (Exception& e) { e.display(); }
            add_images (list);
            return true;
          }

          if (opt.opt->is ("overlay.opacity")) {
            try {
              float value = opt[0];
              opacity_slider->setSliderPosition(int(1.e3f*value));
            }
            catch (Exception& e) { e.display(); }
            return true;
          }

          if (opt.opt->is ("overlay.colourmap")) {
            try {
              int n = opt[0];
              if (n < 0 || !ColourMap::maps[n].name)
                throw Exception ("invalid overlay colourmap index \"" + std::string (opt[0]) + "\" for -overlay.colourmap option");
              colourmap_button->set_colourmap_index(n);
            }
            catch (Exception& e) { e.display(); }
            return true;
          }

          if (opt.opt->is ("overlay.colour")) {
            try {
              auto values = parse_floats (opt[0]);
              if (values.size() != 3)
                throw Exception ("must provide exactly three comma-separated values to the -overlay.colour option");
              const float max_value = std::max ({ values[0], values[1], values[2] });
              if (std::min ({ values[0], values[1], values[2] }) < 0.0 || max_value > 255)
                throw Exception ("values provided to -overlay.colour must be either between 0.0 and 1.0, or between 0 and 255");
              const float multiplier = max_value <= 1.0 ? 255.0 : 1.0;
              QColor colour (int(values[0] * multiplier), int(values[1]*multiplier), int(values[2]*multiplier));
              selected_custom_colour (colour, *colourmap_button);
              colourmap_button->set_fixed_colour();
            }
            catch (Exception& e) { e.display(); }
            return true;
          }

          if (opt.opt->is ("overlay.intensity")) {
            try {
              auto values = parse_floats (opt[0]);
              if (values.size() != 2)
                throw Exception ("must provide exactly two comma-separated values to the -overlay.intensity option");
              min_value->setValue (values[0]);
              max_value->setValue (values[1]);
              values_changed();
            }
            catch (Exception& e) { e.display(); }
            return true;
          }

          if (opt.opt->is ("overlay.threshold_min")) {
            try {
              float value = opt[0];
              lower_threshold->setValue (value);
              lower_threshold_check_box->setChecked (true);
            }
            catch (Exception& e) { e.display(); }
            return true;
          }

          if (opt.opt->is ("overlay.threshold_max")) {
            try {
              float value = opt[0];
              upper_threshold->setValue (value);
              upper_threshold_check_box->setChecked (true);
            }
            catch (Exception& e) { e.display(); }
            return true;
          }

          if (opt.opt->is ("overlay.no_threshold_min")) {
            lower_threshold_check_box->setChecked (false);
            return true;
          }

          if (opt.opt->is ("overlay.no_threshold_max")) {
            upper_threshold_check_box->setChecked (false);
            return true;
          }

          if (opt.opt->is ("overlay.interpolation")) {
            interpolate_check_box->setCheckState (bool(opt[0]) ? Qt::Checked : Qt::Unchecked);
            interpolate_changed();
            return true;
          }

          return false;
        }




      }
    }
  }
}





