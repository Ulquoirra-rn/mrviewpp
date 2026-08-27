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

#include "gui/mrview/tool/view.h"

#include <QColorDialog>
#include <QDockWidget>
#include <QLineEdit>
#include <QMenu>

#include "file/path.h"
#include "gui/mrview/atlas_registration.h"
#include "gui/mrview/tool/tractography/tractography.h"
#include "gui/mrview/tool/tractography/tractogram.h"

#include "mrtrix.h"
#include "math/math.h"
#include "file/config.h"
#include "gui/gui.h"
#include "gui/mrview/window.h"
#include "gui/mrview/mode/volume.h"
#include "gui/mrview/mode/lightbox_gui.h"
#include "gui/mrview/mode/ortho.h"
#include "gui/mrview/mode/lightbox.h"
#include "gui/mrview/adjust_button.h"

#define FOV_RATE_MULTIPLIER 0.01f
#define MRTRIX_MIN_ALPHA 1.0e-3f
#define MRTRIX_ALPHA_MULT (-std::log (MRTRIX_MIN_ALPHA)/1000.0f)


namespace {

  inline float get_alpha_from_slider (float slider_value) {
    return MRTRIX_MIN_ALPHA * std::exp (MRTRIX_ALPHA_MULT * float (slider_value));
  }

  inline float get_slider_value_from_alpha (float alpha) {
    return std::log (alpha/MRTRIX_MIN_ALPHA) / MRTRIX_ALPHA_MULT;
  }

}




namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {


        class View::ClipPlaneModel : public QAbstractItemModel
        { MEMALIGN(View::ClipPlaneModel)
          public:

            ClipPlaneModel (QObject* parent) :
              QAbstractItemModel (parent) { }

            QVariant data (const QModelIndex& index, int role) const {
              if (!index.isValid()) return {};
              if (role == Qt::CheckStateRole) {
                return planes[index.row()].active ? Qt::Checked : Qt::Unchecked;
              }
              if (role != Qt::DisplayRole) return {};
              return qstr (planes[index.row()].name);
            }

            bool setData (const QModelIndex& idx, const QVariant& value, int role) {
              if (role == Qt::CheckStateRole) {
                planes[idx.row()].active = (value == Qt::Checked);
                emit dataChanged (idx, idx);
                return true;
              }
              return QAbstractItemModel::setData (idx, value, role);
            }

            Qt::ItemFlags flags (const QModelIndex& index) const {
              if (!index.isValid()) return {};
              return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
            }

            QModelIndex index (int row, int column, const QModelIndex& parent = QModelIndex()) const
            {
              (void) parent; // to suppress warnings about unused parameters
              return createIndex (row, column);
            }

            QModelIndex parent (const QModelIndex&) const { return {}; }

            int rowCount (const QModelIndex& parent = QModelIndex()) const
            {
              (void) parent; // to suppress warnings about unused parameters
              return planes.size();
            }

            int columnCount (const QModelIndex& parent = QModelIndex()) const
            {
              (void) parent; // to suppress warnings about unused parameters
              return 1;
            }

            void remove (QModelIndex& index) {
              beginRemoveRows (QModelIndex(), index.row(), index.row());
              planes.erase (planes.begin() + index.row());
              endRemoveRows();
            }

            void invert (QModelIndex& index) {
              ClipPlane& p (planes[index.row()]);
              for (size_t n = 0; n < 4; ++n)
                p.plane[n] = -p.plane[n];
              if (p.name[0] == '-')
                p.name = p.name.substr (1);
              else
                p.name = "-" + p.name;
              emit dataChanged (index, index);
            }

            void reset (QModelIndex& index, const Image& image, int proj) {
              reset (planes[index.row()], image, proj);
            }

            void reset (ClipPlane& p, const Image& image, int proj) {
              const transform_type& M (image.header().transform());
              p.plane[0] = M (proj, 0);
              p.plane[1] = M (proj, 1);
              p.plane[2] = M (proj, 2);

              const Eigen::Vector3f centre = image.voxel2scanner() * Eigen::Vector3f { image.header().size(0)/2.0f, image.header().size(1)/2.0f, image.header().size(2)/2.0f };
              p.plane[3] = centre[0]*p.plane[0] + centre[1]*p.plane[1] + centre[2]*p.plane[2];
              p.active = true;

              p.name = ( proj == 0 ? "sagittal" : ( proj == 1 ? "coronal" : "axial" ) );
            }

            void clear () {
              beginRemoveRows (QModelIndex(), 0, planes.size());
              planes.clear();
              endRemoveRows();
            }

            void add (const Image& image, int proj) {
              ClipPlane p;
              reset (p, image, proj);
              beginInsertRows (QModelIndex(), planes.size(), planes.size() + 1);
              planes.push_back (p);
              endInsertRows();
            }

            vector<ClipPlane> planes;
        };




        View::View (Dock* parent) :
          Base (parent)
        {
          VBoxLayout* main_box = new VBoxLayout (this);

          HBoxLayout* hlayout  = new HBoxLayout;
          hlayout->setContentsMargins (0, 0, 0, 0);
          hlayout->setSpacing (0);

          hide_button = new QPushButton ("Hide main image",this);
          hide_button->setToolTip (tr ("Hide all main images"));
          hide_button->setIcon (QIcon (":/hide.svg"));
          hide_button->setCheckable (true);
          hide_button->setChecked (!window().get_image_visibility());
          connect (hide_button, SIGNAL (clicked(bool)), this, SLOT (hide_image_slot (bool)));
          hlayout->addWidget (hide_button, 1);

          main_box->addLayout (hlayout, 0);

          // FoV
          QGroupBox* group_box = new QGroupBox ("FOV");
          main_box->addWidget (group_box);
          hlayout = new HBoxLayout;
          group_box->setLayout (hlayout);

          fov = new AdjustButton (this);
          connect (fov, SIGNAL (valueChanged()), this, SLOT (onSetFOV()));
          hlayout->addWidget (fov);

          plane_combobox = new QComboBox;
          plane_combobox->insertItem (0, "Sagittal");
          plane_combobox->insertItem (1, "Coronal");
          plane_combobox->insertItem (2, "Axial");
          connect (plane_combobox, SIGNAL (activated(int)), this, SLOT (onSetPlane(int)));
          hlayout->addWidget (plane_combobox);

          // Focus
          group_box = new QGroupBox ("Focus");
          main_box->addWidget (group_box);
          GridLayout* layout = new GridLayout;
          group_box->setLayout (layout);

          const int focus_button_width = 80;

          layout->addWidget (new QLabel (tr("Voxel: ")), 0, 0);

          voxel_x = new AdjustButton (this);
          voxel_x->setMinimumWidth(focus_button_width);
          connect (voxel_x, SIGNAL (valueChanged()), this, SLOT (onSetVoxel()));
          layout->addWidget (voxel_x, 0, 1);

          voxel_y = new AdjustButton (this);
          voxel_y->setMinimumWidth(focus_button_width);
          connect (voxel_y, SIGNAL (valueChanged()), this, SLOT (onSetVoxel()));
          layout->addWidget (voxel_y, 0, 2);

          voxel_z = new AdjustButton (this);
          voxel_z->setMinimumWidth(focus_button_width);
          connect (voxel_z, SIGNAL (valueChanged()), this, SLOT (onSetVoxel()));
          layout->addWidget (voxel_z, 0, 3);

          copy_voxel_button = new QPushButton ("copy",this);
          copy_voxel_button->setMinimumWidth(30);
          copy_voxel_button->setToolTip (tr ("copy voxel to clipboard"));
          copy_voxel_button->setCheckable (false);
          connect (copy_voxel_button, SIGNAL (clicked()), this, SLOT (copy_voxel_slot ()));
          layout->addWidget (copy_voxel_button, 0, 4);

          layout->addWidget (new QLabel (tr("Position: ")), 1, 0);

          focus_x = new AdjustButton (this);
          focus_x->setMinimumWidth(focus_button_width);
          connect (focus_x, SIGNAL (valueChanged()), this, SLOT (onSetFocus()));
          layout->addWidget (focus_x, 1, 1);

          focus_y = new AdjustButton (this);
          focus_y->setMinimumWidth(focus_button_width);
          connect (focus_y, SIGNAL (valueChanged()), this, SLOT (onSetFocus()));
          layout->addWidget (focus_y, 1, 2);

          focus_z = new AdjustButton (this);
          focus_z->setMinimumWidth(focus_button_width);
          connect (focus_z, SIGNAL (valueChanged()), this, SLOT (onSetFocus()));
          layout->addWidget (focus_z, 1, 3);

          copy_focus_button = new QPushButton ("copy",this);
          copy_focus_button->setMinimumWidth(30);
          copy_focus_button->setToolTip (tr ("copy position to clipboard"));
          copy_focus_button->setCheckable (false);
          connect (copy_focus_button, SIGNAL (clicked()), this, SLOT (copy_focus_slot ()));
          layout->addWidget (copy_focus_button, 1, 4);

          // Volume
          volume_box = new QGroupBox ("Volume indices (dimension: index)");
          main_box->addWidget (volume_box);
          volume_index_layout = new GridLayout;
          volume_box->setLayout (volume_index_layout);

          // Intensity
          group_box = new QGroupBox ("Intensity scaling");
          main_box->addWidget (group_box);
          hlayout = new HBoxLayout;
          group_box->setLayout (hlayout);

          min_entry = new AdjustButton (this);
          connect (min_entry, SIGNAL (valueChanged()), this, SLOT (onSetScaling()));
          hlayout->addWidget (min_entry);

          max_entry = new AdjustButton (this);
          connect (max_entry, SIGNAL (valueChanged()), this, SLOT (onSetScaling()));
          hlayout->addWidget (max_entry);


          // ortho view options
          ortho_view_in_row_check_box = new QCheckBox ("display images in a row");
          ortho_view_in_row_check_box->setCheckable (true);
          ortho_view_in_row_check_box->setChecked (false);
          ortho_view_in_row_check_box->setToolTip (
              "Display the 3 orthogonal views in a row, rather than a 2x2 montage.\n\n"
              "To make this the default, set the \"MRViewOrthoAsRow\" option in your configuration file.");
          main_box->addWidget (ortho_view_in_row_check_box);

          // volume render options
          transparency_box = new QGroupBox ("Transparency");
          main_box->addWidget (transparency_box);
          VBoxLayout* vlayout = new VBoxLayout;
          transparency_box->setLayout (vlayout);

          hlayout = new HBoxLayout;
          vlayout->addLayout (hlayout);

          transparent_intensity = new AdjustButton (this);
          connect (transparent_intensity, SIGNAL (valueChanged()), this, SLOT (onSetTransparency()));
          hlayout->addWidget (transparent_intensity);

          opaque_intensity = new AdjustButton (this);
          connect (opaque_intensity, SIGNAL (valueChanged()), this, SLOT (onSetTransparency()));
          hlayout->addWidget (opaque_intensity);

          hlayout = new HBoxLayout;
          vlayout->addLayout (hlayout);

          QLabel* alpha_label = new QLabel ("alpha");
          alpha_label->setToolTip (tr (
                "How opaque the image itself is, sample by sample, as the ray passes\n"
                "through it.\n\n"
                "This is a property of the render: lowering it lets deeper material\n"
                "contribute, so the picture changes rather than fades - a low alpha\n"
                "shows more of the inside of the volume, not less of the volume. Nor\n"
                "does it let anything through that the image is in front of; an overlay\n"
                "behind image material is still occluded by it, in proportion.\n\n"
                "x-ray below is the other question: how much of the finished render to\n"
                "show at all, relative to what is drawn inside it. Alpha decides what\n"
                "the volume looks like; x-ray decides whether you are looking at the\n"
                "volume or at its contents."));
          hlayout->addWidget (alpha_label);
          opacity = new QSlider (Qt::Horizontal);
          opacity->setToolTip (alpha_label->toolTip());
          opacity->setRange (0, 1000);
          opacity->setValue (1000);
          connect (opacity, SIGNAL (valueChanged(int)), this, SLOT (onSetTransparency()));
          hlayout->addWidget (opacity);

          hlayout = new HBoxLayout;
          vlayout->addLayout (hlayout);
          QLabel* xray_label = new QLabel ("x-ray");
          xray_label->setToolTip (tr (
                "See through a volume render, so what is inside it shows.\n\n"
                "Scales the image's own contribution and nothing else: tracts and\n"
                "meshes drawn inside the render come through in proportion, and so\n"
                "do overlays. At 100% the render is gone and only its contents are\n"
                "left; at 0% nothing changes.\n\n"
                "Not the same as alpha above, which changes how opaque the image is\n"
                "sample by sample and so changes what the render shows. x-ray leaves\n"
                "the render exactly as it is and fades it against its contents.\n\n"
                "Applies to the Volume mode and to the volume pane in the ortho view."));
          hlayout->addWidget (xray_label);
          xray = new QSlider (Qt::Horizontal);
          xray->setRange (0, 100);
          xray->setValue (int (100.0f * Mode::Volume::xray_strength));
          xray->setToolTip (xray_label->toolTip());
          connect (xray, SIGNAL (valueChanged(int)), this, SLOT (onSetXray()));
          set_slider_steps (xray);
          hlayout->addWidget (xray);


          threshold_box = new QGroupBox ("Thresholds");
          main_box->addWidget (threshold_box);
          hlayout = new HBoxLayout;
          threshold_box->setLayout (hlayout);

          lower_threshold_check_box = new QCheckBox (this);
          hlayout->addWidget (lower_threshold_check_box);
          lower_threshold = new AdjustButton (this);
          lower_threshold->setValue (window().image() ? window().image()->intensity_min() : 0.0);
          connect (lower_threshold_check_box, SIGNAL (clicked(bool)), this, SLOT (onCheckThreshold(bool)));
          connect (lower_threshold, SIGNAL (valueChanged()), this, SLOT (onSetTransparency()));
          hlayout->addWidget (lower_threshold);

          upper_threshold_check_box = new QCheckBox (this);
          hlayout->addWidget (upper_threshold_check_box);
          upper_threshold = new AdjustButton (this);
          upper_threshold->setValue (window().image() ? window().image()->intensity_max() : 1.0);
          connect (upper_threshold_check_box, SIGNAL (clicked(bool)), this, SLOT (onCheckThreshold(bool)));
          connect (upper_threshold, SIGNAL (valueChanged()), this, SLOT (onSetTransparency()));
          hlayout->addWidget (upper_threshold);

          // clip planes:

          clip_box = new QGroupBox ("Clip planes");
          clip_box->setCheckable (true);
          connect (clip_box, SIGNAL (toggled(bool)), this, SLOT(clip_planes_toggle_shown_slot()));
          main_box->addWidget (clip_box);
          vlayout = new VBoxLayout;
          clip_box->setLayout (vlayout);
          hlayout = new HBoxLayout;
          vlayout->addLayout (hlayout);


          clip_planes_model = new ClipPlaneModel (this);
          connect (clip_planes_model, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
              this, SLOT (clip_planes_selection_changed_slot()));
          connect (clip_planes_model, SIGNAL (rowsInserted (const QModelIndex&, int, int)),
              this, SLOT (clip_planes_selection_changed_slot()));
          connect (clip_planes_model, SIGNAL (rowsRemoved (const QModelIndex&, int, int)),
              this, SLOT (clip_planes_selection_changed_slot()));

          clip_planes_list_view = new QListView (this);
          clip_planes_list_view->setModel (clip_planes_model);
          clip_planes_list_view->setSelectionMode (QAbstractItemView::ExtendedSelection);
          clip_planes_list_view->setContextMenuPolicy (Qt::CustomContextMenu);
          clip_planes_list_view->setToolTip ("Right-click for more options");
          connect (clip_planes_list_view, SIGNAL (customContextMenuRequested (const QPoint&)),
              this, SLOT (clip_planes_right_click_menu_slot (const QPoint&)));
          connect (clip_planes_list_view->selectionModel(),
              SIGNAL (selectionChanged (const QItemSelection, const QItemSelection)),
              this, SLOT (clip_planes_selection_changed_slot()));
          hlayout->addWidget (clip_planes_list_view, 1);

          QToolBar* toolbar = new QToolBar (this);
          toolbar->setOrientation (Qt::Vertical);
          toolbar->setFloatable (false);
          toolbar->setMovable (false);
          toolbar->setIconSize (QSize (16, 16));
          hlayout->addWidget (toolbar);

          clip_highlight_check_box = new QCheckBox ("Highlight selected clip planes");
          clip_highlight_check_box->setToolTip ("Helps to identify selected clip planes that can be interacted with.");
          clip_highlight_check_box->setChecked (true);
          connect (clip_highlight_check_box, SIGNAL (toggled(bool)), this, SLOT(clip_planes_toggle_highlight_slot()));
          vlayout->addWidget (clip_highlight_check_box);

          clip_intersectionmode_check_box = new QCheckBox ("Intersection mode");
          clip_intersectionmode_check_box->setToolTip ("Generated volume is the intersection of individual clipped volumes, rather than the union.");
          clip_intersectionmode_check_box->setChecked (false);
          connect (clip_intersectionmode_check_box, SIGNAL (toggled(bool)), this, SLOT(clip_planes_toggle_intersectionmode_slot()));
          vlayout->addWidget (clip_intersectionmode_check_box);



          // clip planes handling:

          clip_planes_option_menu = new QMenu ();
          QMenu* submenu = clip_planes_option_menu->addMenu("&New");

          QToolButton* button = new QToolButton (this);
          button->setMenu (submenu);
          button->setPopupMode (QToolButton::InstantPopup);
          button->setToolTip ("Add new clip planes");
          button->setIcon (QIcon (":/new.svg"));
          toolbar->addWidget (button);

          clip_planes_new_axial_action = new QAction ("&axial", this);
          connect (clip_planes_new_axial_action, SIGNAL (triggered()), this, SLOT (clip_planes_add_axial_slot()));
          submenu->addAction (clip_planes_new_axial_action);

          clip_planes_new_sagittal_action = new QAction ("&sagittal", this);
          connect (clip_planes_new_sagittal_action, SIGNAL (triggered()), this, SLOT (clip_planes_add_sagittal_slot()));
          submenu->addAction (clip_planes_new_sagittal_action);

          clip_planes_new_coronal_action = new QAction ("&coronal", this);
          connect (clip_planes_new_coronal_action, SIGNAL (triggered()), this, SLOT (clip_planes_add_coronal_slot()));
          submenu->addAction (clip_planes_new_coronal_action);

          clip_planes_option_menu->addSeparator();

          submenu = clip_planes_reset_submenu = clip_planes_option_menu->addMenu("&Reset");

          button = new QToolButton (this);
          button->setMenu (submenu);
          button->setPopupMode (QToolButton::InstantPopup);
          button->setToolTip ("Reset selected clip planes");
          button->setIcon (QIcon (":/reset.svg"));
          toolbar->addWidget (button);

          clip_planes_reset_axial_action = new QAction ("&axial", this);
          connect (clip_planes_reset_axial_action, SIGNAL (triggered()), this, SLOT (clip_planes_reset_axial_slot()));
          submenu->addAction (clip_planes_reset_axial_action);

          clip_planes_reset_sagittal_action = new QAction ("&sagittal", this);
          connect (clip_planes_reset_sagittal_action, SIGNAL (triggered()), this, SLOT (clip_planes_reset_sagittal_slot()));
          submenu->addAction (clip_planes_reset_sagittal_action);

          clip_planes_reset_coronal_action = new QAction ("&coronal", this);
          connect (clip_planes_reset_coronal_action, SIGNAL (triggered()), this, SLOT (clip_planes_reset_coronal_slot()));
          submenu->addAction (clip_planes_reset_coronal_action);


          clip_planes_invert_action = new QAction("&Invert", this);
          clip_planes_invert_action->setToolTip ("Invert selected clip planes");
          clip_planes_invert_action->setIcon (QIcon (":/invert.svg"));
          connect (clip_planes_invert_action, SIGNAL (triggered()), this, SLOT (clip_planes_invert_slot()));
          clip_planes_option_menu->addAction (clip_planes_invert_action);

          button = new QToolButton (this);
          button->setDefaultAction (clip_planes_invert_action);
          toolbar->addWidget (button);

          clip_planes_remove_action = new QAction("R&emove", this);
          clip_planes_remove_action->setToolTip ("Remove selected clip planes");
          clip_planes_remove_action->setIcon (QIcon (":/close.svg"));
          connect (clip_planes_remove_action, SIGNAL (triggered()), this, SLOT (clip_planes_remove_slot()));
          clip_planes_option_menu->addAction (clip_planes_remove_action);

          button = new QToolButton (this);
          button->setDefaultAction (clip_planes_remove_action);
          toolbar->addWidget (button);

          clip_planes_option_menu->addSeparator();

          clip_planes_clear_action = new QAction("&Clear", this);
          clip_planes_clear_action->setToolTip ("Clear all clip planes");
          clip_planes_clear_action->setIcon (QIcon (":/clear.svg"));
          connect (clip_planes_clear_action, SIGNAL (triggered()), this, SLOT (clip_planes_clear_slot()));
          clip_planes_option_menu->addAction (clip_planes_clear_action);

          button = new QToolButton (this);
          button->setDefaultAction (clip_planes_clear_action);
          toolbar->addWidget (button);

          clip_planes_option_menu->addSeparator();

          // Built-in tract atlas
          atlas_box = new QGroupBox (tr ("Tract atlas"));
          main_box->addWidget (atlas_box);
          VBoxLayout* atlas_layout = new VBoxLayout;
          atlas_box->setLayout (atlas_layout);

          atlas_status = new QLabel (tr ("no image aligned"));
          atlas_status->setWordWrap (true);
          atlas_layout->addWidget (atlas_status);

          atlas_align_button = new QPushButton (tr ("Align to current image"));
          atlas_align_button->setToolTip (tr ("Align the built-in atlas to the image being viewed"));
          connect (atlas_align_button, SIGNAL (clicked()), this, SLOT (atlas_align_clicked()));
          atlas_layout->addWidget (atlas_align_button);

          atlas_filter = new QLineEdit;
          make_search_box (atlas_filter, tr ("Search bundles - e.g. CST, _L"));
          atlas_filter->setClearButtonEnabled (true);
          connect (atlas_filter, SIGNAL (textChanged (const QString&)),
                   this, SLOT (atlas_filter_changed (const QString&)));
          atlas_layout->addWidget (atlas_filter);

          atlas_list = new QListWidget;
          atlas_list->setSelectionMode (QAbstractItemView::NoSelection);
          atlas_list->setUniformItemSizes (true);
          // Enough to browse without the panel taking over; the list scrolls.
          atlas_list->setMinimumHeight (140);
          connect (atlas_list, SIGNAL (itemChanged (QListWidgetItem*)),
                   this, SLOT (atlas_item_changed (QListWidgetItem*)));
          atlas_list->setContextMenuPolicy (Qt::CustomContextMenu);
          connect (atlas_list, SIGNAL (customContextMenuRequested (const QPoint&)),
                   this, SLOT (atlas_context_menu (const QPoint&)));
          atlas_layout->addWidget (atlas_list);

          connect (&window().atlas_registration(), SIGNAL (changed()),
                   this, SLOT (atlas_state_changed()));
          atlas_state_changed();

          // Light box view options
          init_lightbox_gui (main_box);

          main_box->addStretch ();

          ortho_view_in_row_check_box->setVisible (false);
          transparency_box->setVisible (true);
          threshold_box->setVisible (true);
          clip_box->setVisible (false);
          lightbox_box->setVisible (false);
        }



        void View::showEvent (QShowEvent*)
        {
          connect (&window(), SIGNAL (imageChanged()), this, SLOT (onImageChanged()));
          connect (&window(), SIGNAL (imageVisibilityChanged(bool)), this, SLOT (onImageVisibilityChanged(bool)));
          connect (&window(), SIGNAL (focusChanged()), this, SLOT (onFocusChanged()));
          connect (&window(), SIGNAL (planeChanged()), this, SLOT (onPlaneChanged()));
          connect (&window(), SIGNAL (scalingChanged()), this, SLOT (onScalingChanged()));
          connect (&window(), SIGNAL (modeChanged()), this, SLOT (onModeChanged()));
          connect (&window(), SIGNAL (fieldOfViewChanged()), this, SLOT (onFOVChanged()));
          connect (&window(), SIGNAL (volumeChanged()), this, SLOT (onVolumeIndexChanged()));
          onPlaneChanged();
          onFocusChanged();
          onScalingChanged();
          onModeChanged();
          onImageChanged();
          onFOVChanged();
          clip_planes_selection_changed_slot();
        }




        void View::onVolumeIndexChanged()
        {
          assert (window().image());
          const auto& image (window().image()->image);
          assert (image.ndim() == size_t(volume_index_layout->count() + 3));

          for (int i = 0; i < volume_index_layout->count(); ++i) {
            auto* box = dynamic_cast<SpinBox*> (volume_index_layout->itemAt(i)->widget());
            box->setValue (image.ndim() > size_t(i + 3) ? image.index(i + 3) : 0);
          }
        }




        void View::closeEvent (QCloseEvent*)
        {
          window().disconnect (this);
        }



        void View::atlas_align_clicked ()
        {
          window().request_atlas_registration (true);
        }



        void View::atlas_filter_changed (const QString&)
        {
          update_atlas_list();
        }



        void View::update_atlas_list ()
        {
          auto& registration = window().atlas_registration();
          const QString filter = atlas_filter->text().trimmed();

          // Rebuilding wholesale would fight the user's ticks, so only the visible
          // set changes here; ticked state lives in atlas_loaded.
          atlas_list->blockSignals (true);
          atlas_list->clear();
          for (const auto& bundle : registration.catalogue()) {
            const QString name = qstr (bundle.name);
            // The category is searchable too, so "association" narrows to that
            // group even though the list shows bundle names alone.
            const QString category = qstr (bundle.category);
            if (filter.size() && !name.contains (filter, Qt::CaseInsensitive)
                              && !category.contains (filter, Qt::CaseInsensitive))
              continue;
            QListWidgetItem* item = new QListWidgetItem (name, atlas_list);
            item->setFlags (item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState (atlas_loaded.count (bundle.name) ? Qt::Checked : Qt::Unchecked);
            item->setToolTip (bundle.category.size() ? category : tr ("built-in atlas"));
          }
          atlas_list->blockSignals (false);
        }



        void View::atlas_state_changed ()
        {
          const auto& registration = window().atlas_registration();
          switch (registration.state()) {
            case AtlasRegistration::State::Idle:
              atlas_status->setText (tr ("not aligned"));
              break;
            case AtlasRegistration::State::Running:
              atlas_status->setText (tr ("aligning..."));
              break;
            case AtlasRegistration::State::Ready:
              atlas_status->setText (qstr (Path::basename (registration.registered_image()) +
                                           " - " + registration.note()));
              break;
            case AtlasRegistration::State::Failed:
              atlas_status->setText (qstr ("not aligned: " + registration.note()));
              break;
          }
          atlas_align_button->setEnabled (!registration.busy());
          atlas_list->setEnabled (registration.state() == AtlasRegistration::State::Ready);

          // A new registration invalidates whatever was loaded from the old one.
          if (registration.state() != AtlasRegistration::State::Ready && atlas_loaded.size()) {
            if (Tractography* tractography = get_tool<Tractography> (false)) {
              for (auto& loaded : atlas_loaded)
                tractography->remove_atlas_bundle (loaded.second);
            }
            atlas_loaded.clear();
          }
          update_atlas_list();
        }



        void View::atlas_context_menu (const QPoint& position)
        {
          QListWidgetItem* item = atlas_list->itemAt (position);
          if (!item)
            return;
          const std::string name = item->text().toStdString();
          auto loaded = atlas_loaded.find (name);
          if (loaded == atlas_loaded.end()) {
            // Nothing to colour until the bundle is actually shown.
            return;
          }

          QMenu menu (this);
          QAction* directional = menu.addAction (tr ("Colour by direction"));
          QAction* solid = menu.addAction (tr ("Set colour..."));
          QAction* chosen = menu.exec (atlas_list->mapToGlobal (position));
          if (!chosen)
            return;

          Tractography* tractography = get_tool<Tractography> (false);
          if (!tractography)
            return;
          if (chosen == directional) {
            tractography->set_atlas_bundle_colour (loaded->second, QColor());
          } else if (chosen == solid) {
            const QColor colour = QColorDialog::getColor (Qt::white, this, tr ("Bundle colour"));
            if (colour.isValid())
              tractography->set_atlas_bundle_colour (loaded->second, colour);
          }
        }



        void View::atlas_item_changed (QListWidgetItem* item)
        {
          if (!item)
            return;
          const std::string name = item->text().toStdString();
          auto& registration = window().atlas_registration();

          if (item->checkState() == Qt::Checked) {
            if (atlas_loaded.count (name))
              return;
            // Reading a bundle of an averaged atlas can take a moment: it is only
            // read when it is actually asked for.
            QApplication::setOverrideCursor (Qt::WaitCursor);
            const auto& tracks = registration.bundle (name);
            QApplication::restoreOverrideCursor();
            if (tracks.empty()) {
              item->setCheckState (Qt::Unchecked);
              return;
            }
            // The Tracts tool renders these but does not list them: ticking a
            // bundle here makes it visible, it does not add an entry there.
            // Creating that tool pops its dock open and steals focus from this
            // panel, which is not what ticking a checkbox here should do - so if it
            // had to be created, put it straight back out of sight.
            const bool tractography_was_open = get_tool<Tractography> (false) != nullptr;
            Tractography* tractography = get_tool<Tractography>();
            if (!tractography) {
              item->setCheckState (Qt::Unchecked);
              return;
            }
            if (!tractography_was_open) {
              for (QWidget* w = tractography; w; w = w->parentWidget()) {
                if (QDockWidget* dock = qobject_cast<QDockWidget*> (w)) {
                  dock->hide();
                  break;
                }
              }
            }
            try {
              MR::DWI::Tractography::Properties properties;
              properties["source"] = "built-in tract atlas";
              properties["atlas_bundle"] = name;
              Tractogram* tractogram = tractography->add_atlas_bundle (
                  tracks, properties, "atlas_" + name);
              if (tractogram)
                atlas_loaded[name] = tractogram;
              else
                item->setCheckState (Qt::Unchecked);
            } catch (Exception& e) {
              e.display();
              item->setCheckState (Qt::Unchecked);
            }
          } else {
            auto loaded = atlas_loaded.find (name);
            if (loaded == atlas_loaded.end())
              return;
            if (Tractography* tractography = get_tool<Tractography> (false))
              tractography->remove_atlas_bundle (loaded->second);
            atlas_loaded.erase (loaded);
          }
          window().updateGL();
        }



        void View::onImageChanged ()
        {
          const auto image = window().image();

          setEnabled (image);

          reset_light_box_gui_controls();

          if (!image)
            return;

          onScalingChanged();

          onImageVisibilityChanged(window().get_image_visibility());

          float rate = image->focus_rate();
          focus_x->setRate (rate);
          focus_y->setRate (rate);
          focus_z->setRate (rate);

          const int dim = image->image.ndim();
          volume_box->setVisible(dim > 3);

          while (volume_index_layout->count())
            delete volume_index_layout->takeAt (volume_index_layout->count()-1)->widget();

          for (size_t d = 3; d < image->image.ndim(); ++d) {
            SpinBox* vol_index = new SpinBox (this);
            vol_index->setMinimum (0);
            vol_index->setPrefix (qstr (str(d+1) + ": "));
            vol_index->setValue (image->image.index(d));
            vol_index->setMaximum (image->image.size(d) - 1);
            vol_index->setEnabled (image->image.size(d) > 1);
            volume_index_layout->addWidget (vol_index, volume_index_layout->count()/3, volume_index_layout->count()%3);
            connect (vol_index, SIGNAL (valueChanged(int)), this, SLOT (onSetVolumeIndex()));
          }

          lower_threshold_check_box->setChecked (image->use_discard_lower());
          upper_threshold_check_box->setChecked (image->use_discard_upper());
        }



        void View::onImageVisibilityChanged (bool visible)
        {
          hide_button->setChecked (!visible);
        }



        void View::hide_image_slot (bool flag)
        {
          if(!window().image())
            return;

          window().set_image_visibility(!flag);
        }

        void View::copy_focus_slot ()
        {
          if(!window().image())
            return;

          Eigen::VectorXf focus (window().focus());
          Eigen::IOFormat fmt(Eigen::FullPrecision, Eigen::DontAlignCols, ",", "\n", "", "", "", "");
          std::cout << focus.transpose().format(fmt) << "\n";

          QClipboard *clip = QApplication::clipboard();
          QString input = QString::fromStdString(str(focus.transpose().format(fmt)));
          clip->setText(input);
        }

        void View::copy_voxel_slot ()
        {
          if(!window().image())
            return;

          Eigen::VectorXf focus = window().image()->scanner2voxel() * window().focus();
          Eigen::IOFormat fmt(Eigen::FullPrecision, Eigen::DontAlignCols, ",", "\n", "", "", "", "");
          std::cout << focus.transpose().format(fmt) << "\n";

          QClipboard *clip = QApplication::clipboard();
          QString input = QString::fromStdString(str(focus.transpose().format(fmt)));
          clip->setText(input);
        }



        void View::onFocusChanged ()
        {
          if(!window().image())
            return;

          auto focus (window().focus());
          focus_x->setValue (focus[0]);
          focus_y->setValue (focus[1]);
          focus_z->setValue (focus[2]);

          focus = window().image()->scanner2voxel() * focus;
          voxel_x->setValue (focus[0]);
          voxel_y->setValue (focus[1]);
          voxel_z->setValue (focus[2]);
        }



        void View::onFOVChanged ()
        {
          fov->setValue (window().FOV());
          fov->setRate (FOV_RATE_MULTIPLIER * fov->value());
        }





        void View::onSetFocus ()
        {
          try {
            window().set_focus (Eigen::Vector3f { focus_x->value(), focus_y->value(), focus_z->value() } );
            window().updateGL();
          }
          catch (Exception) { }
        }




        void View::onSetVoxel ()
        {
          try {
            Eigen::Vector3f focus { voxel_x->value(), voxel_y->value(), voxel_z->value() };
            focus = window().image()->voxel2scanner() * focus;
            window().set_focus (focus);
            window().updateGL();
          }
          catch (Exception) { }
        }




        void View::onSetVolumeIndex ()
        {
          if (window().image()) {
            const auto& image (window().image()->image);
            assert (image.ndim() == size_t(volume_index_layout->count()+3));

            for (int i = 0; i < volume_index_layout->count(); ++i) {
              auto* box = dynamic_cast<SpinBox*> (volume_index_layout->itemAt(i)->widget());
              if (image.ndim() <= size_t(i+3))
                break;
              window().set_image_volume (i+3, box->value());
            }
          }
        }







        void View::onModeChanged ()
        {
          const Mode::Base* mode = window().get_current_mode();
          // Always shown. The alpha and intensity ramp belong to the image, not to a
          // mode: the slice modes leave them alone rather than being unable to hold
          // them, and hiding the controls made them look like a property of Volume
          // mode. It also stopped being true the moment Ortho grew a volume pane -
          // the mode is Ortho, the thing on screen is a volume render, and the
          // controls it needs were not there.
          transparency_box->setVisible (true);
          threshold_box->setVisible (true);
          clip_box->setVisible (mode->features & Mode::ShaderClipping);
          if (mode->features & Mode::ShaderClipping)
            clip_planes_selection_changed_slot();
          else
            window().register_camera_interactor();
          lightbox_box->setVisible (false);
          ortho_view_in_row_check_box->setVisible (false);
          mode->request_update_mode_gui(*this);
        }






        void View::onSetXray ()
        {
          Mode::Volume::xray_strength = 0.01f * float (xray->value());
          window().updateGL();
        }



        void View::onSetTransparency ()
        {
          assert (window().image());
          window().image()->transparent_intensity = transparent_intensity->value();
          window().image()->opaque_intensity = opaque_intensity->value();
          window().image()->alpha = get_alpha_from_slider (opacity->value());
          window().image()->lessthan = lower_threshold->value();
          window().image()->greaterthan = upper_threshold->value();
          window().updateGL();
        }





        void View::onPlaneChanged ()
        {
          plane_combobox->setCurrentIndex (window().plane());
        }





        void View::onSetPlane (int index)
        {
          window().set_plane (index);
          window().updateGL();
        }




        void View::onCheckThreshold (bool)
        {
          assert (window().image());
          assert (threshold_box->isEnabled());
          window().image()->set_use_discard_lower (lower_threshold_check_box->isChecked());
          window().image()->set_use_discard_upper (upper_threshold_check_box->isChecked());
          window().updateGL();
        }







        void View::set_transparency_from_image ()
        {
          if (!std::isfinite (window().image()->transparent_intensity) ||
              !std::isfinite (window().image()->opaque_intensity) ||
              !std::isfinite (window().image()->alpha) ||
              !std::isfinite (window().image()->lessthan) ||
              !std::isfinite (window().image()->greaterthan)) { // reset:
            if (!std::isfinite (window().image()->intensity_min()) ||
                !std::isfinite (window().image()->intensity_max()))
              return;

            if (!std::isfinite (window().image()->transparent_intensity))
              window().image()->transparent_intensity = window().image()->intensity_min();
            if (!std::isfinite (window().image()->opaque_intensity))
              window().image()->opaque_intensity = window().image()->intensity_max();
            if (!std::isfinite (window().image()->alpha))
              window().image()->alpha = get_alpha_from_slider (opacity->value());
            if (!std::isfinite (window().image()->lessthan))
              window().image()->lessthan = window().image()->intensity_min();
            if (!std::isfinite (window().image()->greaterthan))
              window().image()->greaterthan = window().image()->intensity_max();
          }

          assert (std::isfinite (window().image()->transparent_intensity));
          assert (std::isfinite (window().image()->opaque_intensity));
          assert (std::isfinite (window().image()->alpha));
          assert (std::isfinite (window().image()->lessthan));
          assert (std::isfinite (window().image()->greaterthan));

          transparent_intensity->setValue (window().image()->transparent_intensity);
          opaque_intensity->setValue (window().image()->opaque_intensity);
          opacity->setValue (get_slider_value_from_alpha (window().image()->alpha));
          lower_threshold->setValue (window().image()->lessthan);
          upper_threshold->setValue (window().image()->greaterthan);
          lower_threshold_check_box->setChecked (window().image()->use_discard_lower());
          upper_threshold_check_box->setChecked (window().image()->use_discard_upper());

          float rate = window().image() ? window().image()->scaling_rate() : 0.0;
          transparent_intensity->setRate (rate);
          opaque_intensity->setRate (rate);
          lower_threshold->setRate (rate);
          upper_threshold->setRate (rate);
        }




        void View::onSetScaling ()
        {
          if (window().image()) {
            window().image()->set_windowing (min_entry->value(), max_entry->value());
            window().updateGL();
          }
        }




        void View::onSetFOV ()
        {
          if (window().image()) {
            window().set_FOV (fov->value());
            fov->setRate (FOV_RATE_MULTIPLIER * fov->value());
            window().updateGL();
          }
        }



        void View::onScalingChanged ()
        {
          if (window().image()) {
            min_entry->setValue (window().image()->scaling_min());
            max_entry->setValue (window().image()->scaling_max());
            float rate = window().image()->scaling_rate();
            min_entry->setRate (rate);
            max_entry->setRate (rate);

            set_transparency_from_image();
          }
        }



        void View::clip_planes_right_click_menu_slot (const QPoint& pos)
        {
          QPoint globalPos = clip_planes_list_view->mapToGlobal (pos);
          QModelIndex index = clip_planes_list_view->indexAt (pos);
          clip_planes_list_view->selectionModel()->select (index, QItemSelectionModel::Select);

          clip_planes_option_menu->popup (globalPos);
        }



        void View::clip_planes_add_axial_slot ()
        {
          clip_planes_model->add (*(window().image()), 2);
          window().updateGL();
        }

        void View::clip_planes_add_sagittal_slot ()
        {
          clip_planes_model->add (*(window().image()), 0);
          window().updateGL();
        }

        void View::clip_planes_add_coronal_slot ()
        {
          clip_planes_model->add (*(window().image()), 1);
          window().updateGL();
        }



        void View::clip_planes_reset_axial_slot ()
        {
          QModelIndexList indices = clip_planes_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i)
            clip_planes_model->reset (indices[i], *(window().image()), 2);
          window().updateGL();
        }


        void View::clip_planes_reset_sagittal_slot ()
        {
          QModelIndexList indices = clip_planes_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i)
            clip_planes_model->reset (indices[i], *(window().image()), 0);
          window().updateGL();
        }


        void View::clip_planes_reset_coronal_slot ()
        {
          QModelIndexList indices = clip_planes_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i)
            clip_planes_model->reset (indices[i], *(window().image()), 1);
          window().updateGL();
        }




        void View::clip_planes_invert_slot ()
        {
          QModelIndexList indices = clip_planes_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i)
            clip_planes_model->invert (indices[i]);
          window().updateGL();
        }


        void View::clip_planes_remove_slot ()
        {
          QModelIndexList indices = clip_planes_list_view->selectionModel()->selectedIndexes();
          while (indices.size()) {
            clip_planes_model->remove (indices.first());
            indices = clip_planes_list_view->selectionModel()->selectedIndexes();
          }
          window().updateGL();
        }


        void View::clip_planes_clear_slot ()
        {
          clip_planes_model->clear();
          window().updateGL();
        }






        vector< std::pair<GL::vec4,bool> > View::get_active_clip_planes () const
        {
          vector< std::pair<GL::vec4,bool> > ret;
          QItemSelectionModel* selection = clip_planes_list_view->selectionModel();
          if (clip_box->isChecked()) {
            for (int i = 0; i < clip_planes_model->rowCount(); ++i) {
              if (clip_planes_model->planes[i].active) {
                std::pair<GL::vec4,bool> item;
                item.first = clip_planes_model->planes[i].plane;
                item.second = selection->isSelected (clip_planes_model->index (i,0));
                ret.push_back (item);
              }
            }
          }

          return ret;
        }

        vector<GL::vec4*> View::get_clip_planes_to_be_edited () const
        {
          vector<GL::vec4*> ret;
          if (clip_box->isChecked()) {
            QModelIndexList indices = clip_planes_list_view->selectionModel()->selectedIndexes();
            for (int i = 0; i < indices.size(); ++i)
              if (clip_planes_model->planes[indices[i].row()].active)
                ret.push_back (&clip_planes_model->planes[indices[i].row()].plane);
          }
          return ret;
        }

        bool View::get_cliphighlightstate () const
        {
          return clip_highlight_check_box->isChecked();
        }

        bool View::get_clipintersectionmodestate () const
        {
          return clip_intersectionmode_check_box->isChecked();
        }

        void View::clip_planes_selection_changed_slot ()
        {
          bool selected = clip_planes_list_view->selectionModel()->selectedIndexes().size();
          clip_planes_reset_submenu->setEnabled (selected);
          clip_planes_invert_action->setEnabled (selected);
          clip_planes_remove_action->setEnabled (selected);
          clip_planes_clear_action->setEnabled (clip_planes_model->rowCount());
          window().register_camera_interactor (selected ? this : nullptr);
          window().updateGL();
        }


        void View::clip_planes_toggle_shown_slot ()
        {
          window().updateGL();
        }

        void View::clip_planes_toggle_highlight_slot ()
        {
          window().updateGL();
        }

        void View::clip_planes_toggle_intersectionmode_slot ()
        {
          window().updateGL();
        }

        // Light box related functions

        void View::light_box_slice_inc_reset_slot()
        {
          reset_light_box_gui_controls();
        }

        void View::light_box_toggle_volumes_slot(bool)
        {
          reset_light_box_gui_controls();
        }



        void View::init_lightbox_gui (QLayout* parent)
        {
          using LightBoxEditButton = MRView::Mode::LightBoxViewControls::LightBoxEditButton;

          light_box_slice_inc = new AdjustButton(this);
          light_box_volume_inc = new LightBoxEditButton(this);
          light_box_rows = new LightBoxEditButton(this);
          light_box_cols = new LightBoxEditButton(this);

          light_box_slice_inc->setMinimumWidth(100);

          lightbox_box = new QGroupBox ("Light box");
          parent->addWidget (lightbox_box);
          GridLayout* grid_layout = new GridLayout;
          lightbox_box->setLayout(grid_layout);

          light_box_slice_inc_label = new QLabel (tr("Slice increment (mm):"));
          grid_layout->addWidget(light_box_slice_inc_label, 1, 0);
          grid_layout->addWidget(light_box_slice_inc, 1, 2);

          light_box_volume_inc_label = new QLabel (tr("Volume increment:"));
          grid_layout->addWidget(light_box_volume_inc_label, 1, 0);
          grid_layout->addWidget(light_box_volume_inc, 1, 2);

          grid_layout->addWidget(new QLabel (tr("Rows:")), 2, 0);
          grid_layout->addWidget(light_box_rows, 2, 2);

          grid_layout->addWidget (new QLabel (tr("Columns:")), 3, 0);
          grid_layout->addWidget(light_box_cols, 3, 2);

          light_box_show_4d = new QCheckBox(tr("Cycle through volumes"), this);
          grid_layout->addWidget(light_box_show_4d, 4, 0, 1, 2);

          light_box_show_grid = new QCheckBox(tr("Show grid"), this);
          grid_layout->addWidget(light_box_show_grid, 5, 0, 1, 2);
        }



        void View::reset_light_box_gui_controls()
        {
          if (!lightbox_box)
            return;

          bool img_4d = window ().image() && window ().image()->image.ndim() == 4;
          bool show_volumes = Mode::LightBox::get_show_volumes ();
          bool can_show_vol = img_4d && show_volumes;

          light_box_rows->setValue (static_cast<int>(Mode::LightBox::get_rows ()));
          light_box_cols->setValue (static_cast<int>(Mode::LightBox::get_cols ()));
          light_box_slice_inc->setValue (Mode::LightBox::get_slice_increment ());
          light_box_slice_inc->setRate (Mode::LightBox::get_slice_inc_adjust_rate ());
          light_box_volume_inc->setValue (Mode::LightBox::get_volume_increment ());
          light_box_show_grid->setChecked (Mode::LightBox::get_show_grid ());

          light_box_show_4d->setEnabled (img_4d);
          light_box_show_4d->setChecked (can_show_vol);
          light_box_slice_inc_label->setVisible (!can_show_vol);
          light_box_slice_inc->setVisible (!can_show_vol);
          light_box_volume_inc_label->setVisible (can_show_vol);
          light_box_volume_inc->setVisible (can_show_vol);
        }

        // Called in response to a request_update_mode_gui(ModeGuiVisitor& visitor) call
        void View::update_ortho_mode_gui (const Mode::Ortho & mode)
        {
          ortho_view_in_row_check_box->setVisible (true);
          ortho_view_in_row_check_box->setChecked (Mode::Ortho::show_as_row);
          connect (ortho_view_in_row_check_box, SIGNAL (toggled(bool)), &mode, SLOT (set_show_as_row_slot(bool)));
        }


        // Called in respose to a request_update_mode_gui(ModeGuiVisitor& visitor) call
        void View::update_lightbox_mode_gui (const Mode::LightBox &mode)
        {
          lightbox_box->setVisible(true);

          connect(&mode, SIGNAL (slice_increment_reset()), this, SLOT (light_box_slice_inc_reset_slot()));

          connect (light_box_rows, SIGNAL (valueChanged(int)), &mode, SLOT (nrows_slot(int)));
          connect (light_box_cols, SIGNAL (valueChanged(int)), &mode, SLOT (ncolumns_slot(int)));
          connect (light_box_slice_inc, SIGNAL (valueChanged(float)), &mode, SLOT (slice_inc_slot(float)));
          connect (light_box_volume_inc, SIGNAL (valueChanged(int)), &mode, SLOT (volume_inc_slot(int)));
          connect (light_box_show_grid, SIGNAL (toggled(bool)), &mode, SLOT (show_grid_slot(bool)));
          connect (light_box_show_4d, SIGNAL (toggled(bool)), &mode, SLOT (show_volumes_slot(bool)));
          connect (light_box_show_4d, SIGNAL (toggled(bool)), this, SLOT (light_box_toggle_volumes_slot(bool)));
          connect (&window(), SIGNAL (volumeChanged()), &mode, SLOT (image_volume_changed_slot()));

          reset_light_box_gui_controls();
        }

        void View::move_clip_planes_in_out (const ModelViewProjection& projection, vector<GL::vec4*>& clip, float distance)
        {
          Eigen::Vector3f d = projection.screen_normal();
          for (size_t n = 0; n < clip.size(); ++n) {
            GL::vec4& p (*clip[n]);
            p[3] += distance * (p[0]*d[0] + p[1]*d[1] + p[2]*d[2]);
          }
          window().updateGL();
        }


        void View::rotate_clip_planes (vector<GL::vec4*>& clip, const Eigen::Quaternionf& rot)
        {
          const auto& focus (window().focus());
          for (size_t n = 0; n < clip.size(); ++n) {
            GL::vec4& p (*clip[n]);
            float distance_to_focus = p[0]*focus[0] + p[1]*focus[1] + p[2]*focus[2] - p[3];
            const Eigen::Quaternionf norm (0.0f, p[0], p[1], p[2]);
            const Eigen::Quaternionf rotated = norm * rot;
            p[0] = rotated.x();
            p[1] = rotated.y();
            p[2] = rotated.z();
            p[3] = p[0]*focus[0] + p[1]*focus[1] + p[2]*focus[2] - distance_to_focus;
          }
          window().updateGL();
        }


        void View::deactivate ()
        {
          clip_planes_list_view->selectionModel()->clear();
        }


        bool View::slice_move_event (const ModelViewProjection& projection, float x)
        {
          vector<GL::vec4*> clip = get_clip_planes_to_be_edited();
          if (!clip.size()) return false;
          const auto &header = window().image()->header();
          float increment = x * std::pow (header.spacing (0) * header.spacing (1) * header.spacing (2), 1.0f/3.0f);
          move_clip_planes_in_out (projection, clip, increment);
          return true;
        }



        bool View::pan_event (const ModelViewProjection& projection)
        {
          vector<GL::vec4*> clip = get_clip_planes_to_be_edited();
          if (!clip.size()) return false;
          Eigen::Vector3f move = projection.screen_to_model_direction (window().mouse_displacement(), window().target());
          for (size_t n = 0; n < clip.size(); ++n) {
            GL::vec4& p (*clip[n]);
            p[3] += (p[0]*move[0] + p[1]*move[1] + p[2]*move[2]);
          }
          window().updateGL();
          return true;
        }


        bool View::panthrough_event (const ModelViewProjection& projection)
        {
          vector<GL::vec4*> clip = get_clip_planes_to_be_edited();
          if (!clip.size()) return false;
          move_clip_planes_in_out (projection, clip, MOVE_IN_OUT_FOV_MULTIPLIER * window().mouse_displacement().y() * window().FOV());
          return true;
        }



        bool View::tilt_event (const ModelViewProjection& projection)
        {
          vector<GL::vec4*> clip = get_clip_planes_to_be_edited();
          if (!clip.size()) return false;
          const Eigen::Quaternionf rot = window().get_current_mode()->get_tilt_rotation (projection);
          if (!rot.coeffs().allFinite())
            return true;
          rotate_clip_planes (clip, rot);
          return true;
        }



        bool View::rotate_event (const ModelViewProjection& projection)
        {
          vector<GL::vec4*> clip = get_clip_planes_to_be_edited();
          if (!clip.size()) return false;
          const Eigen::Quaternionf rot = window().get_current_mode()->get_rotate_rotation (projection);
          if (rot.coeffs().allFinite())
            rotate_clip_planes (clip, rot);
          return true;
        }

      }
    }
  }
}






