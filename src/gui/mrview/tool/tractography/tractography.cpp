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

#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QFontDatabase>
#include <QClipboard>
#include <QMessageBox>
#include <cctype>
#include <cstring>

#include "mrtrix.h"
#include "algo/loop.h"
#include "transform.h"
#include "timer.h"
#include <limits>
#include "file/path.h"
#include "gui/gui.h"
#include "dwi/tractography/file.h"
#include "dwi/tractography/file_trk_write.h"
#include "dwi/tractography/file_trx.h"
#include "dwi/tractography/file_trx_write.h"
#include "gui/mrview/window.h"
#include "gui/mrview/qthelpers.h"
#include "gui/mrview/colour_palette.h"
#include "gui/mrview/tool/tractography/tractography.h"

#include "file/ofstream.h"
#include <set>

#include "gui/mrview/tool/overlay.h"
#include "gui/mrview/tool/roi_editor/roi.h"

#include "dwi/tractography/recognition/cluster.h"
#include "dwi/tractography/recognition/refine.h"
#include "gui/mrview/atlas_registration.h"
#include "dwi/tractography/roi.h"
#include "gui/mrview/tool/tractography/bundle_stats.h"
#include "gui/dialog/file.h"
#include "gui/mrview/tool/list_model_base.h"
#include "gui/mrview/tool/tractography/track_scalar_file.h"
#include "gui/mrview/tool/tractography/tractogram.h"
#include "gui/opengl/lighting.h"
#include "gui/lighting_dock.h"

#include <memory>


namespace MR
{

  namespace
  {
    //! The population probability map for a bundle, opened against the atlas fit.
    /*! Null when the atlas has no map for it, which is ordinary rather than an error:
     *  the HCP1065 release covers 67 of the atlas's 102 bundles. The map stays in atlas
     *  space and subject points are taken back into it through the inverse fit, so
     *  nothing is resampled. */
    std::unique_ptr<DWI::Tractography::Recognition::PopulationMap>
    open_population_map (const std::string& bundle,
                         const GUI::MRView::AtlasRegistration& registration)
    {
      const std::string path = GUI::MRView::AtlasTemplate::population_map_path (bundle);
      if (path.empty())
        return nullptr;
      try {
        return std::unique_ptr<DWI::Tractography::Recognition::PopulationMap> (
            new DWI::Tractography::Recognition::PopulationMap (
                path, registration.fit().mni_to_subject.inverse()));
      } catch (Exception& e) {
        WARN ("could not read the population map \"" + path + "\": " + e[0]);
        return nullptr;
      }
    }
  }

  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        // The stops the strictness slider snaps to, as percentages of the matched
        // streamlines to drop. Evenly spaced on screen, one detent each; 0 leaves the
        // distance threshold in charge and 90 is the most a run is allowed to drop.
        // 50 is here because it is the default a run applies, so the control opens on
        // a landmark rather than between two.
        static const int strictness_landmarks[] = { 0, 25, 50, 75, 90 };
        static const int num_strictness_landmarks = 5;

        //! The landmark nearest a percentage, for showing a value that is not on one.
        static int strictness_landmark_index (int percent)
        {
          int best = 0;
          for (int i = 1; i != num_strictness_landmarks; ++i)
            if (std::abs (strictness_landmarks[i] - percent) < std::abs (strictness_landmarks[best] - percent))
              best = i;
          return best;
        }

        const char* tractogram_geometry_types[] = { "pseudotubes", "lines", "points", nullptr };

        TrackGeometryType geometry_index2type (const int idx)
        {
          switch (idx) {
            case 0: return TrackGeometryType::Pseudotubes;
            case 1: return TrackGeometryType::Lines;
            case 2: return TrackGeometryType::Points;
            default: assert (0); return TrackGeometryType::Pseudotubes;
          }
        }

        size_t geometry_string2index (std::string type_str)
        {
          type_str = lowercase (type_str);
          size_t index = 0;
          for (const char* const* p = tractogram_geometry_types; *p; ++p, ++index) {
            if (type_str == *p)
              return index;
          }
          throw Exception ("Unrecognised value for tractogram geometry \"" + type_str + "\" (options are: " + join(tractogram_geometry_types, ", ") + "); ignoring");
          return 0;
        }



        class Tractography::Model : public ListModelBase
        { MEMALIGN(Tractography::Model)

          public:
            Model (QObject* parent) :
              ListModelBase (parent) { }

            size_t colour_counter = 0;

            // Give a tractogram a distinct solid colour (so multiple tracts are
            // easy to tell apart). Users can switch back to directional colouring
            // from the colour combobox.
            void apply_solid_colour (Tractogram* t) {
              t->set_color_type (TrackColourType::Manual);
              t->set_colour (distinct_colour (colour_counter++));
            }

            // If the newly-added tractogram shares a basename with an existing
            // one, give them all distinct solid colours.
            void recolour_duplicates (Tractogram* added) {
              const std::string base = Path::basename (added->get_filename());
              bool duplicate = false;
              for (auto& item : items) {
                Tractogram* t = dynamic_cast<Tractogram*> (item.get());
                if (t && t != added && Path::basename (t->get_filename()) == base) {
                  duplicate = true;
                  if (t->get_color_type() == TrackColourType::Direction)
                    apply_solid_colour (t);
                }
              }
              if (duplicate)
                apply_solid_colour (added);
            }

            // Insert a fully-loaded tractogram into the list.
            void insert_tractogram (Tractogram* tractogram) {
              beginInsertRows (QModelIndex(), items.size(), items.size() + 1);
              items.push_back (std::unique_ptr<Displayable> (tractogram));
              endInsertRows();
            }

            void add_items (vector<std::string>& filenames,
                            Tractography& tractography_tool) {

              for (size_t i = 0; i < filenames.size(); ++i) {

                // A .trx that contains named groups is loaded as one tractogram
                // per group (each restricted to that group's streamlines).
                if (Path::has_suffix (filenames[i], ".trx")) {
                  vector<std::pair<std::string,std::string>> grps;
                  try { grps = DWI::Tractography::TRX_Data::groups (filenames[i]); }
                  catch (Exception& e) { e.display(); }
                  if (grps.size()) {
                    const std::string base = Path::basename (filenames[i]);
                    for (const auto& g : grps) {
                      try {
                        const vector<uint64_t> idx = DWI::Tractography::TRX_Data::read_uint (filenames[i], g.second);
                        vector<size_t> filter (idx.begin(), idx.end());
                        Tractogram* tractogram = new Tractogram (tractography_tool, filenames[i],
                                                                 base + " : " + g.first, filter);
                        try {
                          tractogram->load_tracks();
                          // Each group gets its own distinct solid colour.
                          apply_solid_colour (tractogram);
                          insert_tractogram (tractogram);
                        } catch (Exception& e) {
                          delete tractogram;
                          e.display();
                        }
                      } catch (Exception& e) {
                        e.display();
                      }
                    }
                    continue;
                  }
                }

                Tractogram* tractogram = new Tractogram (tractography_tool, filenames[i]);
                try {
                  tractogram->load_tracks();
                  // Auto-load a same-named sidecar scalar file (.txt or .tsf) in the
                  // same folder as the per-streamline threshold, if present.
                  {
                    const size_t dot = filenames[i].find_last_of ('.');
                    const std::string stem = (dot == std::string::npos) ? filenames[i] : filenames[i].substr (0, dot);
                    std::string sidecar;
                    if (Path::exists (stem + ".txt"))      sidecar = stem + ".txt";
                    else if (Path::exists (stem + ".tsf"))  sidecar = stem + ".tsf";
                    if (sidecar.size()) {
                      try {
                        tractogram->load_threshold_track_scalars (sidecar);
                        tractogram->threshold_scalar_filename = sidecar;
                        tractogram->set_threshold_type (TrackThresholdType::SeparateFile);
                        INFO ("auto-loaded tract threshold scalar \"" + sidecar + "\"");
                      } catch (Exception& e) {
                        e.display();
                      }
                    }
                  }
                  insert_tractogram (tractogram);
                  recolour_duplicates (tractogram);
                } catch (Exception& e) {
                  delete tractogram;
                  e.display();
                }
              }
            }

            Tractogram* get_tractogram (QModelIndex& index) {
              return dynamic_cast<Tractogram*>(items[index.row()].get());
            }
        };


        Tractography::Tractography (Dock* parent) :
          Base (parent),
          do_crop_to_slab (true),
          use_lighting (false),
          not_3D (true),
          line_opacity (1.0),
          scalar_file_options (nullptr),
          lighting_dock (nullptr) {

            slab_thickness = default_slab_thickness();

            VBoxLayout* main_box = new VBoxLayout (this);
            HBoxLayout* hlayout = new HBoxLayout;
            hlayout->setContentsMargins (0, 0, 0, 0);
            hlayout->setSpacing (0);

            QPushButton* button = new QPushButton (this);
            button->setToolTip (tr ("Open tractogram"));
            button->setIcon (QIcon (":/open.svg"));
            connect (button, SIGNAL (clicked()), this, SLOT (tractogram_open_slot ()));
            hlayout->addWidget (button, 1);

            button = new QPushButton (this);
            button->setToolTip (tr ("Close tractogram"));
            button->setIcon (QIcon (":/close.svg"));
            connect (button, SIGNAL (clicked()), this, SLOT (tractogram_close_slot ()));
            hlayout->addWidget (button, 1);

            hide_all_button = new QPushButton (this);
            hide_all_button->setToolTip (tr ("Hide all tractograms"));
            hide_all_button->setIcon (QIcon (":/hide.svg"));
            hide_all_button->setCheckable (true);
            connect (hide_all_button, SIGNAL (clicked()), this, SLOT (hide_all_slot ()));
            hlayout->addWidget (hide_all_button, 1);

            button = new QPushButton (this);
            button->setToolTip (tr ("Export selected tractograms (current threshold baked in)"));
            button->setIcon (QIcon (":/save.svg"));
            connect (button, SIGNAL (clicked()), this, SLOT (tractogram_export_slot ()));
            hlayout->addWidget (button, 1);

            main_box->addLayout (hlayout, 0);

            // Directly under the open/close row, so the batch show/hide controls sit
            // with the other list-wide actions rather than at the foot of the panel.
            HBoxLayout* checkall_layout = new HBoxLayout;
            QPushButton* check_all_button = new QPushButton (tr ("Check all"), this);
            check_all_button->setObjectName ("batchbtn");
            check_all_button->setToolTip (tr ("Show every tractogram by checking its box"));
            connect (check_all_button, &QPushButton::clicked, this, [this]{ tractogram_list_model->check_all(); window().updateGL(); });
            checkall_layout->addWidget (check_all_button, 1);
            QPushButton* uncheck_all_button = new QPushButton (tr ("Uncheck all"), this);
            uncheck_all_button->setObjectName ("batchbtn");
            uncheck_all_button->setToolTip (tr ("Hide every tractogram by unchecking its box"));
            connect (uncheck_all_button, &QPushButton::clicked, this, [this]{ tractogram_list_model->uncheck_all(); window().updateGL(); });
            checkall_layout->addWidget (uncheck_all_button, 1);
            main_box->addLayout (checkall_layout, 0);

            tractogram_list_view = new QListView (this);
            tractogram_list_view->setSelectionMode (QAbstractItemView::ExtendedSelection);
            tractogram_list_view->setDragEnabled (true);
            tractogram_list_view->setHorizontalScrollBarPolicy (Qt::ScrollBarAlwaysOff);
            tractogram_list_view->setTextElideMode (Qt::ElideLeft);
            tractogram_list_view->viewport()->setAcceptDrops (true);
            tractogram_list_view->setDropIndicatorShown (true);

            tractogram_list_model = new Model (this);
            tractogram_list_view->setModel (tractogram_list_model);

            connect (tractogram_list_model, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
                     this, SLOT (toggle_shown_slot (const QModelIndex&, const QModelIndex&)));

            connect (tractogram_list_view->selectionModel(),
                     SIGNAL (selectionChanged (const QItemSelection &, const QItemSelection &)),
                     SLOT (selection_changed_slot (const QItemSelection &, const QItemSelection &)));

            tractogram_list_view->setContextMenuPolicy (Qt::CustomContextMenu);
            connect (tractogram_list_view, SIGNAL (customContextMenuRequested (const QPoint&)),
                     this, SLOT (right_click_menu_slot (const QPoint&)));

            main_box->addWidget (tractogram_list_view, 1);

            hlayout = new HBoxLayout;
            hlayout->setContentsMargins (0, 0, 0, 0);
            hlayout->setSpacing (0);

            hlayout->addWidget (new QLabel ("color"));

            colour_combobox = new ComboBoxWithErrorMsg (this, "(variable)");
            colour_combobox->setToolTip (tr ("Set how this tractogram will be colored"));
            colour_combobox->addItem ("Direction");
            colour_combobox->addItem ("Endpoints");
            colour_combobox->addItem ("Random");
            colour_combobox->addItem ("Manual");
            colour_combobox->addItem ("File");
            colour_combobox->setEnabled (false);
            connect (colour_combobox, SIGNAL (activated(int)), this, SLOT (colour_mode_selection_slot (int)));
            hlayout->addWidget (colour_combobox);

            colour_button = new QColorButton;
            colour_button->setToolTip (tr ("Set the fixed colour to use for all tracks"));
            colour_button->setEnabled (false);
            connect (colour_button, SIGNAL (clicked()), this, SLOT (colour_button_slot()));
            hlayout->addWidget (colour_button);

            main_box->addLayout (hlayout);

            hlayout = new HBoxLayout;
            hlayout->setContentsMargins (0, 0, 0, 0);
            hlayout->setSpacing (0);

            hlayout->addWidget (new QLabel ("geometry"));

            geom_type_combobox = new ComboBoxWithErrorMsg (this, "(variable)");
            geom_type_combobox->setToolTip (tr ("Set the tractogram geometry type"));
            geom_type_combobox->addItem ("Pseudotubes");
            geom_type_combobox->addItem ("Lines");
            geom_type_combobox->addItem ("Points");
            connect (geom_type_combobox, SIGNAL (activated(int)), this, SLOT (geom_type_selection_slot (int)));
            hlayout->addWidget (geom_type_combobox);

            main_box->addLayout (hlayout);

            hlayout = new HBoxLayout;
            hlayout->setContentsMargins (0, 0, 0, 0);
            hlayout->setSpacing (0);

            thickness_label = new QLabel ("thickness");
            hlayout->addWidget (thickness_label);

            thickness_slider = new QSlider (Qt::Horizontal);
            thickness_slider->setRange (-1000,1000);
            thickness_slider->setSliderPosition (0);
            connect (thickness_slider, SIGNAL (valueChanged (int)), this, SLOT (line_thickness_slot (int)));
            set_slider_steps (thickness_slider);
            hlayout->addWidget (thickness_slider);

            main_box->addLayout (hlayout);

            scalar_file_options = new TrackScalarFileOptions (this);
            main_box->addWidget (scalar_file_options);

            // --- editing / selection / statistics ---
            edit_section = new Section (tr ("Edit && measure"), this, false);
            main_box->addWidget (edit_section);
            GridLayout* edit_grid = new GridLayout (edit_section->contents());

            edit_enable_box = new QCheckBox (tr ("enable editing"), this);
            edit_enable_box->setToolTip (
                tr ("Load the selected tractograms' streamlines into memory so they can be\n"
                    "selected, split and measured. This costs memory proportional to the\n"
                    "number of streamlines, so it is off by default."));
            connect (edit_enable_box, SIGNAL (toggled(bool)), this, SLOT (edit_enable_slot(bool)));
            edit_grid->addWidget (edit_enable_box, 0, 0, 1, 3);

            select_region_button = new QPushButton (tr ("Select by region..."), this);
            select_region_button->setToolTip (tr ("Keep or reject streamlines by their relationship "
                                                 "to a region from the Overlay, Atlas or ROI editor tool"));
            connect (select_region_button, SIGNAL (clicked()), this, SLOT (select_by_region_slot()));
            edit_grid->addWidget (select_region_button, 1, 0, 1, 2);
            invert_button = new QPushButton (tr ("Invert"), this);
            connect (invert_button, SIGNAL (clicked()), this, SLOT (invert_selection_slot()));
            edit_grid->addWidget (invert_button, 1, 2);

            select_all_button = new QPushButton (tr ("Select all"), this);
            connect (select_all_button, SIGNAL (clicked()), this, SLOT (select_all_streamlines_slot()));
            edit_grid->addWidget (select_all_button, 2, 0);
            keep_button = new QPushButton (tr ("Keep"), this);
            keep_button->setToolTip (tr ("Permanently discard everything not selected"));
            connect (keep_button, SIGNAL (clicked()), this, SLOT (keep_selection_slot()));
            edit_grid->addWidget (keep_button, 2, 1);
            delete_button = new QPushButton (tr ("Delete"), this);
            delete_button->setToolTip (tr ("Permanently discard the selected streamlines"));
            connect (delete_button, SIGNAL (clicked()), this, SLOT (delete_selection_slot()));
            edit_grid->addWidget (delete_button, 2, 2);

            split_button = new QPushButton (tr ("Split selection"), this);
            split_button->setToolTip (tr ("Copy the selected streamlines into a new tractogram"));
            connect (split_button, SIGNAL (clicked()), this, SLOT (split_selection_slot()));
            edit_grid->addWidget (split_button, 3, 0, 1, 3);

            rule_list = new QListWidget (this);
            rule_list->setToolTip (
                tr ("Region criteria currently applied to the selected tractogram.\n"
                    "These are re-evaluated whenever the region is edited, so drawing\n"
                    "more of an \"avoids\" region immediately deselects what it covers."));
            rule_list->setMaximumHeight (70);
            rule_list->setSelectionMode (QAbstractItemView::NoSelection);
            edit_grid->addWidget (rule_list, 5, 0, 1, 3);

            clear_rules_button = new QPushButton (tr ("Clear region criteria"), this);
            connect (clear_rules_button, SIGNAL (clicked()), this, SLOT (clear_rules_slot()));
            edit_grid->addWidget (clear_rules_button, 6, 0, 1, 3);

            edit_status_label = new QLabel ("");
            edit_status_label->setWordWrap (true);
            edit_grid->addWidget (edit_status_label, 7, 0, 1, 3);

            // Re-evaluating criteria means re-reading each region and testing every
            // streamline, so bursts of edits (a drag is many strokes) are coalesced
            // rather than recomputed for each one.
            rule_refresh_timer = new QTimer (this);
            rule_refresh_timer->setSingleShot (true);
            connect (rule_refresh_timer, SIGNAL (timeout()), this, SLOT (reapply_rules_slot()));
            connect (&window(), SIGNAL (regionsChanged()), this, SLOT (regions_changed_slot()));

            // --- refine the selected tract against its atlas bundle ---
            // A property of one tract, not of the tool: it re-derives that tract's
            // result from the candidates it already holds. Which is why it lives here
            // next to the list, and not in the generator - with several bundles
            // reconstructed in one run, a control in the generator cannot say which
            // of them it acts on.
            refine_section = new Section (tr ("Refine"), this, false);
            main_box->addWidget (refine_section);
            GridLayout* refine_grid = new GridLayout (refine_section->contents());

            QLabel* strictness_title = new QLabel (tr ("strictness"));
            strictness_title->setToolTip (
                tr ("Drop the streamlines furthest from the atlas bundle. Further right is\n"
                    "stricter and keeps fewer; fully left keeps everything the run matched.\n\n"
                    "Nothing is re-tracked: the streamlines this run rejected are still\n"
                    "here, so any setting is re-derived from them."));
            refine_grid->addWidget (strictness_title, 0, 0);
            refine_strictness = new QSlider (Qt::Horizontal, this);
            // The slider's units are *landmarks*, not percent: one detent per stop,
            // evenly spaced across the groove. A continuous groove offered a hundred
            // values that differ by a streamline or two, so a drag of one pixel
            // re-derived the tract for no visible change, and landing on a value
            // again after moving away was a matter of luck. Five stops can be hit
            // deliberately, and stepped through with the arrow keys.
            // strictness_landmarks[] holds what each stop means; 0 = keep everything
            // the run matched, 90 = keep only the closest tenth. Higher is stricter
            // and keeps less, which is the only way round the word reads.
            refine_strictness->setRange (0, num_strictness_landmarks - 1);
            // Its stops are already one apart; this is for the marks on the groove.
            set_slider_steps (refine_strictness, num_strictness_landmarks - 1);
            // The same default a run applies, so the slider opens where the tract
            // actually is rather than at a value it was never refined with.
            refine_strictness->setValue (strictness_landmark_index (0));
            refine_strictness->setToolTip (strictness_title->toolTip());
            refine_strictness_percent = 0;
            // Only emit on release: while the drag is live, update_refine_controls()
            // writes the slider back from the tract it is in the middle of changing,
            // which fights the drag.
            refine_strictness->setTracking (false);
            refine_grid->addWidget (refine_strictness, 0, 1);
            refine_count_label = new QLabel ("");
            refine_grid->addWidget (refine_count_label, 0, 2);

            refine_competitive = new QCheckBox (tr ("count a neighbouring bundle's better fit against a fibre"), this);
            refine_competitive->setToolTip (tr ("For each streamline, ask which atlas bundle it is closest to rather than only\nhow close it is to this one. A neighbour that fits it better does not delete\nit: it adds to the streamline's score, in proportion to how much better, so\nstrictness drops the contested ones first.\n\nMeasured on the projection category at 15 mm, 400 genuine and 400 bent\nstreamlines refined together: at 50% strictness all 400 genuine survive and 1\nof the bent ones does. Against the whole category, no medial lemniscus passes\nas corticospinal tract and no corticospinal streamline bent through the\nthalamus survives, while the tract itself keeps 400 of 400 - three more than\nrejecting outright ever kept."));
            refine_grid->addWidget (refine_competitive, 1, 0, 1, 2);
            refine_neighbours_button = new QPushButton (tr ("Neighbours..."), this);
            refine_neighbours_button->setToolTip (
                tr ("Choose which bundles compete for this tract's streamlines"));
            refine_grid->addWidget (refine_neighbours_button, 1, 2);

            QLabel* refine_prune_title = new QLabel (tr ("prune outliers"));
            refine_prune_title->setToolTip (
                tr ("Drop streamlines far from the rest of what was kept, measured against\n"
                    "this tract's own spread rather than a fixed distance."));
            refine_grid->addWidget (refine_prune_title, 2, 0);
            refine_prune = new QComboBox (this);
            refine_prune->addItem (tr ("off"));
            refine_prune->addItem (tr ("low"));
            refine_prune->addItem (tr ("medium"));
            refine_prune->addItem (tr ("high"));
            refine_prune->setToolTip (refine_prune_title->toolTip());
            refine_grid->addWidget (refine_prune, 2, 1);
            refine_revert_button = new QPushButton (tr ("Revert"), this);
            refine_revert_button->setToolTip (tr ("Back to the settings this run used"));
            refine_grid->addWidget (refine_revert_button, 2, 2);

            refine_status_label = new QLabel ("");
            refine_status_label->setWordWrap (true);
            refine_grid->addWidget (refine_status_label, 3, 0, 1, 3);

            // Dragging a slider emits a value per pixel; re-deriving on each would
            // queue work faster than it completes. One shot after the drag settles.
            refine_timer = new QTimer (this);
            refine_timer->setSingleShot (true);
            connect (refine_timer, SIGNAL (timeout()), this, SLOT (apply_refine_slot()));
            connect (refine_strictness, SIGNAL (valueChanged(int)), this, SLOT (strictness_moved_slot(int)));
            connect (refine_competitive, SIGNAL (toggled(bool)), this, SLOT (refine_setting_changed()));
            connect (refine_prune, SIGNAL (currentIndexChanged(int)), this, SLOT (refine_setting_changed()));
            connect (refine_neighbours_button, SIGNAL (clicked()), this, SLOT (refine_neighbours_slot()));
            connect (refine_revert_button, SIGNAL (clicked()), this, SLOT (refine_revert_slot()));

            QGroupBox* general_groupbox = new QGroupBox ("General options");
            GridLayout* general_opt_grid = new GridLayout;
            general_opt_grid->setContentsMargins (0, 0, 0, 0);
            general_opt_grid->setSpacing (0);

            general_groupbox->setLayout (general_opt_grid);

            opacity_slider = new QSlider (Qt::Horizontal);
            opacity_slider->setRange (1,1000);
            opacity_slider->setSliderPosition (1000);
            connect (opacity_slider, SIGNAL (valueChanged (int)), this, SLOT (opacity_slot (int)));
            general_opt_grid->addWidget (new QLabel ("opacity"), 0, 0);
            general_opt_grid->addWidget (opacity_slider, 0, 1);

            slab_group_box = new QGroupBox (tr("crop to slab"));
            slab_group_box->setCheckable (true);
            slab_group_box->setChecked (true);
            general_opt_grid->addWidget (slab_group_box, 4, 0, 1, 2);

            connect (slab_group_box, SIGNAL (clicked (bool)), this, SLOT (on_crop_to_slab_slot (bool)));

            GridLayout* slab_layout = new GridLayout;
            slab_group_box->setLayout(slab_layout);
            slab_layout->addWidget (new QLabel ("thickness (mm)"), 0, 0);
            slab_entry = new AdjustButton (this, 0.1);
            slab_entry->setValue (slab_thickness);
            slab_entry->setMin (0.0);
            connect (slab_entry, SIGNAL (valueChanged()), this, SLOT (on_slab_thickness_slot()));
            // The image can arrive - or be replaced by one with a different voxel
            // size - long after this panel was built.
            connect (&window(), SIGNAL (imageChanged()), this, SLOT (main_image_changed_slot()));
            slab_layout->addWidget (slab_entry, 0, 1);

            lighting_group_box = new QGroupBox (tr("use lighting"));
            lighting_group_box->setCheckable (true);
            lighting_group_box->setChecked (false);
            general_opt_grid->addWidget (lighting_group_box, 5, 0, 1, 2);

            connect (lighting_group_box, SIGNAL (clicked (bool)), this, SLOT (on_use_lighting_slot (bool)));

            VBoxLayout* lighting_layout = new VBoxLayout (lighting_group_box);
            lighting_button = new QPushButton ("Track lighting...");
            lighting_button->setIcon (QIcon (":/light.svg"));
            connect (lighting_button, SIGNAL (clicked()), this, SLOT (on_lighting_settings()));
            lighting_layout->addWidget (lighting_button);

            main_box->addWidget (general_groupbox, 0);

            lighting = new GL::Lighting (parent);
            lighting->diffuse = 0.8;
            lighting->shine = 5.0;
            connect (lighting, SIGNAL (changed()), SLOT (hide_all_slot()));


            QAction* action;
            track_option_menu = new QMenu ();
            action = new QAction("&Colour by direction", this);
            connect (action, SIGNAL(triggered()), this, SLOT (colour_track_by_direction_slot()));
            track_option_menu->addAction (action);
            action = new QAction("&Colour by track ends", this);
            connect (action, SIGNAL(triggered()), this, SLOT (colour_track_by_ends_slot()));
            track_option_menu->addAction (action);
            action = new QAction("&Randomise colour", this);
            connect (action, SIGNAL(triggered()), this, SLOT (randomise_track_colour_slot()));
            track_option_menu->addAction (action);
            action = new QAction("&Set colour", this);
            connect (action, SIGNAL(triggered()), this, SLOT (set_track_colour_slot()));
            track_option_menu->addAction (action);
            action = new QAction("&Colour by (track) scalar file", this);
            connect (action, SIGNAL(triggered()), this, SLOT (colour_by_scalar_file_slot()));
            track_option_menu->addAction (action);
            track_option_menu->addSeparator();
            action = new QAction("&Statistics...", this);
            action->setToolTip (tr ("Streamline count, length, curvature, span and bundle volume"));
            connect (action, SIGNAL(triggered()), this, SLOT (statistics_slot()));
            track_option_menu->addAction (action);
            action = new QAction("Along-tract &profile...", this);
            action->setToolTip (tr ("Sample an image at 100 points along the bundle and write the profile as CSV"));
            connect (action, SIGNAL(triggered()), this, SLOT (profile_slot()));
            track_option_menu->addAction (action);
            track_option_menu->addSeparator();
            combine_action = new QAction("Com&bine into one tract...", this);
            combine_action->setToolTip (tr ("Merge the selected tracts into one and close the originals"));
            connect (combine_action, SIGNAL(triggered()), this, SLOT (combine_tracts_slot()));
            track_option_menu->addAction (combine_action);
            action = new QAction("Cluster into &bundles...", this);
            action->setToolTip (tr ("Split into bundles by shape (QuickBundles); loads the streamlines into memory"));
            connect (action, SIGNAL(triggered()), this, SLOT (cluster_tracts_slot()));
            track_option_menu->addAction (action);
            action = new QAction("&Refine against an atlas bundle...", this);
            action->setToolTip (tr ("Keep the streamlines that belong to a named bundle, and drop "
                                    "the ones a neighbouring bundle fits better"));
            connect (action, SIGNAL(triggered()), this, SLOT (refine_tracts_slot()));
            track_option_menu->addAction (action);
            rejected_action = new QAction("Split streamlines &rejected by the distance metric", this);
            rejected_action->setToolTip (tr ("List what auto-tracking discarded on its shape distance, as its own tract"));
            connect (rejected_action, SIGNAL(triggered()), this, SLOT (split_rejected_slot()));
            track_option_menu->addAction (rejected_action);
            track_option_menu->addSeparator();
            action = new QAction("Show &endpoints as overlay", this);
            action->setToolTip (tr ("Map where the streamlines terminate, and list it in the Overlay tool"));
            connect (action, SIGNAL(triggered()), this, SLOT (endpoints_overlay_slot()));
            track_option_menu->addAction (action);

            //CONF option: MRViewDefaultTractGeomType
            //CONF default: Pseudotubes
            //CONF The default geometry type used to render tractograms.
            //CONF Options are Pseudotubes, Lines or Points
            const std::string default_geom_type = File::Config::get ("MRViewDefaultTractGeomType", tractogram_geometry_types[0]);
            try {
              const size_t default_geom_index = geometry_string2index (default_geom_type);
              Tractogram::default_tract_geom = geometry_index2type (default_geom_index);
              geom_type_combobox->setCurrentIndex (default_geom_index);
            } catch (Exception& e) {
              e.display();
            }

            // In the instance where pseudotubes are _not_ the default, enable lighting by default
            if (Tractogram::default_tract_geom != TrackGeometryType::Pseudotubes) {
              use_lighting = true;
              lighting_group_box->setChecked (true);
            }

            update_geometry_type_gui();

        }


        Tractography::~Tractography () {}


        Tractogram* Tractography::add_tractogram_from_memory (
            const vector<MR::DWI::Tractography::Streamline<float>>& tracks,
            const MR::DWI::Tractography::Properties& props,
            const std::string& display_name,
            uint64_t total_attempted,
            bool solid_colour)
        {
          Tractogram* tractogram = new Tractogram (*this, display_name, display_name);
          try {
            tractogram->load_tracks_from_memory (tracks, props, total_attempted);
            if (solid_colour)
              tractogram_list_model->apply_solid_colour (tractogram);
            tractogram_list_model->insert_tractogram (tractogram);
          } catch (Exception& e) {
            delete tractogram;
            throw;
          }
          tractogram_list_view->selectionModel()->clear();
          tractogram_list_view->selectionModel()->select (
              tractogram_list_model->index (tractogram_list_model->rowCount()-1, 0),
              QItemSelectionModel::Select);
          window().updateGL();
          return tractogram;
        }



        void Tractography::draw (const Projection& transform, bool is_3D, int, int)
        {
          GL::assert_context_is_current();
          not_3D = !is_3D;
          for (int i = 0; i < tractogram_list_model->rowCount(); ++i) {
            Tractogram* tractogram = dynamic_cast<Tractogram*>(tractogram_list_model->items[i].get());
            if (tractogram->show && !hide_all_button->isChecked())
              tractogram->render (transform);
          }
          // Atlas bundles are not in the list but are drawn on the same terms.
          for (auto& bundle : atlas_bundles) {
            if (bundle->show && !hide_all_button->isChecked())
              bundle->render (transform);
          }
          GL::assert_context_is_current();
        }


        void Tractography::draw_colourbars ()
        {
          if (hide_all_button->isChecked())
            return;

          for (int i = 0; i < tractogram_list_model->rowCount(); ++i) {
            Tractogram* tractogram = dynamic_cast<Tractogram*>(tractogram_list_model->items[i].get());
            if (tractogram->show && tractogram->get_color_type() == TrackColourType::ScalarFile && tractogram->intensity_scalar_filename.length())
              tractogram->request_render_colourbar (*scalar_file_options);
          }
        }



        size_t Tractography::visible_number_colourbars () {
           size_t total_visible(0);

           if (!hide_all_button->isChecked()) {
             for (size_t i = 0, N = tractogram_list_model->rowCount(); i < N; ++i) {
               Tractogram* tractogram = dynamic_cast<Tractogram*>(tractogram_list_model->items[i].get());
               if (tractogram->show && tractogram->get_color_type() == TrackColourType::ScalarFile && tractogram->intensity_scalar_filename.length())
                 total_visible += 1;
             }
           }

           return total_visible;
        }



        void Tractography::tractogram_open_slot ()
        {

          vector<std::string> list = Dialog::File::get_files (this, "Select tractograms to open", "Tractograms (*.tck *.trk *.trx *.dcm)", &current_folder);
          add_tractogram(list);
        }





        void Tractography::add_tractogram (vector<std::string>& list)
        {
          if (list.empty())
          { return; }
          try {
            tractogram_list_model->add_items (list, *this);
            select_last_added_tractogram();
          }
          catch (Exception& E) {
            E.display();
          }

        }



        namespace {
          // Defined further down, next to the export slots that also use it.
          void write_filtered_tracks (const Tractogram::FilteredTracks& ft, const std::string& path);

          //! A file name that stands for a tract without inheriting its punctuation.
          std::string session_stem (const std::string& name, size_t index)
          {
            std::string out;
            for (const char c : name)
              out += (std::isalnum (static_cast<unsigned char> (c)) || c == '_' || c == '-') ? c : '_';
            if (out.empty())
              out = "tract";
            return out + "_" + str (index);
          }
        }



        void Tractography::get_session (nlohmann::json& node) const
        {
          nlohmann::json entries = nlohmann::json::array();
          node = entries;
          // What the session writes out this time. Anything else left in the spill
          // directory belonged to a tract that has since been closed, and is removed
          // below - otherwise every generated tract ever made accumulates there.
          std::set<std::string> written;
          const std::string spill = Window::autosave_session_dir();

          for (size_t i = 0; i < tractogram_list_model->items.size(); ++i) {
            const Tractogram* t = dynamic_cast<const Tractogram*> (tractogram_list_model->items[i].get());
            if (!t)
              continue;
            nlohmann::json entry;
            const std::string& path = t->get_filename();
            if (Path::is_file (path)) {
              // A tract read from disk is restored from disk; the file is the user's
              // and nothing is copied.
              entry["file"] = path;
            }
            else if (spill.size()) {
              // A generated tract exists only in this process. Dropping it - which is
              // what used to happen, silently - throws away the expensive half of a
              // session: the tracking. So it is written out beside the session file.
              try {
                const std::string stem = session_stem (t->display_name(), i);
                const std::string kept = Path::join (spill, stem + ".tck");
                Tractogram::FilteredTracks ft;
                t->get_filtered_streamlines (ft);
                write_filtered_tracks (ft, kept);
                entry["file"] = kept;
                entry["generated"] = true;
                written.insert (kept);

                // The streamlines this tract's run rejected are what strictness
                // re-derives from, so without them a restored tract is frozen at
                // whatever it was left at. They are kept in a second file rather than
                // merged, since which is which is the whole point.
                const vector<MR::DWI::Tractography::Streamline<float>>& rejected = t->rejected_tracks();
                if (rejected.size()) {
                  const std::string dropped = Path::join (spill, stem + ".rejected.tck");
                  Tractogram::FilteredTracks rft;
                  rft.tracks = rejected;
                  rft.source_name = ft.source_name;
                  write_filtered_tracks (rft, dropped);
                  entry["rejected"] = dropped;
                  written.insert (dropped);
                }
              }
              catch (Exception& E) {
                E.display();
                continue;
              }
            }
            else {
              continue;
            }

            entry["name"] = t->display_name();
            // The refinement is provenance, not data: a handful of numbers that say
            // what this tract was recognised as. Cheap to carry, and without it a
            // restored tract cannot be re-derived at another strictness even with its
            // candidates present.
            const Tractogram::Refinement& r = t->refinement();
            if (r.valid) {
              nlohmann::json refine;
              refine["bundle"] = r.bundle;
              refine["competitive"] = r.options.competitive;
              if (std::isfinite (r.options.keep_fraction))
                refine["keep_fraction"] = r.options.keep_fraction;
              if (std::isfinite (r.options.outlier_k))
                refine["outlier_k"] = r.options.outlier_k;
              refine["per_node_outliers"] = r.options.per_node_outliers;
              refine["neighbours"] = r.neighbours;
              refine["kept"] = uint64_t (r.kept);
              refine["candidates"] = uint64_t (r.candidates);
              entry["refine"] = refine;
            }
            entries.push_back (entry);
          }

          // Remove the spill files this session wrote that it no longer needs - a
          // tract that has since been closed. Only its own: sweeping the directory
          // instead would delete the *previous* session's tracts two minutes after
          // launch, before anyone had the chance to restore them.
          for (const std::string& f : session_spill_files) {
            if (written.count (f))
              continue;
            try { File::remove (f); }
            catch (Exception&) { }   // a spill file left behind is not worth an error
          }
          session_spill_files = std::move (written);

          // Which sections were folded open. Small, but it is the difference between
          // a panel that comes back as you left it and one that comes back shut.
          node = nlohmann::json::object();
          node["tracts"] = entries;
          node["open_sections"] = { { "refine", refine_section->is_open() },
                                    { "edit", edit_section->is_open() } };
        }



        void Tractography::set_session (const nlohmann::json& node)
        {
          // A bare array of track files, or - written by the build in which the
          // generator was hosted inside this panel - an object holding them under
          // "tracts". Window lifts that build's "trackgen" back to the top level
          // before this runs, so there is nothing to do with it here.
          if (node.is_object()) {
            if (node.find ("open_sections") != node.end()) {
              const nlohmann::json& open = node["open_sections"];
              if (open.find ("refine") != open.end())
                refine_section->set_open (open["refine"].get<bool>());
              if (open.find ("edit") != open.end())
                edit_section->set_open (open["edit"].get<bool>());
            }
            if (node.find ("tracts") != node.end())
              set_session (node["tracts"]);
            return;
          }
          if (!node.is_array())
            return;
          // One file at a time: add_tractogram() hands the whole list to add_items(),
          // which throws on the first file it cannot read - so a single missing .tck
          // used to discard every other tract in the session.
          vector<std::string> skipped;
          for (const auto& f : node) {
            // Older sessions hold a bare path per tract; current ones hold an object,
            // because a generated tract needs its name, its rejected streamlines and
            // what it was recognised as carried with it.
            const bool detailed = f.is_object();
            if (!detailed && !f.is_string())
              continue;
            const std::string path = detailed
                ? (f.find ("file") != f.end() ? f["file"].get<std::string>() : std::string())
                : f.get<std::string>();
            if (path.empty() || !Path::is_file (path)) {
              skipped.push_back (path.size() ? path : std::string ("(no file)"));
              continue;
            }
            vector<std::string> one (1, path);
            try {
              tractogram_list_model->add_items (one, *this);
            } catch (Exception&) {
              skipped.push_back (path);
              continue;
            }
            if (!detailed || !tractogram_list_model->rowCount())
              continue;
            Tractogram* t = dynamic_cast<Tractogram*> (
                tractogram_list_model->items[tractogram_list_model->rowCount()-1].get());
            if (!t)
              continue;
            if (f.find ("name") != f.end())
              t->set_filename (f["name"].get<std::string>());   // the list's label
            // Adopted: a restored spill file now belongs to this session, so closing
            // the tract removes it on the next save, and leaving it does not.
            if (f.value ("generated", false))
              session_spill_files.insert (path);
            if (f.find ("rejected") != f.end()) {
              const std::string dropped = f["rejected"].get<std::string>();
              session_spill_files.insert (dropped);
              try {
                if (Path::is_file (dropped)) {
                  vector<MR::DWI::Tractography::Streamline<float>> candidates;
                  MR::DWI::Tractography::Properties props;
                  MR::DWI::Tractography::Reader<float> reader (dropped, props);
                  MR::DWI::Tractography::Streamline<float> tck;
                  while (reader (tck))
                    candidates.push_back (tck);
                  t->set_rejected_tracks (candidates);
                }
              }
              catch (Exception&) { }   // the candidates are a bonus, not the tract
            }
            if (f.find ("refine") != f.end()) {
              const nlohmann::json& r = f["refine"];
              Tractogram::Refinement& provenance = t->refinement();
              if (r.find ("bundle") != r.end())
                provenance.bundle = r["bundle"].get<std::string>();
              if (r.find ("competitive") != r.end())
                provenance.options.competitive = r["competitive"].get<bool>();
              provenance.options.keep_fraction = r.find ("keep_fraction") != r.end()
                                               ? r["keep_fraction"].get<float>() : NaN;
              provenance.options.outlier_k = r.find ("outlier_k") != r.end()
                                           ? r["outlier_k"].get<float>() : NaN;
              if (r.find ("per_node_outliers") != r.end())
                provenance.options.per_node_outliers = r["per_node_outliers"].get<bool>();
              if (r.find ("neighbours") != r.end())
                provenance.neighbours = r["neighbours"].get<vector<std::string>>();
              if (r.find ("kept") != r.end())
                provenance.kept = r["kept"].get<uint64_t>();
              if (r.find ("candidates") != r.end())
                provenance.candidates = r["candidates"].get<uint64_t>();
              provenance.original = provenance.options;
              // Only usable if the atlas can still supply the bundle to measure
              // against - the settings restore either way, but re-deriving needs the
              // reference, and saying so up front beats failing when the slider moves.
              provenance.valid = provenance.bundle.size()
                              && window().atlas_registration().has_bundle (provenance.bundle);
            }
          }
          if (tractogram_list_model->rowCount())
            select_last_added_tractogram();
          if (skipped.size())
            WARN ("session: " + str(skipped.size()) + " tract file(s) could not be restored: "
                  + join (skipped, ", "));
        }





        void Tractography::dropEvent (QDropEvent* event)
        {
          static constexpr int max_files = 32;

          const QMimeData* mimeData = event->mimeData();
          if (mimeData->hasUrls()) {
            vector<std::string> list;
            QList<QUrl> urlList = mimeData->urls();
            for (int i = 0; i < urlList.size() && i < max_files; ++i) {
                list.push_back (QtHelpers::url_to_std_string (urlList.at (i)));
            }
            try {
              tractogram_list_model->add_items (list, *this);
              window().updateGL();
            }
            catch (Exception& e) {
              e.display();
            }
            event->acceptProposedAction();
          }
        }


        void Tractography::tractogram_close_slot ()
        {
          GL::Context::Grab context;
          QModelIndexList indexes = tractogram_list_view->selectionModel()->selectedIndexes();
          while (indexes.size()) {
            tractogram_list_model->remove_item (indexes.first());
            indexes = tractogram_list_view->selectionModel()->selectedIndexes();
          }
          scalar_file_options->set_tractogram (nullptr);
          scalar_file_options->update_UI();
          window().updateGL();
        }


        Tractogram* Tractography::add_atlas_bundle (
            const vector<MR::DWI::Tractography::Streamline<float>>& tracks,
            const MR::DWI::Tractography::Properties& properties,
            const std::string& display_name)
        {
          if (tracks.empty())
            return nullptr;
          GL::Context::Grab context;
          std::unique_ptr<Tractogram> tractogram (new Tractogram (*this, display_name, display_name));
          tractogram->load_tracks_from_memory (tracks, properties, tracks.size());
          tractogram->set_color_type (TrackColourType::Direction);
          tractogram->show = true;
          Tractogram* raw = tractogram.get();
          atlas_bundles.push_back (std::move (tractogram));
          window().updateGL();
          return raw;
        }



        bool Tractography::has_tractogram_named (const std::string& name) const
        {
          for (size_t i = 0; i != tractogram_list_model->items.size(); ++i) {
            const Tractogram* t = dynamic_cast<const Tractogram*> (tractogram_list_model->items[i].get());
            if (t && Path::basename (t->get_filename()) == name)
              return true;
          }
          return false;
        }



        Tractogram* Tractography::find_tractogram_named (const std::string& name)
        {
          // The display name, which is what the user sees and edits, rather than the
          // source path: a tract generated in memory has no path.
          for (size_t i = 0; i != tractogram_list_model->items.size(); ++i) {
            Tractogram* t = dynamic_cast<Tractogram*> (tractogram_list_model->items[i].get());
            if (t && t->display_name() == name)
              return t;
          }
          return nullptr;
        }



        bool Tractography::select_tractogram_named (const std::string& name)
        {
          for (size_t i = 0; i != tractogram_list_model->items.size(); ++i) {
            Tractogram* t = dynamic_cast<Tractogram*> (tractogram_list_model->items[i].get());
            if (!t || t->display_name() != name)
              continue;
            const QModelIndex index = tractogram_list_model->index (int (i), 0);
            tractogram_list_view->selectionModel()->clearSelection();
            tractogram_list_view->selectionModel()->select (index, QItemSelectionModel::Select);
            tractogram_list_view->setCurrentIndex (index);
            return true;
          }
          return false;
        }



        void Tractography::cluster_selected ()
        {
          cluster_tracts_slot();
        }



        void Tractography::begin_manual_editing ()
        {
          // setChecked already emits toggled, which is connected to edit_enable_slot;
          // calling it again would toggle editing straight back off.
          edit_enable_box->setChecked (true);
          // Editing happens in the viewer with this tool's controls, so bring it
          // forward rather than leaving the caller's panel in front.
          for (QWidget* w = this; w; w = w->parentWidget()) {
            if (QDockWidget* dock = qobject_cast<QDockWidget*> (w)) {
              dock->show();
              dock->raise();
              break;
            }
          }
        }



        void Tractography::remove_atlas_bundle (Tractogram* tractogram)
        {
          if (!tractogram)
            return;
          GL::Context::Grab context;
          for (auto it = atlas_bundles.begin(); it != atlas_bundles.end(); ++it) {
            if (it->get() == tractogram) {
              atlas_bundles.erase (it);
              window().updateGL();
              return;
            }
          }
        }



        void Tractography::apply_distinct_colour (Tractogram* tractogram)
        {
          if (!tractogram)
            return;
          GL::Context::Grab context;
          tractogram_list_model->apply_solid_colour (tractogram);
          window().updateGL();
        }



        void Tractography::set_atlas_bundle_colour (Tractogram* tractogram, const QColor& colour)
        {
          if (!tractogram)
            return;
          GL::Context::Grab context;
          if (colour.isValid()) {
            tractogram->set_color_type (TrackColourType::Manual);
            tractogram->set_colour (colour);
          } else {
            tractogram->set_color_type (TrackColourType::Direction);
          }
          window().updateGL();
        }



        bool Tractography::contains (const Tractogram* tractogram) const
        {
          if (!tractogram)
            return false;
          for (size_t i = 0; i != tractogram_list_model->items.size(); ++i) {
            if (tractogram_list_model->items[i].get() == tractogram)
              return true;
          }
          return false;
        }



        void Tractography::remove_tractogram (Tractogram* tractogram)
        {
          if (!tractogram)
            return;
          for (int row = 0; row != tractogram_list_model->rowCount(); ++row) {
            QModelIndex index = tractogram_list_model->index (row, 0);
            if (tractogram_list_model->get_tractogram (index) != tractogram)
              continue;
            GL::Context::Grab context;
            // The scalar-file panel may be pointing at what we are about to delete.
            scalar_file_options->set_tractogram (nullptr);
            scalar_file_options->update_UI();
            tractogram_list_model->remove_item (index);
            window().updateGL();
            return;
          }
        }



        namespace {

          // Write a single filtered tractogram to one file, dispatched by suffix.
          // (.trx here writes a standalone file with a single group.)
          void write_filtered_tracks (const Tractogram::FilteredTracks& ft, const std::string& path)
          {
            if (Path::has_suffix (path, ".tck")) {
              DWI::Tractography::Properties props;
              DWI::Tractography::Writer<float> writer (path, props);
              for (const auto& tck : ft.tracks)
                writer (tck);
            }
            else if (Path::has_suffix (path, ".trk")) {
              DWI::Tractography::TRKWriter writer (path);
              for (const auto& tck : ft.tracks)
                writer (tck);
              writer.close();
            }
            else if (Path::has_suffix (path, ".trx")) {
              DWI::Tractography::TRXWriter writer (path);
              writer.begin_group (ft.source_name);
              for (size_t i = 0; i != ft.tracks.size(); ++i)
                writer.add (ft.tracks[i],
                            ft.per_vertex     ? &ft.dpv[i] : nullptr,
                            ft.per_streamline ? &ft.dps[i] : nullptr);
              writer.close();
            }
            else
              throw Exception ("unsupported tractography output format for \"" + path
                               + "\" (use .tck, .trk or .trx)");
          }

          std::string strip_known_suffix (std::string name)
          {
            for (const char* ext : { ".tck", ".trk", ".trx" }) {
              if (Path::has_suffix (name, ext)) {
                name = name.substr (0, name.size() - std::strlen (ext));
                break;
              }
            }
            return name;
          }

          //! Streamlines for a read-only analysis, without retaining anything.
          /*! Editing keeps a CPU copy so a selection can be tracked against it, which
           *  is why it is opt-in - a whole-brain tractogram is hundreds of megabytes.
           *  Statistics, profiles and clustering only need to *read* the streamlines,
           *  so they borrow the editing cache when it happens to exist and otherwise
           *  fill a scratch buffer that dies with the caller. */
          const vector<MR::DWI::Tractography::Streamline<float>>& tract_streamlines (
              Tractogram* t, Tractogram::FilteredTracks& scratch)
          {
            if (t->editing_enabled())
              return t->cpu_tracks();
            t->get_filtered_streamlines (scratch);
            return scratch.tracks;
          }

          //! Suggested export name for a tractogram, without extension.
          std::string suggested_export_name (const Tractogram* t)
          {
            // The display name, so a rename in the list carries through to the save
            // dialog rather than the original file's name reappearing.
            std::string name = strip_known_suffix (Path::basename (t->display_name()));

            // Which algorithm produced it matters when comparing runs, and it is
            // not otherwise visible in a .tck's name.
            const std::string method = t->tracking_method();
            if (method.size() && name.find (method) == std::string::npos)
              name += "_" + method;

            // Only when a threshold is actually in force: the suffix used to be
            // unconditional, which said "thresholded" about untouched tracts.
            if (t->get_threshold_type() != TrackThresholdType::None)
              name += "_thresholded";
            return name;
          }
        }


        void Tractography::tractogram_export_slot ()
        {
          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          vector<Tractogram*> selected;
          for (QModelIndex idx : indices)
            if (Tractogram* t = tractogram_list_model->get_tractogram (idx))
              selected.push_back (t);

          if (selected.empty()) {
            QMessageBox::information (this, "Export tractography",
                "Please select one or more tractograms to export.");
            return;
          }

          try {
            if (selected.size() == 1) {
              // Single tractogram: plain save in any supported format.
              const std::string suggested = suggested_export_name (selected[0]) + ".tck";
              const std::string out_path = Dialog::File::get_save_name (this,
                  "Export tractogram", suggested, "Tractograms (*.tck *.trk *.trx)");
              if (out_path.empty())
                return;
              Tractogram::FilteredTracks ft;
              selected[0]->get_filtered_streamlines (ft);
              write_filtered_tracks (ft, out_path);
              QMessageBox::information (this, "Export tractography",
                  qstr (str (ft.tracks.size()) + " streamlines exported to:\n" + out_path));
              return;
            }

            const Dialog::File::MultiSaveChoice choice =
                Dialog::File::ask_multi_save_mode (this, str(selected.size()) + " tractograms",
                    { ".tck", ".trk", ".trx" });
            if (choice.mode == Dialog::File::MultiSaveMode::Cancel)
              return;

            if (choice.mode == Dialog::File::MultiSaveMode::SingleFile) {
              // A single combined file must be .trx (only format with groups).
              // Named after the selection, not a generic "tractograms".
              const std::string stem = suggested_export_name (selected[0])
                                     + "_plus" + str (selected.size() - 1);
              std::string out_path = Dialog::File::get_save_name (this,
                  "Export tractograms as a single .trx", stem + ".trx", "TRX (*.trx)");
              if (out_path.empty())
                return;
              if (!Path::has_suffix (out_path, ".trx"))
                out_path += ".trx";
              DWI::Tractography::TRXWriter writer (out_path);
              size_t total = 0;
              for (Tractogram* t : selected) {
                Tractogram::FilteredTracks ft;
                t->get_filtered_streamlines (ft);
                writer.begin_group (ft.source_name);
                for (size_t i = 0; i != ft.tracks.size(); ++i)
                  writer.add (ft.tracks[i],
                              ft.per_vertex     ? &ft.dpv[i] : nullptr,
                              ft.per_streamline ? &ft.dps[i] : nullptr);
                total += ft.tracks.size();
              }
              writer.close();
              QMessageBox::information (this, "Export tractography",
                  qstr (str (total) + " streamlines from " + str (selected.size())
                        + " tractograms exported to:\n" + out_path));
            }
            else {
              // Individual files, one .tck per tractogram, into the chosen folder.
              const std::string folder = Dialog::File::get_folder (this, "Select folder for exported tractograms");
              if (folder.empty())
                return;
              size_t total = 0;
              for (Tractogram* t : selected) {
                Tractogram::FilteredTracks ft;
                t->get_filtered_streamlines (ft);
                write_filtered_tracks (ft, Path::join (folder, suggested_export_name (t) + choice.extension));
                total += ft.tracks.size();
              }
              QMessageBox::information (this, "Export tractography",
                  qstr (str (total) + " streamlines from " + str (selected.size())
                        + " tractograms exported to:\n" + folder));
            }
          }
          catch (Exception& E) {
            E.display();
            QMessageBox::critical (this, "Export tractography",
                qstr ("Export failed:\n" + std::string (E[0])));
          }
        }


        void Tractography::toggle_shown_slot (const QModelIndex& index, const QModelIndex& index2)
        {
          if (index.row() == index2.row()) {
            tractogram_list_view->setCurrentIndex(index);
          } else {
            for (size_t i = 0; i < tractogram_list_model->items.size(); ++i) {
              if (tractogram_list_model->items[i]->show) {
                tractogram_list_view->setCurrentIndex (tractogram_list_model->index (i, 0));
                break;
              }
            }
          }
          window().updateGL();
        }


        void Tractography::hide_all_slot ()
        {
          window().updateGL();
        }


        void Tractography::on_crop_to_slab_slot (bool is_checked)
        {
          do_crop_to_slab = is_checked;

          for (size_t i = 0, N = tractogram_list_model->rowCount(); i < N; ++i) {
            Tractogram* tractogram = dynamic_cast<Tractogram*>(tractogram_list_model->items[i].get());
            tractogram->should_update_stride = true;
          }

          window().updateGL();
        }


        void Tractography::on_use_lighting_slot (bool is_checked)
        {
          use_lighting = is_checked;
          window().updateGL();
        }


        void Tractography::on_lighting_settings ()
        {
          if (!lighting_dock) {
            lighting_dock = new LightingDock("Tractogram lighting", *lighting);
            window().addDockWidget (Qt::RightDockWidgetArea, lighting_dock);
          }
          lighting_dock->show();
        }


        //! One voxel of the main image, which is what the slab is meant to show.
        /*! The slab crops the tracts to what lies near the slice on screen, so its
         *  natural unit is the slice's own thickness. It used to be two voxels, and
         *  it was computed once when the panel was first built - with a 2.5 mm
         *  fallback if no image was open yet, which is where a 5 mm slab over 1 mm
         *  data came from: the panel had been opened before the image, and nothing
         *  ever recomputed it. */
        float Tractography::default_slab_thickness () const
        {
          const auto image = window().image();
          if (!image)
            return 2.5f;
          return (image->header().spacing(0) +
                  image->header().spacing(1) +
                  image->header().spacing(2)) / 3.0f;
        }



        void Tractography::main_image_changed_slot ()
        {
          // A slab set by hand is the user's number and stays; otherwise it follows
          // whichever image is now on screen, including the first one to arrive.
          if (slab_thickness_user_set)
            return;
          const float thickness = default_slab_thickness();
          if (thickness == slab_thickness)
            return;
          slab_thickness = thickness;
          slab_entry->blockSignals (true);
          slab_entry->setValue (slab_thickness);
          slab_entry->blockSignals (false);
          window().updateGL();
        }



        void Tractography::on_slab_thickness_slot()
        {
          slab_thickness = slab_entry->value();
          slab_thickness_user_set = true;
          window().updateGL();
        }


        void Tractography::opacity_slot (int opacity)
        {
          line_opacity = Math::pow2(static_cast<float>(opacity)) / 1.0e6f;
          window().updateGL();
        }


        void Tractography::line_thickness_slot (int thickness)
        {
          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i)  {
            Tractogram* tractogram = tractogram_list_model->get_tractogram (indices[i]);
            tractogram->line_thickness = thickness;
            tractogram->should_update_stride = true;
          }

          window().updateGL();
        }


        vector<Tractogram*> Tractography::selected_tractograms ()
        {
          vector<Tractogram*> out;
          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          for (QModelIndex idx : indices)
            if (Tractogram* t = tractogram_list_model->get_tractogram (idx))
              out.push_back (t);
          return out;
        }



        void Tractography::update_edit_controls ()
        {
          auto selected = selected_tractograms();
          bool any_editing = false;
          size_t total = 0, chosen = 0;
          for (Tractogram* t : selected) {
            if (!t->editing_enabled())
              continue;
            any_editing = true;
            total += t->num_cpu_tracks();
            for (const uint8_t f : t->selection())
              chosen += f ? 1 : 0;
          }
          // Starting a selection turns editing on by itself, so those three only need
          // a tract to work on; the three that consume a selection still need one to
          // exist.
          const bool any_selected = selected.size();
          for (QPushButton* b : { select_region_button, invert_button, select_all_button })
            b->setEnabled (any_selected);
          for (QPushButton* b : { keep_button, delete_button, split_button })
            b->setEnabled (any_editing);
          update_refine_controls();
          // Reflect editing that an operation turned on, without re-entering the slot.
          edit_enable_box->blockSignals (true);
          edit_enable_box->setChecked (any_editing);
          edit_enable_box->blockSignals (false);
          if (any_editing)
            edit_status_label->setText (QString ("%1 of %2 streamlines selected")
                .arg (uint64_t (chosen)).arg (uint64_t (total)));
          else
            edit_status_label->setText ("");
        }



        void Tractography::edit_enable_slot (bool enable)
        {
          auto selected = selected_tractograms();
          if (selected.empty()) {
            QMessageBox::information (this, "Edit tractogram",
                "Select one or more tractograms in the list first.");
            edit_enable_box->setChecked (false);
            return;
          }
          try {
            for (Tractogram* t : selected) {
              if (enable) {
                QApplication::setOverrideCursor (Qt::WaitCursor);
                t->enable_editing();
                QApplication::restoreOverrideCursor();
              } else {
                t->clear_selection();
                t->disable_editing();
              }
            }
          } catch (Exception& e) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning (this, "Edit tractogram", qstr (e[0]));
            edit_enable_box->setChecked (false);
          }
          update_edit_controls();
          window().updateGL();
        }



        void Tractography::select_by_region_slot ()
        {
          auto selected = selected_tractograms();
          if (selected.empty())
            return;

          vector<RegionRef> available;
          collect_regions (available);

          // "passes through" keeps streamlines that touch the region;
          // "avoids" keeps those that do not.
          QMenu menu (this);

          // Draw a fresh region without leaving this panel. Useful for "avoids" in
          // particular: create it, then paint where you do not want streamlines.
          QMenu* fresh = menu.addMenu (tr ("New region (ROI editor)..."));
          fresh->addAction (tr ("passes through"))->setData (qstr (std::string ("in:<new>")));
          fresh->addAction (tr ("avoids"))->setData (qstr (std::string ("out:<new>")));
          if (available.size())
            menu.addSeparator();
          // Regions painted or loaded by the user come first, inline. The atlas
          // bundles - a hundred of them - go behind one entry, so selecting by a
          // hand-drawn ROI stays a two-click operation instead of a scroll through
          // the whole atlas.
          vector<RegionRef> own, atlas;
          for (const auto& region : available)
            (region.name.find ('/') == std::string::npos ? own : atlas).push_back (region);

          auto add_choices = [] (QMenu* sub, const RegionRef& region) {
            sub->addAction (tr ("passes through"))->setData (qstr ("in:" + region.key));
            sub->addAction (tr ("avoids"))->setData (qstr ("out:" + region.key));
          };
          build_region_menu (menu, own, add_choices);
          if (atlas.size()) {
            menu.addSeparator();
            QMenu* atlas_menu = menu.addMenu (tr ("Atlas bundle (%1)").arg (atlas.size()));
            build_region_menu (*atlas_menu, atlas, add_choices);
          }
          QAction* chosen = menu.exec (select_region_button->mapToGlobal (
              QPoint (0, select_region_button->height())));
          if (!chosen)
            return;

          const std::string data = chosen->data().toString().toStdString();
          const bool want_inside = data.compare (0, 3, "in:") == 0;
          const std::string key = data.substr (want_inside ? 3 : 4);

          RegionRef created;
          const RegionRef* region = nullptr;
          if (key == "<new>") {
            try {
              ROI* roi_tool = get_tool<ROI>();
              if (!roi_tool)
                throw Exception ("could not open the ROI editor");
              created = roi_tool->create_region();
              region = &created;
            } catch (Exception& e) {
              QMessageBox::warning (this, "New region", qstr (e[0]));
              return;
            }
          } else {
            for (const auto& candidate : available)
              if (candidate.key == key)
                region = &candidate;
          }
          if (!region)
            return;

          try {
            QApplication::setOverrideCursor (Qt::WaitCursor);
            for (Tractogram* t : selected) {
              QApplication::restoreOverrideCursor();
              const bool ready = ensure_editing (t);
              QApplication::setOverrideCursor (Qt::WaitCursor);
              if (!ready)
                continue;
              // Record the criterion and evaluate it, rather than baking the
              // result in: the region may be edited afterwards.
              t->add_selection_rule (*region, want_inside);
              t->apply_selection_rules();
            }
            QApplication::restoreOverrideCursor();
          } catch (Exception& e) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning (this, "Select by region", qstr (e[0]));
          }
          apply_region_opacities();
          refresh_rule_list();
          update_edit_controls();
          window().updateGL();
        }



        void Tractography::refresh_rule_list ()
        {
          rule_list->clear();
          bool any = false;
          for (Tractogram* t : selected_tractograms()) {
            if (!t->editing_enabled())
              continue;
            for (const auto& rule : t->selection_rules()) {
              QListWidgetItem* item = new QListWidgetItem (
                  qstr (rule.region.name) + (rule.want_inside ? "  -  passes through" : "  -  avoids"),
                  rule_list);
              item->setToolTip (qstr (rule.region.label()));
              QPixmap swatch (12, 12);
              swatch.fill (rule.region.colour);
              item->setIcon (QIcon (swatch));
              any = true;
            }
          }
          clear_rules_button->setEnabled (any);
          rule_list->setVisible (true);
        }



        void Tractography::apply_region_opacities (const vector<RegionRef>& released)
        {
          // A region dimmed as "avoid" for one tractogram stays dimmed while any
          // tractogram still avoids it; anything else goes back to full opacity.
          std::set<std::string> avoided, referenced;
          for (size_t i = 0; i != tractogram_list_model->items.size(); ++i) {
            Tractogram* t = dynamic_cast<Tractogram*> (tractogram_list_model->items[i].get());
            if (!t)
              continue;
            for (const auto& rule : t->selection_rules()) {
              referenced.insert (rule.region.key);
              if (!rule.want_inside)
                avoided.insert (rule.region.key);
            }
          }

          vector<RegionRef> all (released);
          {
            vector<RegionRef> current;
            collect_regions (current);
            for (const auto& region : current)
              all.push_back (region);
          }
          std::set<std::string> done;
          for (const auto& region : all) {
            if (!done.insert (region.key).second)
              continue;
            if (!referenced.count (region.key) && !released.size())
              continue;   // never touched by us: leave the user's own opacity alone
            if (RegionProvider* provider = provider_for (region))
              provider->set_region_opacity (region, avoided.count (region.key) ? avoid_region_opacity : 1.0f);
          }
        }



        void Tractography::clear_rules_slot ()
        {
          vector<RegionRef> released;
          for (Tractogram* t : selected_tractograms()) {
            if (!t->editing_enabled())
              continue;
            for (const auto& rule : t->selection_rules())
              released.push_back (rule.region);
            t->clear_selection_rules();
            try { t->apply_selection_rules(); }
            catch (Exception& e) { e.display(); }
          }
          apply_region_opacities (released);
          refresh_rule_list();
          update_edit_controls();
          window().updateGL();
        }



        void Tractography::regions_changed_slot ()
        {
          // Only arm the timer if something actually depends on a region.
          for (size_t i = 0; i != tractogram_list_model->items.size(); ++i) {
            Tractogram* t = dynamic_cast<Tractogram*> (tractogram_list_model->items[i].get());
            if (t && t->editing_enabled() && t->selection_rules().size()) {
              rule_refresh_timer->start (200);
              return;
            }
          }
        }



        void Tractography::reapply_rules_slot ()
        {
          // A region was edited, added or removed somewhere: re-evaluate every
          // tractogram that has standing criteria, not just the selected ones, so
          // the displayed selection never silently goes stale.
          size_t unavailable = 0;
          bool any = false;
          for (size_t i = 0; i != tractogram_list_model->items.size(); ++i) {
            Tractogram* t = dynamic_cast<Tractogram*> (tractogram_list_model->items[i].get());
            if (!t || !t->editing_enabled() || t->selection_rules().empty())
              continue;
            try {
              unavailable += t->apply_selection_rules();
              any = true;
            } catch (Exception& e) {
              e.display();
            }
          }
          if (!any)
            return;
          refresh_rule_list();
          update_edit_controls();
          if (unavailable)
            edit_status_label->setText (QString ("%1 region criteria could not be evaluated "
                                                "(region empty or its tool closed)").arg (uint64_t (unavailable)));
          window().updateGL();
        }



        void Tractography::invert_selection_slot ()
        {
          for (Tractogram* t : selected_tractograms()) {
            if (!ensure_editing (t))
              continue;
            vector<uint8_t> flags = t->selection();
            for (auto& f : flags)
              f = f ? 0 : 1;
            try { t->set_selection (flags); }
            catch (Exception& e) { e.display(); }
            // A hand-made selection replaces the criteria; keeping them would mean
            // the next region edit silently undid this.
            t->clear_selection_rules();
          }
          refresh_rule_list();
          update_edit_controls();
          window().updateGL();
        }



        void Tractography::select_all_streamlines_slot ()
        {
          for (Tractogram* t : selected_tractograms()) {
            if (!ensure_editing (t))
              continue;
            vector<uint8_t> flags (t->num_cpu_tracks(), 1);
            try { t->set_selection (flags); }
            catch (Exception& e) { e.display(); }
            t->clear_selection_rules();
          }
          refresh_rule_list();
          update_edit_controls();
          window().updateGL();
        }



        void Tractography::keep_selection_slot ()
        {
          for (Tractogram* t : selected_tractograms()) {
            if (!t->editing_enabled())
              continue;
            try { t->apply_selection (true); }
            catch (Exception& e) { QMessageBox::warning (this, "Keep selection", qstr (e[0])); }
          }
          update_edit_controls();
          window().updateGL();
        }



        void Tractography::delete_selection_slot ()
        {
          for (Tractogram* t : selected_tractograms()) {
            if (!t->editing_enabled())
              continue;
            try { t->apply_selection (false); }
            catch (Exception& e) { QMessageBox::warning (this, "Delete selection", qstr (e[0])); }
          }
          update_edit_controls();
          window().updateGL();
        }



        void Tractography::split_selection_slot ()
        {
          for (Tractogram* t : selected_tractograms()) {
            if (!t->editing_enabled())
              continue;
            auto tracks = t->selected_tracks();
            if (tracks.empty()) {
              QMessageBox::information (this, "Split selection", "Nothing is selected.");
              continue;
            }
            MR::DWI::Tractography::Properties props;
            props["split_from"] = t->display_name();
            try {
              // No spaces or brackets: this name becomes a filename on export.
              add_tractogram_from_memory (tracks, props,
                  strip_known_suffix (Path::basename (t->display_name())) + "_selection", tracks.size());
            } catch (Exception& e) {
              QMessageBox::warning (this, "Split selection", qstr (e[0]));
            }
          }
          update_edit_controls();
        }



        bool Tractography::ensure_editing (Tractogram* t)
        {
          // Selection genuinely needs the retained CPU copy - it is what the flags
          // index into. Turn it on rather than refusing: the checkbox is there to
          // release the memory again, not to be a precondition the user has to know.
          if (t->editing_enabled())
            return true;
          try {
            QApplication::setOverrideCursor (Qt::WaitCursor);
            t->enable_editing();
            QApplication::restoreOverrideCursor();
          } catch (Exception& e) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning (this, "Edit tractogram", qstr (e[0]));
            return false;
          }
          return true;
        }



        void Tractography::split_rejected_slot ()
        {
          for (Tractogram* t : selected_tractograms()) {
            const auto& rejected = t->rejected_tracks();
            if (rejected.empty())
              continue;
            const std::string name = strip_known_suffix (Path::basename (t->display_name()));
            MR::DWI::Tractography::Properties props;
            props["split_from"] = t->display_name();
            props["rejected_by"] = "shape distance to the atlas bundle";
            try {
              // A distinct solid colour: this is meant to be compared against the
              // tract it was rejected from, which is directionally coloured.
              add_tractogram_from_memory (rejected, props, name + "_deleted", rejected.size(), true);
            } catch (Exception& e) {
              QMessageBox::warning (this, "Rejected streamlines", qstr (e[0]));
            }
          }
          window().updateGL();
        }



        void Tractography::combine_tracts_slot ()
        {
          vector<Tractogram*> targets = selected_tractograms();
          if (targets.size() < 2) {
            QMessageBox::information (this, "Combine tracts",
                "Select two or more tracts in the list to combine.");
            return;
          }

          std::string suggested = strip_known_suffix (Path::basename (targets[0]->display_name()));
          suggested += "_and_" + str (targets.size() - 1) + "_more";
          bool ok = false;
          const QString name = QInputDialog::getText (this, tr ("Combine tracts"),
              tr ("Name for the combined tract (the %1 originals are closed):").arg (targets.size()),
              QLineEdit::Normal, qstr (suggested), &ok);
          if (!ok || name.trimmed().isEmpty())
            return;

          MR::Timer clock;
          vector<MR::DWI::Tractography::Streamline<float>> merged;
          std::string sources;
          try {
            QApplication::setOverrideCursor (Qt::WaitCursor);
            for (Tractogram* t : targets) {
              Tractogram::FilteredTracks scratch;
              const auto& tracks = tract_streamlines (t, scratch);
              merged.insert (merged.end(), tracks.begin(), tracks.end());
              sources += (sources.size() ? ", " : "") + Path::basename (t->display_name());
            }
            QApplication::restoreOverrideCursor();
          } catch (Exception& e) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning (this, "Combine tracts", qstr (e[0]));
            return;
          }

          if (merged.empty()) {
            QMessageBox::information (this, "Combine tracts", "Those tracts hold no streamlines.");
            return;
          }

          try {
            MR::DWI::Tractography::Properties props;
            props["combined_from"] = sources;
            props["combined_count"] = str (targets.size());
            add_tractogram_from_memory (merged, props, name.trimmed().toStdString(), merged.size());
            // Only now that the merge exists: closing first would lose the
            // streamlines if creating it failed.
            for (Tractogram* t : targets)
              remove_tractogram (t);
          } catch (Exception& e) {
            QMessageBox::warning (this, "Combine tracts", qstr (e[0]));
            return;
          }

          edit_status_label->setText (QString ("%1 streamlines from %2 tracts combined in %3 s")
              .arg (uint64_t (merged.size())).arg (targets.size()).arg (clock.elapsed(), 0, 'f', 2));
          update_edit_controls();
          window().updateGL();
        }



        void Tractography::refresh_tract_controls ()
        {
          update_edit_controls();     // which ends by refreshing the Refine group
        }



        void Tractography::update_refine_controls ()
        {
          vector<Tractogram*> selected = selected_tractograms();
          Tractogram* single = selected.size() == 1 ? selected[0] : nullptr;
          const bool usable = single && single->refinement().valid;

          refine_section->set_title (usable
              ? tr ("Refine \"%1\"").arg (qstr (single->display_name()))
              : tr ("Refine"));
          for (QWidget* w : { (QWidget*) refine_strictness, (QWidget*) refine_competitive,
                              (QWidget*) refine_prune, (QWidget*) refine_neighbours_button,
                              (QWidget*) refine_revert_button })
            w->setEnabled (usable);

          if (!usable) {
            refine_count_label->setText ("");
            // Say which of the three reasons it is, rather than leaving a dead panel.
            refine_status_label->setText (
                selected.empty() ? tr ("Select one tract to refine it.")
              : selected.size() > 1 ? tr ("Select a single tract: refining compares one tract "
                                          "against one atlas bundle.")
              : tr ("This tract was not recognised against an atlas bundle, so there is nothing "
                    "to re-derive it from. Use \"Refine against an atlas bundle...\" on the "
                    "right-click menu to give it one."));
            return;
          }

          // Loading the controls must not read as the user having changed them.
          const Tractogram::Refinement& r = single->refinement();
          refine_loading = true;
          if (!refine_strictness->isSliderDown()) {
            // The tract's own value is what is applied; the slider shows the nearest
            // landmark to it. A tract refined at 30% from the dialog therefore reads
            // as 25 without being changed to it - nothing is re-derived until the
            // slider is actually moved.
            refine_strictness_percent = std::isfinite (r.options.keep_fraction)
                ? int (std::lround (100.0f * (1.0f - r.options.keep_fraction))) : 0;
            refine_strictness->setValue (strictness_landmark_index (refine_strictness_percent));
          }
          refine_competitive->setChecked (r.options.competitive);
          refine_prune->setCurrentIndex (
              !std::isfinite (r.options.outlier_k) ? 0
            : r.options.outlier_k >= 3.5f ? 1
            : r.options.outlier_k >= 2.75f ? 2 : 3);
          refine_loading = false;

          // Streamlines currently shown, out of what the run had to choose from.
          // num_cpu_tracks() is only populated while editing is on, so the displayed
          // count comes from the tractogram itself.
          refine_count_label->setText (r.candidates
              ? tr ("%1% \u00b7 %2 / %3").arg (refine_strictness_percent)
                    .arg (uint64_t (r.kept)).arg (uint64_t (r.candidates))
              : tr ("%1%").arg (refine_strictness_percent));
          QString neighbours = r.neighbours.size()
              ? tr ("%1 chosen").arg (uint64_t (r.neighbours.size()))
              : tr ("automatic");
          refine_status_label->setText (tr ("against atlas %1; competing bundles: %2")
              .arg (qstr (r.bundle)).arg (neighbours));
        }



        void Tractography::strictness_moved_slot (int landmark)
        {
          // Loading the controls moves the slider to the nearest landmark; that must
          // not overwrite the tract's own value with the landmark's.
          if (refine_loading)
            return;
          // The slider carries an index; what gets applied is the percentage it
          // stands for. Kept in a member so a value that is not on a landmark -
          // one set by the Refine dialog - survives until the slider is moved.
          refine_strictness_percent = strictness_landmarks[
              std::min (std::max (landmark, 0), num_strictness_landmarks - 1)];
          refine_setting_changed();
        }



        void Tractography::refine_setting_changed ()
        {
          if (refine_loading)
            return;
          // With tracking off the slider only reports on release, so this is one
          // shot per gesture rather than one per pixel. The delay is still worth
          // keeping: the combo box and the checkbox emit immediately, and a run of
          // them (revert, then a prune change) should re-derive once.
          refine_timer->start (100);
        }



        void Tractography::apply_refine_slot ()
        {
          vector<Tractogram*> selected = selected_tractograms();
          if (selected.size() != 1 || !selected[0]->refinement().valid)
            return;
          Tractogram* t = selected[0];
          Tractogram::Refinement& r = t->refinement();
          r.options.competitive = refine_competitive->isChecked();
          const int strictness = refine_strictness_percent;
          r.options.keep_fraction = strictness > 0 ? 1.0f - float (strictness) / 100.0f : NaN;
          // Same calibration as a run uses; see collect_refine_options in trackgen.cpp.
          r.options.per_node_outliers = true;
          switch (refine_prune->currentIndex()) {
            case 1:  r.options.outlier_k = 4.0f; break;
            case 2:  r.options.outlier_k = 3.0f; break;
            case 3:  r.options.outlier_k = 2.5f; break;
            default: r.options.outlier_k = NaN;  break;
          }
          apply_refinement (t);
          update_refine_controls();
        }



        void Tractography::refine_revert_slot ()
        {
          vector<Tractogram*> selected = selected_tractograms();
          if (selected.size() != 1 || !selected[0]->refinement().valid)
            return;
          Tractogram::Refinement& r = selected[0]->refinement();
          r.options = r.original;
          r.neighbours.clear();
          apply_refinement (selected[0]);
          update_refine_controls();
        }



        void Tractography::apply_refinement (Tractogram* t)
        {
          using namespace MR::DWI::Tractography::Recognition;
          Tractogram::Refinement& r = t->refinement();
          if (!r.valid)
            return;

          const vector<MR::DWI::Tractography::Streamline<float>> candidates = t->refine_candidates();
          if (candidates.empty()) {
            refine_status_label->setText (tr ("nothing left to re-derive from"));
            return;
          }

          try {
            QApplication::setOverrideCursor (Qt::WaitCursor);
            auto& registration = window().atlas_registration();
            const vector<MR::DWI::Tractography::Streamline<float>> reference =
                registration.bundle (r.bundle);
            if (reference.empty())
              throw Exception ("atlas bundle \"" + r.bundle + "\" could not be read");

            vector<Competitor> competitors;
            if (r.options.competitive) {
              vector<std::string> wanted = r.neighbours;
              if (wanted.empty()) {
                if (!AtlasTemplate::footprints_ready())
                  AtlasTemplate::load_footprint_cache();
                wanted = AtlasTemplate::competitors_for (r.bundle);
              }
              for (const std::string& other : wanted) {
                try {
                  const auto& neighbour = registration.bundle (other);
                  if (neighbour.size())
                    competitors.push_back ({ other, neighbour });
                } catch (Exception&) { }
              }
            }

            MR::Timer clock;
            vector<MR::DWI::Tractography::Streamline<float>> kept, rejected;
            RefineReport report;
            auto population = open_population_map (r.bundle, registration);
            refine_bundle (reference, r.match, r.options, competitors, candidates,
                           kept, rejected, report, nullptr, population.get());
            QApplication::restoreOverrideCursor();

            if (kept.empty()) {
              QMessageBox::information (this, "Refine",
                  qstr ("Nothing would be kept.\n\n" + report.summary()
                        + "\n\nRaise the strictness slider, or turn off pruning."));
              return;
            }

            // In place, so the tract keeps its identity, colour and list position -
            // this runs on every drag of the slider.
            const bool was_editing = t->editing_enabled();
            t->reload_from_memory (kept, candidates.size());
            t->set_rejected_tracks (rejected);
            if (was_editing)
              t->enable_editing();
            r.kept = kept.size();
            r.candidates = candidates.size();
            window().updateGL();
            refine_status_label->setText (QString ("%1 (%2 s)")
                .arg (qstr (report.summary())).arg (clock.elapsed(), 0, 'f', 2));
          } catch (Exception& e) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning (this, "Refine", qstr (e[0]));
          }
        }



        void Tractography::refine_neighbours_slot ()
        {
          vector<Tractogram*> selected = selected_tractograms();
          if (selected.size() != 1 || !selected[0]->refinement().valid)
            return;
          Tractogram::Refinement& r = selected[0]->refinement();

          if (!AtlasTemplate::footprints_ready())
            AtlasTemplate::load_footprint_cache();
          // Wider than a run uses, so a bundle can be added by hand.
          vector<std::string> offered = AtlasTemplate::competitors_for (r.bundle, 0.05f, 40);
          if (offered.empty()) {
            QMessageBox::information (this, "Neighbours",
                "No neighbouring bundles are known for this bundle yet. It is in no atlas "
                "category, and the territory index is built the first time a run needs it.");
            return;
          }
          const vector<std::string> current = r.neighbours.size()
              ? r.neighbours : AtlasTemplate::competitors_for (r.bundle);

          QDialog dialog (this);
          dialog.setWindowTitle (tr ("Competing bundles"));
          VBoxLayout* layout = new VBoxLayout (&dialog);
          layout->addWidget (new QLabel (tr (
              "A streamline is dropped when one of these bundles fits it better than\n"
              "\"%1\" does. Ticked by default are every other bundle of its own class,\n"
              "then any bundle from elsewhere that runs through the same territory.").arg (qstr (r.bundle)), &dialog));
          QListWidget* list = new QListWidget (&dialog);
          for (const std::string& candidate : offered) {
            QListWidgetItem* item = new QListWidgetItem (qstr (candidate), list);
            item->setFlags (item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState (std::find (current.begin(), current.end(), candidate) != current.end()
                                 ? Qt::Checked : Qt::Unchecked);
          }
          list->setMinimumHeight (240);
          layout->addWidget (list);
          QDialogButtonBox* buttons = new QDialogButtonBox (
              QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
          connect (buttons, SIGNAL (accepted()), &dialog, SLOT (accept()));
          connect (buttons, SIGNAL (rejected()), &dialog, SLOT (reject()));
          layout->addWidget (buttons);
          if (dialog.exec() != QDialog::Accepted)
            return;

          r.neighbours.clear();
          for (int i = 0; i != list->count(); ++i)
            if (list->item(i)->checkState() == Qt::Checked)
              r.neighbours.push_back (list->item(i)->text().toStdString());
          apply_refinement (selected[0]);
          update_refine_controls();
        }



        void Tractography::refine_tracts_slot ()
        {
          using namespace MR::DWI::Tractography::Recognition;

          vector<Tractogram*> targets = selected_tractograms();
          if (targets.empty())
            return;

          auto& registration = window().atlas_registration();
          const auto& atlas_catalogue = registration.catalogue();
          if (atlas_catalogue.empty()) {
            QMessageBox::information (this, "Refine against an atlas bundle",
                "No tract atlas is available. Load an FOD image so the atlas can be aligned "
                "to this subject, or point MRViewTractAtlasPath at a directory of bundles.");
            return;
          }

          // A tract opened from a file carries no atlas bundle, so which bundle it is
          // meant to be has to be asked. Same two-step picker the Track generation
          // panel uses - a hundred bundles is too many for one flat list.
          QDialog dialog (this);
          dialog.setWindowTitle (tr ("Refine against an atlas bundle"));
          VBoxLayout* layout = new VBoxLayout (&dialog);
          layout->addWidget (new QLabel (tr (
              "Keeps the streamlines that belong to the bundle you name, using its own\n"
              "length range and endpoints, and drops the ones a neighbouring bundle fits\n"
              "better. The originals are left alone; the result is listed as a new tract."),
              &dialog));

          GridLayout* grid = new GridLayout;
          layout->addLayout (grid);

          grid->addWidget (new QLabel (tr ("find")), 0, 0);
          QLineEdit* filter = new QLineEdit (&dialog);
          make_search_box (filter, tr ("Search every category - e.g. CST, _L"));
          filter->setClearButtonEnabled (true);
          grid->addWidget (filter, 0, 1);

          grid->addWidget (new QLabel (tr ("category")), 1, 0);
          QComboBox* category = new QComboBox (&dialog);
          grid->addWidget (category, 1, 1);
          grid->addWidget (new QLabel (tr ("bundle")), 2, 0);
          QComboBox* bundle = new QComboBox (&dialog);
          grid->addWidget (bundle, 2, 1);

          vector<std::string> categories;
          for (const auto& ref : atlas_catalogue) {
            const std::string group = ref.category.size() ? ref.category : std::string ("atlas");
            if (std::find (categories.begin(), categories.end(), group) == categories.end())
              categories.push_back (group);
          }
          for (const auto& group : categories)
            category->addItem (qstr (group));

          auto fill_bundles = [&] () {
            bundle->clear();
            const std::string group = category->currentText().toStdString();
            const std::string needle = filter->text().toLower().toStdString();
            for (const auto& ref : atlas_catalogue) {
              if ((ref.category.size() ? ref.category : std::string ("atlas")) != group)
                continue;
              if (needle.size()) {
                std::string lower = ref.name;
                std::transform (lower.begin(), lower.end(), lower.begin(),
                                [] (unsigned char c) { return std::tolower (c); });
                if (lower.find (needle) == std::string::npos)
                  continue;
              }
              bundle->addItem (qstr (ref.name));
            }
          };
          // A search that matches nothing in the shown category moves to the one that
          // has it, so a name is enough to find a bundle without knowing its group.
          auto follow_filter = [&] () {
            const std::string needle = filter->text().toLower().toStdString();
            if (needle.size()) {
              for (const auto& ref : atlas_catalogue) {
                std::string lower = ref.name;
                std::transform (lower.begin(), lower.end(), lower.begin(),
                                [] (unsigned char c) { return std::tolower (c); });
                if (lower.find (needle) == std::string::npos)
                  continue;
                const int idx = category->findText (qstr (ref.category.size() ? ref.category
                                                                              : std::string ("atlas")));
                if (idx >= 0 && idx != category->currentIndex()) {
                  category->blockSignals (true);
                  category->setCurrentIndex (idx);
                  category->blockSignals (false);
                }
                break;
              }
            }
            fill_bundles();
          };
          connect (category, &QComboBox::currentTextChanged, [&] (const QString&) { fill_bundles(); });
          connect (filter, &QLineEdit::textChanged, [&] (const QString&) { follow_filter(); });
          fill_bundles();

          QCheckBox* competitive = new QCheckBox (tr ("count a neighbouring bundle's better fit against a fibre"), &dialog);
          competitive->setChecked (true);
          competitive->setToolTip (tr ("For each streamline, ask which atlas bundle it is closest to rather than only\nhow close it is to this one. A neighbour that fits it better does not delete\nit: it adds to the streamline's score, in proportion to how much better, so\nstrictness drops the contested ones first.\n\nMeasured on the projection category at 15 mm, 400 genuine and 400 bent\nstreamlines refined together: at 50% strictness all 400 genuine survive and 1\nof the bent ones does. Against the whole category, no medial lemniscus passes\nas corticospinal tract and no corticospinal streamline bent through the\nthalamus survives, while the tract itself keeps 400 of 400 - three more than\nrejecting outright ever kept."));
          layout->addWidget (competitive);

          GridLayout* more = new GridLayout;
          layout->addLayout (more);
          more->addWidget (new QLabel (tr ("strictness")), 0, 0);
          QSpinBox* keep = new QSpinBox (&dialog);
          // The share to *drop*, so higher reads as stricter, as it does everywhere else.
          keep->setRange (0, 90);
          keep->setValue (0);
          keep->setSuffix (tr (" %"));
          keep->setToolTip (tr ("Drop this share of the streamlines, furthest from the bundle first.\n"
                                "0% applies the distance threshold below instead."));
          more->addWidget (keep, 0, 1);
          more->addWidget (new QLabel (tr ("distance (mm)")), 1, 0);
          AdjustButton* distance = new AdjustButton (&dialog, 0.5f);
          distance->setValue (10.0f);
          more->addWidget (distance, 1, 1);
          more->addWidget (new QLabel (tr ("prune outliers")), 2, 0);
          QComboBox* prune = new QComboBox (&dialog);
          prune->addItem (tr ("off"));
          prune->addItem (tr ("low"));
          prune->addItem (tr ("medium"));
          prune->addItem (tr ("high"));
          more->addWidget (prune, 2, 1);

          QDialogButtonBox* buttons = new QDialogButtonBox (
              QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
          connect (buttons, SIGNAL (accepted()), &dialog, SLOT (accept()));
          connect (buttons, SIGNAL (rejected()), &dialog, SLOT (reject()));
          layout->addWidget (buttons);
          if (dialog.exec() != QDialog::Accepted || !bundle->count())
            return;

          const std::string bundle_name = bundle->currentText().toStdString();

          BundleMatcher::Params match;
          match.metric = BundleMatcher::Metric::Hausdorff;
          match.max_mdf = distance->value();

          RefineOptions options;
          options.competitive = competitive->isChecked();
          options.length_window = true;
          options.endpoint_gate = true;
          if (keep->value() > 0)
            options.keep_fraction = 1.0f - float (keep->value()) / 100.0f;
          options.per_node_outliers = true;
          switch (prune->currentIndex()) {
            case 1:  options.outlier_k = 4.0f; break;
            case 2:  options.outlier_k = 3.0f; break;
            case 3:  options.outlier_k = 2.5f; break;
            default: break;
          }

          try {
            QApplication::setOverrideCursor (Qt::WaitCursor);
            const vector<MR::DWI::Tractography::Streamline<float>> reference =
                registration.bundle (bundle_name);
            if (reference.empty())
              throw Exception ("atlas bundle \"" + bundle_name + "\" could not be read");

            vector<Competitor> competitors;
            if (options.competitive) {
              // Whatever index is already in hand; building it here would block the
              // GUI for tens of seconds, and the Track generation panel builds it in
              // the background anyway.
              if (!AtlasTemplate::footprints_ready())
                AtlasTemplate::load_footprint_cache();
              for (const std::string& other : AtlasTemplate::competitors_for (bundle_name)) {
                try {
                  const auto& neighbour = registration.bundle (other);
                  if (neighbour.size())
                    competitors.push_back ({ other, neighbour });
                } catch (Exception&) { }
              }
              if (competitors.empty())
                options.competitive = false;
            }

            auto population = open_population_map (bundle_name, registration);

            for (Tractogram* t : targets) {
              Tractogram::FilteredTracks scratch;
              const auto& tracks = tract_streamlines (t, scratch);
              if (tracks.empty())
                continue;
              vector<MR::DWI::Tractography::Streamline<float>> kept, rejected;
              RefineReport report;
              MR::Timer clock;
              refine_bundle (reference, match, options, competitors, tracks, kept, rejected, report,
                             nullptr, population.get());
              QApplication::restoreOverrideCursor();

              if (kept.empty()) {
                QMessageBox::information (this, "Refine against an atlas bundle",
                    qstr ("Nothing in \"" + t->display_name() + "\" matched " + bundle_name
                          + ".\n\n" + report.summary()
                          + "\n\nRaise the distance, or lower \"keep the closest\"."));
                continue;
              }

              MR::DWI::Tractography::Properties props;
              props["refined_from"] = t->display_name();
              props["refine_reference"] = bundle_name;
              props["refine_distance"] = str (report.applied_distance);
              if (options.competitive) {
                std::string names;
                for (const auto& competitor : competitors)
                  names += (names.size() ? "," : "") + competitor.name;
                props["refine_competitors"] = names;
              }
              const std::string name = strip_known_suffix (Path::basename (t->display_name()))
                                     + "_" + bundle_name;
              if (Tractogram* added = add_tractogram_from_memory (kept, props, name, tracks.size())) {
                added->set_rejected_tracks (rejected);
                // So the new tract can be re-derived from the Refine group afterwards,
                // exactly like one that came out of a run.
                Tractogram::Refinement& provenance = added->refinement();
                provenance.bundle = bundle_name;
                provenance.match = match;
                provenance.options = options;
                provenance.original = options;
                provenance.kept = kept.size();
                provenance.candidates = tracks.size();
                provenance.valid = true;
              }
              QMessageBox::information (this, "Refine against an atlas bundle",
                  qstr (report.summary() + "\nlisted as \"" + name + "\", in "
                        + str (clock.elapsed(), 3) + " s"));
              QApplication::setOverrideCursor (Qt::WaitCursor);
            }
            QApplication::restoreOverrideCursor();
          } catch (Exception& e) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning (this, "Refine against an atlas bundle", qstr (e[0]));
          }
        }



        void Tractography::cluster_tracts_slot ()
        {
          using namespace MR::DWI::Tractography::Recognition;

          vector<Tractogram*> targets = selected_tractograms();
          if (targets.empty())
            return;

          // Number of clusters and which fit, in one dialog: the method changes what
          // the count means enough that asking for them separately would be awkward.
          QDialog dialog (this);
          dialog.setWindowTitle (tr ("Cluster tracts"));
          VBoxLayout* layout = new VBoxLayout (&dialog);
          QLabel* blurb = new QLabel (tr (
              "Streamlines are grouped by their endpoints, midpoint and length\n"
              "(the features DSI Studio clusters on), all in millimetres.\n\n"
              "Asking for more clusters than you expect bundles gives cleaner\n"
              "groups, which you can then combine by eye: on seven neighbouring\n"
              "atlas bundles, 7 clusters put 89% of streamlines with the right\n"
              "bundle and 14 clusters 96%."), &dialog);
          layout->addWidget (blurb);

          GridLayout* grid = new GridLayout;
          layout->addLayout (grid);
          grid->addWidget (new QLabel (tr ("clusters")), 0, 0);
          QSpinBox* count_box = new QSpinBox (&dialog);
          count_box->setRange (2, 200);
          count_box->setValue (8);
          count_box->setToolTip (tr ("How many groups to split into; fewer are made if the data cannot support that many"));
          grid->addWidget (count_box, 0, 1);
          grid->addWidget (new QLabel (tr ("method")), 1, 0);
          QComboBox* method_box = new QComboBox (&dialog);
          method_box->addItem (tr ("expectation-maximisation"));
          method_box->addItem (tr ("k-means"));
          method_box->addItem (tr ("hierarchical (every pair)"));
          method_box->setToolTip (tr ("EM fits each cluster its own full covariance, which suits the\n"
                                      "elongated, unequally sized clouds that bundles form, and measured\n"
                                      "best here: on seven adjacent atlas bundles at 7 clusters, EM 90%,\n"
                                      "k-means 85%, hierarchical 83%.\n\n"
                                      "k-means treats every cluster as equally spread, and is what seeds\n"
                                      "EM. Hierarchical compares every pair of streamlines over their\n"
                                      "whole length instead of comparing each to a cluster centre; it\n"
                                      "holds an N x N matrix, so it is capped at 12000 streamlines."));
          grid->addWidget (method_box, 1, 1);

          QDialogButtonBox* buttons = new QDialogButtonBox (
              QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
          connect (buttons, SIGNAL (accepted()), &dialog, SLOT (accept()));
          connect (buttons, SIGNAL (rejected()), &dialog, SLOT (reject()));
          layout->addWidget (buttons);
          if (dialog.exec() != QDialog::Accepted)
            return;

          const size_t num_clusters = size_t (count_box->value());
          const ClusterMethod method = method_box->currentIndex() == 2 ? ClusterMethod::Hierarchical
                                     : method_box->currentIndex() == 1 ? ClusterMethod::KMeans
                                     : ClusterMethod::EM;
          const std::string method_name = method == ClusterMethod::Hierarchical ? "hierarchical"
                                        : method == ClusterMethod::KMeans ? "k-means" : "EM";

          for (Tractogram* t : targets) {
            try {
              Tractogram::FilteredTracks scratch;
              const auto& tracks = tract_streamlines (t, scratch);
              if (tracks.size() < 2) {
                QMessageBox::information (this, "Cluster tracts", "Too few streamlines to cluster.");
                continue;
              }

              MR::Timer clock;
              QApplication::setOverrideCursor (Qt::WaitCursor);
              ClusterOptions options;
              options.method = method;
              const ClusterResult clustered = cluster_streamlines (tracks, num_clusters, options);
              vector<vector<MR::DWI::Tractography::Streamline<float>>> clusters (clustered.num_clusters);
              for (size_t i = 0; i != tracks.size(); ++i) {
                if (clustered.assignment[i] != ClusterResult::invalid)
                  clusters[clustered.assignment[i]].push_back (tracks[i]);
              }
              QApplication::restoreOverrideCursor();

              // Largest first, so cluster1 is the dominant group rather than
              // whichever streamline happened to come first in the file.
              vector<size_t> order (clusters.size());
              for (size_t i = 0; i != order.size(); ++i) order[i] = i;
              std::sort (order.begin(), order.end(), [&clusters] (size_t a, size_t b) {
                return clusters[a].size() > clusters[b].size();
              });

              const std::string name = strip_known_suffix (Path::basename (t->display_name()));
              size_t made = 0;
              for (size_t k : order) {
                if (clusters[k].empty())
                  continue;         // k-means can leave a cluster with nothing in it
                MR::DWI::Tractography::Properties props;
                props["split_from"] = t->display_name();
                props["cluster_method"] = method_name;
                props["cluster_count"] = str (num_clusters);
                add_tractogram_from_memory (clusters[k], props,
                    name + "_cluster" + str (++made), clusters[k].size());
              }

              std::string message = str (made) + " clusters from " + str (tracks.size())
                                  + " streamlines (" + method_name + ", "
                                  + str (clustered.iterations) + " iterations, "
                                  + str (clock.elapsed(), 3) + " s)\n"
                                  + "mean distance to cluster centre: "
                                  + str (clustered.mean_distance, 3) + " mm";
              if (made < num_clusters)
                message += "\nFewer than the " + str (num_clusters)
                         + " asked for: the features do not separate any further.";
              if (!clustered.converged)
                message += "\nStopped at the iteration limit rather than settling; "
                           "the grouping may shift if you run it again.";
              QMessageBox::information (this, "Cluster tracts", qstr (message));
            } catch (Exception& e) {
              QApplication::restoreOverrideCursor();
              QMessageBox::warning (this, "Cluster tracts", qstr (e[0]));
            }
          }
          update_edit_controls();
          window().updateGL();
        }



        void Tractography::statistics_slot ()
        {
          const MR::Header* grid = nullptr;
          MR::Header header;
          if (window().image()) {
            header = window().image()->header();
            header.ndim() = 3;
            grid = &header;
          }

          std::string text, csv = BundleStats::csv_header() + "\n";
          // Named after the tract when there is only one, as in every other save.
          const auto stats_selection = selected_tractograms();
          const std::string stats_filename = stats_selection.size() == 1
              ? strip_known_suffix (Path::basename (stats_selection[0]->display_name())) + "_stats.csv"
              : std::string ("bundle_stats.csv");
          MR::Timer clock;
          QApplication::setOverrideCursor (Qt::WaitCursor);
          for (Tractogram* t : selected_tractograms()) {
            const std::string name = Path::basename (t->display_name());
            Tractogram::FilteredTracks scratch;
            const BundleStats stats = compute_bundle_stats (tract_streamlines (t, scratch), grid);
            text += stats.as_text (name);
            csv += stats.as_csv_row (name) + "\n";
          }
          QApplication::restoreOverrideCursor();

          if (text.empty()) {
            QMessageBox::information (this, "Bundle statistics",
                "Select one or more tractograms in the list first.");
            return;
          }
          const double elapsed = clock.elapsed();

          // Its own dialog rather than a message box, in a fixed-pitch font: the
          // columns are the readable part and a proportional font throws them away.
          QDialog dialog (this);
          dialog.setWindowTitle (tr ("Bundle statistics"));
          VBoxLayout* layout = new VBoxLayout (&dialog);

          QString summary = tr ("%1 tract(s), computed in %2 s")
              .arg (selected_tractograms().size()).arg (elapsed, 0, 'f', 2);
          if (!grid)
            summary += tr ("   -   volume needs a main image loaded");
          layout->addWidget (new QLabel (summary, &dialog));

          QPlainTextEdit* view = new QPlainTextEdit (qstr (text), &dialog);
          view->setReadOnly (true);
          view->setLineWrapMode (QPlainTextEdit::NoWrap);
          view->setFont (QFontDatabase::systemFont (QFontDatabase::FixedFont));
          view->setMinimumSize (460, 320);
          layout->addWidget (view, 1);

          QDialogButtonBox* buttons = new QDialogButtonBox (QDialogButtonBox::Close, Qt::Horizontal, &dialog);
          QPushButton* copy = buttons->addButton (tr ("Copy"), QDialogButtonBox::ActionRole);
          QPushButton* save = buttons->addButton (tr ("Save CSV..."), QDialogButtonBox::ActionRole);
          connect (buttons, SIGNAL (rejected()), &dialog, SLOT (reject()));
          connect (copy, &QPushButton::clicked, this, [text] () {
            QApplication::clipboard()->setText (qstr (text));
          });
          connect (save, &QPushButton::clicked, this, [this, csv, stats_filename] () {
            const std::string path = Dialog::File::get_save_name (this, "Save statistics", stats_filename);
            if (path.empty())
              return;
            try {
              File::OFStream out (path);
              out << csv;
            } catch (Exception& e) { e.display(); }
          });
          layout->addWidget (buttons);
          dialog.exec();
        }



        void Tractography::profile_slot ()
        {
          auto selected = selected_tractograms();
          if (selected.empty()) {
            QMessageBox::information (this, "Along-tract profile",
                "Select one or more tractograms in the list first.");
            return;
          }

          const std::string profile_filename = selected.size() == 1
              ? strip_known_suffix (Path::basename (selected[0]->display_name())) + "_profile.csv"
              : std::string ("profile.csv");

          const std::string image_path = Dialog::File::get_image (this,
              "Select the image to sample along the bundle");
          if (image_path.empty())
            return;

          try {
            auto scalar = MR::Image<float>::open (image_path);
            const std::string scalar_name = Path::basename (image_path);
            std::string csv;
            QApplication::setOverrideCursor (Qt::WaitCursor);
            for (Tractogram* t : selected) {
              Tractogram::FilteredTracks scratch;
              const auto profile = compute_along_tract_profile (
                  tract_streamlines (t, scratch), scalar, 100, scalar_name);
              csv += "# " + Path::basename (t->display_name()) + " sampled from " + scalar_name + "\n";
              csv += profile.as_csv();
            }
            QApplication::restoreOverrideCursor();

            const std::string path = Dialog::File::get_save_name (this,
                "Save along-tract profile", profile_filename);
            if (path.size()) {
              File::OFStream out (path);
              out << csv;
              edit_status_label->setText (qstr ("profile written to " + Path::basename (path)));
            }
          } catch (Exception& e) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning (this, "Along-tract profile", qstr (e[0]));
          }
        }



        void Tractography::right_click_menu_slot (const QPoint& pos)
        {
          QModelIndex index = tractogram_list_view->indexAt (pos);
          if (index.isValid()) {
            QPoint globalPos = tractogram_list_view->mapToGlobal (pos);
            tractogram_list_view->selectionModel()->select (index, QItemSelectionModel::Select);
            // Only auto-tracked tracts have a rejected set to split off.
            size_t rejected = 0;
            for (Tractogram* t : selected_tractograms())
              rejected += t->rejected_tracks().size();
            rejected_action->setEnabled (rejected);
            // Combining one tract with itself is a no-op, so it needs two.
            combine_action->setEnabled (selected_tractograms().size() > 1);
            rejected_action->setText (rejected
                ? tr ("Split the %1 streamlines &rejected by the distance metric").arg (uint64_t (rejected))
                : tr ("Split streamlines &rejected by the distance metric"));
            track_option_menu->exec (globalPos);
          }
        }


        void Tractography::endpoints_overlay_slot ()
        {
          // The map needs a grid, and the image in the view pane is the one the
          // overlay will be drawn against.
          if (!window().image()) {
            QMessageBox::information (this, "Track endpoints",
                "Load an image first: the endpoint map is built on its voxel grid.");
            return;
          }

          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          if (indices.empty())
            return;

          if (!endpoint_dir) {
            endpoint_dir.reset (new QTemporaryDir);
            if (!endpoint_dir->isValid()) {
              endpoint_dir.reset();
              QMessageBox::warning (this, "Track endpoints",
                  "Could not create a temporary directory for the endpoint map.");
              return;
            }
          }

          MR::Header H (window().image()->header());
          H.ndim() = 3;
          // Counts, not a mask: where a bundle terminates is a distribution, and a
          // mask would flatten a dense cortical projection into the same value as a
          // single stray streamline.
          H.datatype() = MR::DataType::UInt16LE;
          H.reset_intensity_scaling();
          H.keyval().clear();

          MR::Timer clock;
          vector<std::string> written;
          std::string report;
          for (int i = 0; i != indices.size(); ++i) {
            Tractogram* tractogram = tractogram_list_model->get_tractogram (indices[i]);
            if (!tractogram)
              continue;
            const std::string name = Path::basename (tractogram->display_name());
            try {
              Tractogram::FilteredTracks filtered;
              tractogram->get_filtered_streamlines (filtered);
              if (filtered.tracks.empty()) {
                QMessageBox::information (this, "Track endpoints",
                    qstr ("\"" + name + "\" has no visible streamlines to map."));
                continue;
              }

              // A tract's name is free-form, and MRtrix reads "[...]" in a filename
              // as a number-sequence specifier ("[atlas] OR_L" failed with "can't
              // parse integer sequence specifier"). Keep only characters that mean
              // nothing to the image-name parser or the shell.
              std::string safe = name;
              for (char& c : safe) {
                if (!(std::isalnum (static_cast<unsigned char> (c)) || c == '-' || c == '_' || c == '.'))
                  c = '_';
              }
              // Two runs of the same bundle carry the same tract name, so the path
              // has to be made unique - otherwise the second map would be written
              // over the file the first overlay is still reading.
              std::string path = endpoint_dir->filePath (
                  qstr (safe + "_endpoints.mif")).toStdString();
              for (size_t n = 2; Path::exists (path); ++n)
                path = endpoint_dir->filePath (
                    qstr (safe + "_endpoints_" + str(n) + ".mif")).toStdString();
              auto out = MR::Image<uint16_t>::create (path, H);
              const MR::Transform T (H);

              // Each endpoint is spread over a small ball rather than a single voxel.
              // A bare one-voxel-per-endpoint map is technically correct and
              // practically invisible: a few hundred streamlines put at most a few
              // hundred lit voxels in the whole volume, and a 2D slice through it
              // shows a handful of isolated dots or, more often, nothing at all.
              const float radius_mm = 2.0f;
              const int span[3] = {
                int (std::ceil (radius_mm / H.spacing(0))),
                int (std::ceil (radius_mm / H.spacing(1))),
                int (std::ceil (radius_mm / H.spacing(2)))
              };

              size_t outside = 0;
              auto mark = [&] (const Eigen::Vector3f& p) {
                const Eigen::Vector3d v = T.scanner2voxel * p.cast<double>();
                const ssize_t cx = std::lround (v[0]), cy = std::lround (v[1]), cz = std::lround (v[2]);
                if (cx < 0 || cy < 0 || cz < 0 ||
                    cx >= out.size(0) || cy >= out.size(1) || cz >= out.size(2)) {
                  ++outside;
                  return;
                }
                for (ssize_t z = cz - span[2]; z <= cz + span[2]; ++z) {
                  if (z < 0 || z >= out.size(2)) continue;
                  for (ssize_t y = cy - span[1]; y <= cy + span[1]; ++y) {
                    if (y < 0 || y >= out.size(1)) continue;
                    for (ssize_t x = cx - span[0]; x <= cx + span[0]; ++x) {
                      if (x < 0 || x >= out.size(0)) continue;
                      // Test in millimetres, so the ball stays a ball on an
                      // anisotropic grid.
                      const Eigen::Vector3d centre = T.voxel2scanner * Eigen::Vector3d (x, y, z);
                      if ((centre - p.cast<double>()).norm() > radius_mm)
                        continue;
                      out.index(0) = x; out.index(1) = y; out.index(2) = z;
                      if (out.value() < std::numeric_limits<uint16_t>::max())
                        out.value() = out.value() + 1;
                    }
                  }
                }
              };
              for (const auto& tck : filtered.tracks) {
                if (tck.empty())
                  continue;
                mark (tck.front());
                mark (tck.back());
              }
              if (outside == 2 * filtered.tracks.size()) {
                QMessageBox::warning (this, "Track endpoints",
                    qstr ("\"" + name + "\" lies entirely outside the current image; "
                          "are they in the same space?"));
                continue;
              }
              if (outside)
                WARN (str(outside) + " endpoints of \"" + name + "\" fall outside the image");

              // Say what actually landed in the map, so an empty or misplaced one is
              // obvious rather than looking like a display problem.
              size_t lit = 0, peak = 0;
              for (auto l = MR::Loop (0, 3) (out); l; ++l) {
                const uint16_t v = out.value();
                if (v) { ++lit; peak = std::max<size_t> (peak, v); }
              }
              report += (report.size() ? "\n" : "") + name + ": "
                      + str (2 * filtered.tracks.size() - outside) + " endpoints over "
                      + str (lit) + " voxels, peak " + str (peak);
              if (!lit) {
                QMessageBox::warning (this, "Track endpoints",
                    qstr ("\"" + name + "\" produced an empty map."));
                continue;
              }
              written.push_back (path);
            } catch (Exception& e) {
              e.display();
            }
          }

          if (written.empty())
            return;
          if (Overlay* overlay = get_tool<Overlay>()) {
            overlay->add_overlays (written);
            QMessageBox::information (this, "Track endpoints",
                qstr ("Listed in the Overlay tool, as a count of streamline ends per "
                      "voxel within 2 mm (" + str (clock.elapsed(), 2) + " s):\n\n" + report));
          } else {
            QMessageBox::warning (this, "Track endpoints", "Could not open the Overlay tool.");
          }
        }



        void Tractography::colour_track_by_direction_slot()
        {
          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i)  {
            Tractogram* tractogram = tractogram_list_model->get_tractogram (indices[i]);
            tractogram->set_color_type (TrackColourType::Direction);
            if (tractogram->get_threshold_type() == TrackThresholdType::UseColourFile)
              tractogram->set_threshold_type (TrackThresholdType::None);
          }
          colour_combobox->blockSignals (true);
          colour_combobox->setCurrentIndex (0);
          colour_combobox->clearError();
          colour_combobox->blockSignals (false);
          colour_button->setEnabled (false);
          update_scalar_options();
          window().updateGL();
        }


        void Tractography::colour_track_by_ends_slot()
        {
          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i) {
            Tractogram* tractogram = tractogram_list_model->get_tractogram (indices[i]);
            tractogram->set_color_type (TrackColourType::Ends);
            tractogram->load_end_colours();
            if (tractogram->get_threshold_type() == TrackThresholdType::UseColourFile)
              tractogram->set_threshold_type (TrackThresholdType::None);
          }
          colour_combobox->blockSignals (true);
          colour_combobox->setCurrentIndex (1);
          colour_combobox->clearError();
          colour_combobox->blockSignals (false);
          colour_button->setEnabled (false);
          update_scalar_options();
          window().updateGL();
        }


        void Tractography::randomise_track_colour_slot()
        {
          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          vector<Tractogram*> chosen;
          for (int i = 0; i < indices.size(); ++i) {
            if (Tractogram* t = tractogram_list_model->get_tractogram (indices[i]))
              chosen.push_back (t);
          }
          if (chosen.empty())
            return;

          // Colours already in use, so a "random" colour cannot land on one of them -
          // which is the whole point of randomising when several solid-coloured tracts
          // are shown together. Atlas bundles count: they are drawn in the same view
          // even though they are not listed here.
          vector<std::array<float,3>> in_use;
          auto add_in_use = [&] (const Tractogram* t) {
            if (!t || t->get_color_type() != TrackColourType::Manual)
              return;
            if (std::find (chosen.begin(), chosen.end(), t) != chosen.end())
              return;
            in_use.push_back ({ { t->colour[0] / 255.0f, t->colour[1] / 255.0f, t->colour[2] / 255.0f } });
          };
          for (int i = 0; i < tractogram_list_model->rowCount(); ++i)
            add_in_use (dynamic_cast<Tractogram*> (tractogram_list_model->items[i].get()));
          for (const auto& bundle : atlas_bundles)
            add_in_use (bundle.get());

          // Weighted by luma, because equal RGB steps are not equally visible: two
          // blues differing by 0.2 look far closer than two greens do.
          auto separation = [] (const std::array<float,3>& a, const std::array<float,3>& b) {
            const float dr = a[0]-b[0], dg = a[1]-b[1], db = a[2]-b[2];
            return std::sqrt (0.3f*dr*dr + 0.59f*dg*dg + 0.11f*db*db);
          };

          Math::RNG::Uniform<float> rng;
          for (Tractogram* tractogram : chosen) {
            std::array<float,3> best { { 0.0f, 0.0f, 0.0f } };
            float best_separation = -1.0f;
            // Candidates from the same golden-angle palette that names apart the
            // auto-generated tracts, plus random ones so repeated presses still move.
            for (size_t attempt = 0; attempt != 64; ++attempt) {
              std::array<float,3> candidate;
              if (attempt < 32) {
                const auto c = distinct_colour (size_t (rng() * 64.0f) + attempt);
                candidate = { { c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f } };
              } else {
                candidate = { { rng(), rng(), rng() } };
              }
              // Keep the old rule: nothing so dark it reads as black on black.
              if (candidate[0] < 0.5f && candidate[1] < 0.5f && candidate[2] < 0.5f)
                continue;
              float nearest = std::numeric_limits<float>::max();
              for (const auto& used : in_use)
                nearest = std::min (nearest, separation (candidate, used));
              if (in_use.empty())
                nearest = 1.0f;         // nothing to avoid; the first pick will do
              if (nearest > best_separation) {
                best_separation = nearest;
                best = candidate;
              }
              if (in_use.empty())
                break;
            }

            tractogram->set_color_type (TrackColourType::Manual);
            const QColor c (best[0]*255.0f, best[1]*255.0f, best[2]*255.0f);
            tractogram->set_colour (c);
            if (tractogram->get_threshold_type() == TrackThresholdType::UseColourFile)
              tractogram->set_threshold_type (TrackThresholdType::None);
            // Each newly chosen colour is itself something the next one must avoid.
            in_use.push_back (best);
            if (tractogram == chosen.front())
              colour_button->setColor (c);
          }

          colour_combobox->blockSignals (true);
          colour_combobox->setCurrentIndex (2);
          colour_combobox->clearError();
          colour_combobox->blockSignals (false);
          colour_button->setEnabled (true);
          update_scalar_options();
          window().updateGL();
        }


        void Tractography::set_track_colour_slot()
        {
          QColor color;
          color = QColorDialog::getColor(Qt::red, this, "Select Color", QColorDialog::DontUseNativeDialog);
          if (color.isValid()) {
            QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
            for (int i = 0; i < indices.size(); ++i) {
              Tractogram* tractogram = tractogram_list_model->get_tractogram (indices[i]);
              tractogram->set_color_type (TrackColourType::Manual);
              tractogram->set_colour (color);
              if (tractogram->get_threshold_type() == TrackThresholdType::UseColourFile)
                tractogram->set_threshold_type (TrackThresholdType::None);
            }
            colour_combobox->blockSignals (true);
            colour_combobox->setCurrentIndex (3);
            colour_combobox->clearError();
            colour_combobox->blockSignals (false);
            colour_button->setEnabled (true);
            colour_button->setColor (color);
            update_scalar_options();
          }
          window().updateGL();
        }


        void Tractography::colour_by_scalar_file_slot()
        {
          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          if (indices.size() != 1) {
            // User may have accessed this from the context menu
            QMessageBox::warning (QApplication::activeWindow(),
                                  tr ("Tractogram colour error"),
                                  tr ("Cannot set multiple tractograms to use the same file for streamline colouring"),
                                  QMessageBox::Ok,
                                  QMessageBox::Ok);
            return;
          }

          Tractogram* tractogram = tractogram_list_model->get_tractogram (indices[0]);
          scalar_file_options->set_tractogram (tractogram);
          if (tractogram->intensity_scalar_filename.empty()) {
            if (!scalar_file_options->open_intensity_track_scalar_file_slot()) {
              colour_combobox->blockSignals (true);
              switch (tractogram->get_color_type()) {
                case TrackColourType::Direction:  colour_combobox->setCurrentIndex (0); break;
                case TrackColourType::Ends:       colour_combobox->setCurrentIndex (1); break;
                case TrackColourType::Manual:     colour_combobox->setCurrentIndex (3); break;
                case TrackColourType::ScalarFile: colour_combobox->setCurrentIndex (4); break;
              }
              colour_combobox->clearError();
              colour_combobox->blockSignals (false);
              return;
            }
          }
          tractogram->set_color_type (TrackColourType::ScalarFile);
          colour_combobox->blockSignals (true);
          colour_combobox->setCurrentIndex (4);
          colour_combobox->clearError();
          colour_combobox->blockSignals (false);
          colour_button->setEnabled (false);
          update_scalar_options();
          window().updateGL();
        }


        void Tractography::colour_mode_selection_slot (int)
        {
          switch (colour_combobox->currentIndex()) {
            case 0: colour_track_by_direction_slot(); break;
            case 1: colour_track_by_ends_slot(); break;
            case 2: randomise_track_colour_slot(); break;
            case 3: set_track_colour_slot(); break;
            case 4: colour_by_scalar_file_slot(); break;
            case 5: break;
            default: assert (0);
          }
        }


        void Tractography::colour_button_slot()
        {
          // Button brings up its own colour prompt; if set_track_colour_slot()
          //   were to be called, this would present its own selection prompt
          // Need to instead set the colours here explicitly
          const QColor color = colour_button->color();
          if (color.isValid()) {
            QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
            for (int i = 0; i < indices.size(); ++i)
              tractogram_list_model->get_tractogram (indices[i])->set_colour (color);
            colour_combobox->blockSignals (true);
            colour_combobox->setCurrentIndex (3); // In case it was on random
            colour_combobox->clearError();
            colour_combobox->blockSignals (false);
            window().updateGL();
          }
        }



        void Tractography::geom_type_selection_slot (int selected_index)
        {
          // Combo box shows the "(variable)" message, and the user has
          //   re-clicked on it -> nothing to do
          if (selected_index == 3)
            return;

          TrackGeometryType geom_type = geometry_index2type (selected_index);

          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          for (int i = 0; i < indices.size(); ++i)
            tractogram_list_model->get_tractogram (indices[i])->set_geometry_type (geom_type);

          update_geometry_type_gui();

          window().updateGL();
        }



        void Tractography::selection_changed_slot (const QItemSelection &, const QItemSelection &)
        {
          update_scalar_options();
          update_geometry_type_gui();

          // Keep the editing controls in step with whichever tractograms are now
          // selected; the checkbox reflects them rather than driving them.
          {
            auto chosen = selected_tractograms();
            bool all_editing = !chosen.empty();
            for (Tractogram* t : chosen)
              if (!t->editing_enabled()) all_editing = false;
            edit_enable_box->blockSignals (true);
            edit_enable_box->setChecked (all_editing);
            edit_enable_box->blockSignals (false);
            update_edit_controls();
            refresh_rule_list();
          }

          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          if (!indices.size()) {
            colour_combobox->setEnabled (false);
            colour_button->setEnabled (false);
            return;
          }
          colour_combobox->setEnabled (true);

          const Tractogram* first_tractogram = tractogram_list_model->get_tractogram (indices[0]);

          TrackColourType color_type = first_tractogram->get_color_type();
          QColor color (first_tractogram->colour[0], first_tractogram->colour[1], first_tractogram->colour[2]);
          TrackGeometryType geom_type = first_tractogram->get_geometry_type();
          bool color_type_consistent = true, geometry_type_consistent = true;
          float mean_thickness = first_tractogram->line_thickness;
          for (int i = 1; i != indices.size(); ++i) {
            const Tractogram* tractogram = tractogram_list_model->get_tractogram (indices[i]);
            if (tractogram->get_color_type() != color_type)
              color_type_consistent = false;
            if (tractogram->get_geometry_type() != geom_type)
              geometry_type_consistent = false;
            mean_thickness += tractogram->line_thickness;
          }
          if (color_type_consistent) {
            colour_combobox->blockSignals (true);
            switch (color_type) {
              case TrackColourType::Direction:
                colour_combobox->setCurrentIndex (0);
                colour_button->setEnabled (false);
                break;
              case TrackColourType::Ends:
                colour_combobox->setCurrentIndex (1);
                colour_button->setEnabled (false);
                break;
              case TrackColourType::Manual:
                colour_combobox->setCurrentIndex (3);
                colour_button->setEnabled (true);
                colour_button->setColor (color);
                break;
              case TrackColourType::ScalarFile:
                colour_combobox->setCurrentIndex (4);
                colour_button->setEnabled (false);
                break;
            }
            colour_combobox->clearError();
            colour_combobox->blockSignals (false);
          } else {
            colour_combobox->setError();
          }

          if (geometry_type_consistent) {
            geom_type_combobox->blockSignals (true);
            switch (geom_type) {
              case TrackGeometryType::Pseudotubes:
                geom_type_combobox->setCurrentIndex (0);
                break;
              case TrackGeometryType::Lines:
                geom_type_combobox->setCurrentIndex (1);
                break;
              case TrackGeometryType::Points:
                geom_type_combobox->setCurrentIndex (2);
                break;
            }
            geom_type_combobox->clearError();
            geom_type_combobox->blockSignals (false);
          } else {
            geom_type_combobox->setError();
          }

          thickness_slider->blockSignals (true);
          thickness_slider->setSliderPosition (mean_thickness / float(indices.size()));
          thickness_slider->blockSignals (false);
        }



        void Tractography::update_scalar_options()
        {
          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          if (indices.size() == 1)
            scalar_file_options->set_tractogram (tractogram_list_model->get_tractogram (indices[0]));
          else
            scalar_file_options->set_tractogram (nullptr);
          scalar_file_options->update_UI();
        }



        void Tractography::update_geometry_type_gui()
        {
          thickness_slider->setHidden (true);
          thickness_label->setHidden (true);
          lighting_button->setEnabled (false);
          lighting_group_box->setEnabled (false);
          geom_type_combobox->setEnabled (false);

          QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
          if (!indices.size())
            return;

          geom_type_combobox->setEnabled (true);

          const Tractogram* first_tractogram = tractogram_list_model->get_tractogram (indices[0]);
          const TrackGeometryType geom_type = first_tractogram->get_geometry_type();

          if (geom_type == TrackGeometryType::Pseudotubes || geom_type == TrackGeometryType::Points) {
            thickness_slider->setHidden (false);
            thickness_label->setHidden (false);
            lighting_button->setEnabled (true);
            lighting_group_box->setEnabled (true);
          }

        }







        void Tractography::add_commandline_options (MR::App::OptionList& options)
        {
          using namespace MR::App;
          options
            + OptionGroup ("Tractography tool options")

            + Option ("tractography.load", "Load the specified tracks file into the tractography tool.").allow_multiple()
            +   Argument ("tracks").type_file_in()

            + Option ("tractography.thickness", "Line thickness of tractography display, [-1.0, 1.0], default is 0.0.").allow_multiple()
            +   Argument ("value").type_float ( -1.0, 1.0 )

            + Option ("tractography.geometry", "The geometry type to use when rendering tractograms (options are: " + join(tractogram_geometry_types, ", ") + ")").allow_multiple()
            +   Argument ("value").type_choice (tractogram_geometry_types)

            + Option ("tractography.opacity", "Opacity of tractography display, [0.0, 1.0], default is 1.0.").allow_multiple()
            +   Argument ("value").type_float ( 0.0, 1.0 )

            + Option ("tractography.slab", "Slab thickness of tractography display, in mm. -1 to turn off crop to slab.").allow_multiple()
            +   Argument ("value").type_float(-1, 1e6)

            + Option ("tractography.lighting", "Toggle the use of lighting of tractogram geometry").allow_multiple()
            +  Argument ("value").type_bool()

            + Option ("tractography.colour", "Specify a manual colour for the tractogram, as three comma-separated values").allow_multiple()
            +   Argument ("R,G,B").type_sequence_float()

            + Option ("tractography.tsf_load", "Load the specified tractography scalar file.").allow_multiple()
            +  Argument ("tsf").type_file_in()

            + Option ("tractography.tsf_range", "Set range for the tractography scalar file. Requires -tractography.tsf_load already provided.").allow_multiple()
            +  Argument ("RangeMin,RangeMax").type_sequence_float()

            + Option ("tractography.tsf_thresh", "Set thresholds for the tractography scalar file. Requires -tractography.tsf_load already provided.").allow_multiple()
            +  Argument ("ThresholdMin,ThresholdMax").type_sequence_float()

            + Option ("tractography.tsf_colourmap", "Sets the colourmap of the .tsf file as indexed in the tsf colourmap dropdown menu. Requires -tractography.tsf_load already.").allow_multiple()
            +   Argument ("index").type_integer();

        }

        /*
          Selects the last tractogram in the tractogram_list_view and updates the window. If no tractograms are in the list view, no action is taken.
        */
        void Tractography::select_last_added_tractogram()
        {
          int count = tractogram_list_model->rowCount();
          if(count != 0){
            QModelIndex index = tractogram_list_view->model()->index(count-1, 0);
            tractogram_list_view->setCurrentIndex(index);
            window().updateGL();
          }
        }


        bool Tractography::process_commandline_option (const MR::App::ParsedOption& opt)
        {

          if (opt.opt->is ("tractography.load"))
          {
            vector<std::string> list (1, std::string(opt[0]));
            add_tractogram(list);
            return true;
          }


          if (opt.opt->is ("tractography.tsf_load"))
          {
            try {

              if (process_commandline_option_tsf_check_tracto_loaded()) {
                QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();

                if (indices.size() == 1) {//just in case future edits break this assumption
                  Tractogram* tractogram = tractogram_list_model->get_tractogram (indices[0]);

                  //set its tsf filename and load the tsf file
                  scalar_file_options->set_tractogram (tractogram);
                  scalar_file_options->open_intensity_track_scalar_file_slot (std::string(opt[0]));

                  //Set the GUI to use the file for visualisation
                  colour_combobox->setCurrentIndex(4); // Set combobox to "File"
                }
              }
            }
            catch (Exception& E) { E.display(); }

            return true;
          }

          if (opt.opt->is ("tractography.tsf_range"))
          {
            try {
              //Set the tsf visualisation range
              vector<default_type> range;
              if (process_commandline_option_tsf_option(opt,2, range))
                scalar_file_options->set_scaling (range[0], range[1]);
            }
            catch (Exception& E) { E.display(); }
            return true;
          }


          if (opt.opt->is ("tractography.tsf_thresh"))
          {
            try {
              //Set the tsf visualisation threshold
              vector<default_type> range;
              if (process_commandline_option_tsf_option(opt,2, range))
                scalar_file_options->set_threshold (TrackThresholdType::UseColourFile,range[0], range[1]);
            }
            catch(Exception& E) { E.display(); }
            return true;
          }



          if (opt.opt->is ("tractography.thickness")) {
            // Thickness runs from -1000 to 1000,
            float thickness = float(opt[0]) * 1000.0f;
            try {
              thickness_slider->setValue(thickness);
            }
            catch (Exception& E) { E.display(); }
            return true;
          }


          if (opt.opt->is ("tractography.tsf_colourmap")) {
            try {
              int n = opt[0];
              if (n < 0 || !ColourMap::maps[n].name)
                throw Exception ("invalid tsf colourmap index \"" + std::string (opt[0]) + "\" for -tractography.tsf_colourmap option");
              if (process_commandline_option_tsf_check_tracto_loaded()) {
                // get list of selected tractograms:
                QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
                if (indices.size() != 1)
                  throw Exception ("-tractography.tsf_colourmap option requires one tractogram to be selected");

                // get pointer to tractogram:
                Tractogram* tractogram = tractogram_list_model->get_tractogram (indices[0]);

                // check tractogram has a scalar file attached and prepare the scalar_file_options object:
                if (tractogram->get_color_type() == TrackColourType::ScalarFile) {
                  scalar_file_options->set_tractogram (tractogram);
                  scalar_file_options->set_colourmap (opt[0]);
                }
              }
            } catch (Exception& e) { e.display(); }
            return true;
          }


           if (opt.opt->is ("tractography.colour")) {
            try {

              auto values = parse_floats (opt[0]);
              if (values.size() != 3)
                throw Exception ("must provide exactly three comma-separated values to the -tractography.colour option");
              const float max_value = std::max ({ values[0], values[1], values[2] });
              if (std::min ({ values[0], values[1], values[2] }) < 0.0 || max_value > 255)
                throw Exception ("values provided to -tractogram.colour must be either between 0.0 and 1.0, or between 0 and 255");
              const float multiplier = max_value <= 1.0 ? 255.0 : 1.0;

              //input need to be a float *
              QColor colour (multiplier*values[0], multiplier*values[1], multiplier*values[2]);

              QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();

              if (indices.size() != 1)
                  throw Exception ("-tractography.colour option requires one tractogram to be selected");
              // get pointer to tractogram:
              Tractogram* tractogram = tractogram_list_model->get_tractogram (indices[0]);

              // set the color
              tractogram->set_color_type (TrackColourType::Manual);
              tractogram->set_colour (colour);

              // update_color_type_gui
              colour_combobox->setCurrentIndex (3);
              colour_button->setEnabled (true);
              colour_button->setColor (colour);


            }
            catch (Exception& e) { e.display(); }
            return true;
           }


          if (opt.opt-> is ("tractography.geometry")) {
            try {             const TrackGeometryType geom_type = geometry_index2type (geometry_string2index (opt[0]));
              QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
              if (indices.size()) {
                for (int i = 0; i < indices.size(); ++i)
                  tractogram_list_model->get_tractogram (indices[i])->set_geometry_type (geom_type);
              } else {
                Tractogram::default_tract_geom = geom_type;
              }
              update_geometry_type_gui();
            }
            catch (Exception& E) { E.display(); }
            return true;
          }


          if (opt.opt->is ("tractography.opacity")) {
            // Opacity runs from 0 to 1000, so multiply by 1000
            float opacity = float(opt[0]) * 1000.0f;
            try {
              opacity_slider->setValue(opacity);
            }
            catch (Exception& E) { E.display(); }
            return true;
          }

          if (opt.opt->is ("tractography.slab")) {
            float thickness = opt[0];
            try {
              bool crop = thickness > 0;
              slab_group_box->setChecked(crop);
              on_crop_to_slab_slot(crop);//Needs to be manually bumped
              if(crop)
              {
                slab_entry->setValue(thickness);
                on_slab_thickness_slot();//Needs to be manually bumped
              }
            }
            catch (Exception& E) { E.display(); }
            return true;
          }

          if (opt.opt->is ("tractography.lighting")) {
            const bool value = bool(opt[0]);
            lighting_group_box->setChecked (value);
            use_lighting = bool(value);
            return true;
          }

          return false;
        }






      /*Checks whether any tractography has been loaded and warns the user if it has not*/
        bool Tractography::process_commandline_option_tsf_check_tracto_loaded()
        {
          int count = tractogram_list_model->rowCount();
          if (count == 0){
            //Error to std error to save many dialogs appearing for a single missed argument
            std::cerr << "TSF argument specified but no tractography loaded. Ensure TSF arguments follow the tractography.load argument.\n";
          }
          return count != 0;
        }






      /*Checks whether legal to apply tsf options and prepares the scalar_file_options to do so. Returns the vector of floats parsed from the options, or null on fail*/
        bool Tractography::process_commandline_option_tsf_option(const MR::App::ParsedOption& opt, uint reqArgSize, vector<default_type>& range)
        {
          if(process_commandline_option_tsf_check_tracto_loaded()){
            QModelIndexList indices = tractogram_list_view->selectionModel()->selectedIndexes();
            range = opt[0].as_sequence_float();
            if(indices.size() == 1 && range.size() == reqArgSize){
              //values supplied
              Tractogram* tractogram = tractogram_list_model->get_tractogram (indices[0]);
              if(tractogram->get_color_type() == TrackColourType::ScalarFile){
                //prereq options supplied/executed
                scalar_file_options->set_tractogram(tractogram);
                return true;
              }
              else
              {
                std::cerr << "Could not apply TSF argument - tractography.load_tsf not supplied.\n";
              }
            }
            else
            {
              std::cerr << "Could not apply TSF argument - insufficient number of arguments provided.\n";
            }
          }
        return false;
        }
      }
    }
  }
}





