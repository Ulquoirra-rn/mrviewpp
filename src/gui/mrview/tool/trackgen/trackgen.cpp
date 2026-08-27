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

#include "gui/mrview/tool/trackgen/trackgen.h"

#include <algorithm>
#include <mutex>

#include <QApplication>

#include "gui/mrview/atlas_registration.h"
#include "gui/mrview/tool/tractography/tractogram.h"
#include "dwi/tractography/file.h"
#include "dwi/tractography/recognition/bundle_matcher.h"

#include <QFileDialog>
#include <cmath>
#include <cstring>

#include <QCompleter>
#include <QStringListModel>
#include <QDockWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QTimer>

#include "app.h"
#include "progressbar.h"
#include "file/config.h"
#include "file/path.h"

#include "dwi/tractography/file.h"
#include "dwi/tractography/roi.h"
#include "dwi/tractography/seeding/basic.h"
#include "dwi/tractography/seeding/list.h"

#include "gui/dialog/file.h"
#include "gui/mrview/tool/odf/odf.h"
#include "gui/mrview/tool/roi_editor/roi.h"
#include "gui/mrview/tool/tractography/tractography.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        namespace
        {
          // Filled by the worker thread through the error/warning hooks it installs
          // while running, and drained on the GUI thread. File-scope because those
          // hooks are plain function pointers with no user-data argument.
          std::mutex worker_message_mutex;
          vector<std::string> worker_messages;
        }


        using namespace MR::DWI::Tractography;

        namespace
        {
          // Order matters: it is stored in the session file.
          const char* const role_names[] = { "seed", "include", "ordered include", "exclude", "mask" };
          constexpr int num_roles = 5;

          enum Role { SEED = 0, INCLUDE = 1, ORDERED_INCLUDE = 2, EXCLUDE = 3, MASK = 4 };

          //! Name a manual tracking run: tract_<n>, plus whatever was set explicitly.
          /*! Only parameters the user actually typed appear (blank fields are left
           *  out of `scalars` so the algorithm's own default applies), so two runs
           *  that differ only in cutoff are immediately distinguishable. The target
           *  streamline count is deliberately omitted - it is always set, so it
           *  would appear on every name without telling you anything. */
          std::string name_for_run (size_t iteration, const std::map<std::string, std::string>& scalars)
          {
            std::string name = "tract_" + str (iteration);
            // Fixed order, so the same settings always give the same name.
            static const std::pair<const char*, const char*> labelled[] = {
              { "threshold", "cutoff" }, { "max_angle", "angle" }, { "step_size", "step" },
              { "min_dist", "minlen" },  { "max_dist", "maxlen" }
            };
            for (const auto& entry : labelled) {
              const auto it = scalars.find (entry.first);
              if (it != scalars.end())
                name += std::string ("_") + entry.second + it->second;
            }
            if (scalars.count ("rk4"))                 name += "_rk4";
            if (scalars.count ("unidirectional"))      name += "_unidir";
            if (scalars.count ("stop_on_all_include")) name += "_stop";
            return name;
          }


          // Everything the worker needs, captured by value on the GUI thread.
          // Properties itself is built inside the worker, because SharedBase
          // holds a reference to it for the lifetime of the run.
          struct Request { NOMEMALIGN
            std::string source_path;
            int algorithm = 2;
            size_t nthreads = 0;
            size_t seeds_per_voxel = 0;
            std::map<std::string, std::string> scalars;   // Properties key -> value
            vector<std::pair<int, std::pair<std::string, MR::Image<bool>>>> regions;  // role, (name, mask)
          };
        }



        TrackGen::TrackGen (Dock* parent) :
            Base (parent),
            poll_timer (nullptr)
        {
          VBoxLayout* main_box = new VBoxLayout (this);

          // --- what to show ---
          // Two audiences, one panel. Guided shows the six controls a clinical run
          // needs; Expert shows all of them, unchanged from before this existed.
          // Nothing is duplicated: Guided's presets write into the same widgets
          // Expert exposes, so a run has one set of settings either way.
          {
            HBoxLayout* mode_layout = new HBoxLayout;
            mode_layout->addWidget (new QLabel (tr ("Mode")));
            mode_combo = new QComboBox (this);
            mode_combo->addItem (tr ("Guided"));
            mode_combo->addItem (tr ("Expert"));
            mode_combo->setToolTip (
                tr ("Guided shows only what a bundle reconstruction needs: the image, the\n"
                    "bundles to reconstruct, and how much time to spend.\n\n"
                    "Expert adds the tracking algorithm, every tckgen parameter, the shape\n"
                    "matching settings, and manual ROI tracking."));
            mode_layout->addWidget (mode_combo, 1);
            main_box->addLayout (mode_layout);
          }

          // --- source + algorithm ---
          QGroupBox* source_box = new QGroupBox (tr ("Source"));
          main_box->addWidget (source_box);
          GridLayout* source_layout = new GridLayout;
          source_box->setLayout (source_layout);

          source_layout->addWidget (new QLabel (tr ("FOD image")), 0, 0);
          source_combo = new QComboBox (this);
          source_combo->setToolTip (tr ("An SH image loaded in the ODF display tool"));
          source_layout->addWidget (source_combo, 0, 1);
          QPushButton* refresh = new QPushButton (tr ("Find"), this);
          refresh->setToolTip (tr ("Load an FOD image, whether or not one is already open "
                                   "(re-scans the ODF display tool first)"));
          connect (refresh, SIGNAL (clicked()), this, SLOT (find_sources_slot()));
          source_layout->addWidget (refresh, 0, 2);

          QLabel* algorithm_label = new QLabel (tr ("algorithm"));
          source_layout->addWidget (algorithm_label, 1, 0);
          expert_only.push_back (algorithm_label);
          algorithm_combo = new QComboBox (this);
          expert_only.push_back (algorithm_combo);
          // Only algorithms that can track from an FOD are offered: the source is
          // an SH image from the ODF tool, and the rest (FACT, the tensor
          // algorithms, SeedTest) fail once tracking starts rather than up front.
          // The engine's own index is carried in the item data so it stays correct
          // regardless of what is filtered out.
          // Nothing is selected to begin with, so a run is always a deliberate
          // choice of algorithm rather than whatever happened to be first.
          algorithm_combo->addItem (tr ("(select an algorithm)"));
          for (size_t i = 0; i != trackgen_num_algorithms(); ++i) {
            if (!trackgen_algorithm_uses_sh (i))
              continue;
            algorithm_combo->addItem (trackgen_algorithms[i], int (i));
          }
          algorithm_combo->setCurrentIndex (0);
          connect (algorithm_combo, SIGNAL (currentIndexChanged(int)), this, SLOT (algorithm_changed_slot(int)));
          source_layout->addWidget (algorithm_combo, 1, 1, 1, 2);

          // Guided replaces the algorithm and the whole Parameters group with one
          // question the user can actually answer: how long is this allowed to take.
          QLabel* quality_label = new QLabel (tr ("quality"));
          // The same cell the algorithm row uses: the two are never visible together,
          // and Guided should not leave a gap where Expert's algorithm row was.
          source_layout->addWidget (quality_label, 1, 0);
          guided_only.push_back (quality_label);
          quality_combo = new QComboBox (this);
          quality_combo->addItem (tr ("Fast - a look"));
          quality_combo->addItem (tr ("Balanced"));
          quality_combo->addItem (tr ("Thorough - dense"));
          quality_combo->setCurrentIndex (1);
          quality_combo->setToolTip (
              tr ("How many streamlines to aim for, and how long to keep seeding for them.\n"
                  "Fast is for checking that a bundle reconstructs at all; Thorough is for\n"
                  "the reconstruction you keep. These set the same \"select\" and seeding\n"
                  "budget the Expert panel exposes."));
          connect (quality_combo, SIGNAL (currentIndexChanged(int)), this, SLOT (quality_changed_slot(int)));
          source_layout->addWidget (quality_combo, 1, 1, 1, 2);
          guided_only.push_back (quality_combo);

          // Search first, then the two drop-downs it cuts across.
          source_layout->addWidget (new QLabel (tr ("find bundle")), 2, 0);
          bundle_filter = new QLineEdit (this);
          make_search_box (bundle_filter, tr ("Search bundles - e.g. AF, _L"));
          bundle_filter->setClearButtonEnabled (true);
          bundle_filter->setToolTip (tr ("Type any part of a bundle's name; picking a match selects it,\n"
                                         "switching category if it lives in another one"));
          source_layout->addWidget (bundle_filter, 2, 1, 1, 2);

          bundle_search_model = new QStringListModel (this);
          bundle_completer = new QCompleter (bundle_search_model, this);
          bundle_completer->setCaseSensitivity (Qt::CaseInsensitive);
          bundle_completer->setFilterMode (Qt::MatchContains);
          bundle_completer->setCompletionMode (QCompleter::PopupCompletion);
          bundle_completer->setMaxVisibleItems (12);
          bundle_filter->setCompleter (bundle_completer);
          connect (bundle_completer, SIGNAL (activated (const QString&)),
                   this, SLOT (bundle_search_activated (const QString&)));
          // The list narrows as you type as well as offering completions: the
          // completer only reaches bundles by exact name, and "show me every bundle
          // with _L in it" is the more common thing to want.
          connect (bundle_filter, SIGNAL (textChanged (const QString&)),
                   this, SLOT (bundle_filter_changed_slot (const QString&)));

          source_layout->addWidget (new QLabel (tr ("category")), 3, 0);
          category_combo = new QComboBox (this);
          category_combo->setToolTip (
              tr ("Which group of the atlas to choose from - association, projection,\n"
                  "commissural, cerebellum, cranial_nerve - plus \"loaded\" for bundles\n"
                  "opened with the button on the right.\n\n"
                  "Leave this on \"(none)\" to track from the regions below instead."));
          category_combo->addItem (tr ("(none - use regions)"));
          connect (category_combo, SIGNAL (currentIndexChanged(int)), this, SLOT (category_changed_slot(int)));
          source_layout->addWidget (category_combo, 3, 1);
          atlas_button = new QPushButton (tr ("Load..."), this);
          atlas_button->setToolTip (tr ("Load .tck bundles from a tract atlas, already in this subject's space"));
          connect (atlas_button, SIGNAL (clicked()), this, SLOT (browse_atlas_slot()));
          // Built-in atlas bundles appear in these drop-downs once they are aligned.
          connect (&window().atlas_registration(), SIGNAL (changed()),
                   this, SLOT (atlas_registration_changed()));
          source_layout->addWidget (atlas_button, 3, 2);

          source_layout->addWidget (new QLabel (tr ("bundles")), 4, 0);
          bundle_list = new QListWidget (this);
          bundle_list->setToolTip (
              tr ("Tick every bundle you want reconstructed. Each becomes its own tract:\n"
                  "the bundle's footprint is the tracking territory, its ends are inclusion\n"
                  "regions, and the result is filtered to the streamlines matching its shape.\n\n"
                  "Ticks are kept when you change category or search, so bundles from\n"
                  "different categories can go in one run."));
          bundle_list->setSelectionMode (QAbstractItemView::NoSelection);
          // Tall enough to see a handful without dominating the panel; the dock
          // scrolls if the user makes it smaller.
          bundle_list->setMinimumHeight (110);
          bundle_list->setMaximumHeight (190);
          connect (bundle_list, SIGNAL (itemChanged (QListWidgetItem*)),
                   this, SLOT (bundle_item_changed_slot (QListWidgetItem*)));
          source_layout->addWidget (bundle_list, 4, 1, 1, 2);

          // The ticked set is the one thing that is not visible in the list when a
          // filter or another category is showing, so it gets its own line.
          {
            HBoxLayout* selection_layout = new HBoxLayout;
            selection_label = new QLabel (tr ("none selected"));
            selection_label->setWordWrap (true);
            selection_layout->addWidget (selection_label, 1);
            clear_selection_button = new QPushButton (tr ("Clear"), this);
            clear_selection_button->setToolTip (tr ("Untick every bundle"));
            clear_selection_button->setEnabled (false);
            connect (clear_selection_button, SIGNAL (clicked()), this, SLOT (clear_selection_slot()));
            selection_layout->addWidget (clear_selection_button);
            QWidget* selection_row = new QWidget (this);
            selection_row->setLayout (selection_layout);
            source_layout->addWidget (selection_row, 5, 0, 1, 3);
          }

          // --- regions ---
          region_box = new QGroupBox (tr ("Regions"));
          main_box->addWidget (region_box, 1);
          VBoxLayout* region_layout = new VBoxLayout;
          region_box->setLayout (region_layout);

          region_table = new QTableWidget (this);
          region_table->setColumnCount (3);
          region_table->setHorizontalHeaderLabels ({ tr ("region"), tr ("from"), tr ("role") });
          region_table->horizontalHeader()->setStretchLastSection (false);
          region_table->horizontalHeader()->setSectionResizeMode (0, QHeaderView::Stretch);
          // A QTableWidget asks for room for every column at its natural width, which
          // is most of why this panel was wide. The first column stretches into
          // whatever there is, so the other two only need to be legible.
          region_table->horizontalHeader()->setMinimumSectionSize (44);
          region_table->setMinimumWidth (180);
          region_table->verticalHeader()->hide();
          region_table->setSelectionBehavior (QAbstractItemView::SelectRows);
          region_layout->addWidget (region_table);

          HBoxLayout* region_buttons = new HBoxLayout;
          add_region_button = new QPushButton (tr ("Add region..."), this);
          add_region_button->setToolTip (tr ("Use a region from the Overlay, Atlas or ROI editor tool"));
          connect (add_region_button, SIGNAL (clicked()), this, SLOT (add_region_slot()));
          region_buttons->addWidget (add_region_button);
          remove_region_button = new QPushButton (tr ("Remove"), this);
          connect (remove_region_button, SIGNAL (clicked()), this, SLOT (remove_region_slot()));
          region_buttons->addWidget (remove_region_button);
          region_layout->addLayout (region_buttons);

          // --- auto-tracking settings (only shown when a bundle is selected) ---
          autotrack_box = new QGroupBox (tr ("Bundle recognition"));
          main_box->addWidget (autotrack_box);
          GridLayout* at_layout = new GridLayout;
          autotrack_box->setLayout (at_layout);

          auto add_at_param = [&] (int row, int col, const QString& label, AdjustButton*& button,
                                   float rate, float initial, const QString& tip) {
            QLabel* l = new QLabel (label);
            l->setToolTip (tip);
            at_layout->addWidget (l, row, 2*col);
            button = new AdjustButton (this, rate);
            button->setValue (initial);
            button->setToolTip (tip);
            at_layout->addWidget (button, row, 2*col+1);
          };
          add_at_param (0, 0, tr ("dilate (mm)"), dilate_button, 0.5f, 2.0f,
                        tr ("how far to grow the bundle's footprint before tracking inside it;\n"
                            "too tight and tracking just reproduces the atlas"));
          add_at_param (0, 1, tr ("end dilate (mm)"), endpoint_dilate_button, 0.5f, 6.0f,
                        tr ("radius of the two endpoint inclusion regions"));
          add_at_param (1, 0, tr ("distance (mm)"), mdf_button, 0.5f, 4.0f,
                        tr ("maximum shape distance to the atlas bundle, in mm;\n"
                            "raise it to keep more streamlines, lower it to be stricter.\n"
                            "Set it to 0 to skip shape matching altogether and keep\n"
                            "everything tracked in the bundle's territory.\n"
                            "Its scale depends on the metric below, so it is reset when you\n"
                            "switch metric."));
          add_at_param (1, 1, tr ("QB radius (mm)"), qb_button, 0.5f, 8.0f,
                        tr ("clustering radius used to compress the atlas bundle before matching"));

          // Edits to these are remembered per bundle (autotrack_param_edited).
          for (AdjustButton* b : { mdf_button, qb_button, dilate_button })
            connect (b, SIGNAL (valueChanged()), this, SLOT (autotrack_param_edited()));

          endpoint_includes_box = new QCheckBox (tr ("require both endpoint regions"), this);
          endpoint_includes_box->setChecked (true);
          endpoint_includes_box->setToolTip (
              tr ("Streamlines must reach both ends of the atlas bundle.\n"
                  "Turn this off for bundles that fan out or terminate diffusely."));
          at_layout->addWidget (endpoint_includes_box, 2, 0, 1, 4);

          restrict_box = new QCheckBox (tr ("confine tracking to bundle territory"), this);
          restrict_box->setChecked (false);
          restrict_box->setToolTip (
              tr ("Keep streamlines inside the dilated bundle footprint.\n"
                  "This keeps results tight to the atlas but truncates them where they\n"
                  "leave it. Turn it off to let streamlines run their natural length and\n"
                  "rely on the shape match alone - usually better when the atlas came\n"
                  "from a different tracking pipeline."));
          at_layout->addWidget (restrict_box, 3, 0, 1, 4);

          at_layout->addWidget (new QLabel (tr ("shape metric")), 4, 0);
          metric_combo = new QComboBox (this);
          metric_combo->addItem (tr ("MDF (equal-length bundles)"));
          metric_combo->addItem (tr ("closest point, mean (tolerant)"));
          metric_combo->addItem (tr ("closest point, 90th pct (robust)"));
          metric_combo->addItem (tr ("Hausdorff (strictest)"));
          metric_combo->setCurrentIndex (3);   // Hausdorff: strictest, and the default
          mdf_button->setValue (metric_default_distance (3));
          connect (metric_combo, SIGNAL (currentIndexChanged(int)), this, SLOT (metric_changed_slot(int)));
          metric_combo->setToolTip (
              tr ("MDF compares streamlines at matched relative positions, so it only\n"
                  "works between streamlines of similar length.\n\n"
                  "Closest point averages each candidate vertex's distance to the nearest\n"
                  "bundle vertex. It needs no correspondence and tolerates a wide spread of\n"
                  "lengths, so it suits atlas bundles that are not length-uniform."));
          at_layout->addWidget (metric_combo, 4, 1, 1, 3);

          // --- parameters (shared by both modes) ---
          QGroupBox* param_box = new QGroupBox (tr ("Parameters (blank = tckgen default)"));
          main_box->addWidget (param_box);
          expert_only.push_back (param_box);
          GridLayout* param_layout = new GridLayout;
          param_box->setLayout (param_layout);

          auto add_param = [&] (int row, int col, const QString& label, AdjustButton*& button, float rate) {
            param_layout->addWidget (new QLabel (label), row, 2*col);
            button = new AdjustButton (this, rate);
            param_layout->addWidget (button, row, 2*col+1);
          };
          add_param (0, 0, tr ("select"),    select_button,    100.0f);
          select_button->setValue (1000.0f);
          add_param (0, 1, tr ("cutoff"),    cutoff_button,    0.01f);
          add_param (1, 0, tr ("step (mm)"), step_button,      0.05f);
          add_param (1, 1, tr ("angle (deg)"), angle_button,   1.0f);
          add_param (2, 0, tr ("min length"), minlength_button, 1.0f);
          add_param (2, 1, tr ("max length"), maxlength_button, 1.0f);

          param_layout->addWidget (new QLabel (tr ("seeds/voxel")), 3, 0);
          seeds_per_voxel_button = new QSpinBox (this);
          seeds_per_voxel_button->setRange (0, 10000);
          seeds_per_voxel_button->setValue (0);
          seeds_per_voxel_button->setToolTip (tr ("0 = seed randomly throughout the seed region (tckgen's default);\n"
                                                  "any other value seeds that many streamlines per seed voxel"));
          param_layout->addWidget (seeds_per_voxel_button, 3, 1);

          param_layout->addWidget (new QLabel (tr ("threads")), 3, 2);
          nthreads_button = new QSpinBox (this);
          nthreads_button->setRange (0, 256);
          nthreads_button->setValue (0);
          nthreads_button->setToolTip (tr ("0 = use all available cores"));
          param_layout->addWidget (nthreads_button, 3, 3);

          rk4_box = new QCheckBox (tr ("RK4"), this);
          param_layout->addWidget (rk4_box, 4, 0, 1, 2);
          stop_box = new QCheckBox (tr ("stop at include"), this);
          stop_box->setToolTip (tr ("stop propagating a streamline once it has traversed all include regions"));
          param_layout->addWidget (stop_box, 4, 2, 1, 2);
          unidirectional_box = new QCheckBox (tr ("unidirectional"), this);
          param_layout->addWidget (unidirectional_box, 5, 0, 1, 2);

          // --- recognition (settings for the *next* run) ---
          // These change what a run produces, so they belong above Generate where
          // they can be seen beforehand. Adjusting a result after the fact is a
          // property of that tract and lives in the Tracts list's Refine section.
          QGroupBox* recognition_box = new QGroupBox (tr ("Recognition"));
          main_box->addWidget (recognition_box);
          GridLayout* recognition_layout = new GridLayout;
          recognition_box->setLayout (recognition_layout);

          competitive_box = new QCheckBox (tr ("count a neighbouring bundle's better fit against a fibre"), this);
          competitive_box->setChecked (true);
          competitive_box->setToolTip (tr ("For each streamline, ask which atlas bundle it is closest to rather than only\nhow close it is to this one. A neighbour that fits it better does not delete\nit: it adds to the streamline's score, in proportion to how much better, so\nstrictness drops the contested ones first.\n\nMeasured on the projection category at 15 mm, 400 genuine and 400 bent\nstreamlines refined together: at 50% strictness all 400 genuine survive and 1\nof the bent ones does. Against the whole category, no medial lemniscus passes\nas corticospinal tract and no corticospinal streamline bent through the\nthalamus survives, while the tract itself keeps 400 of 400 - three more than\nrejecting outright ever kept."));
          connect (competitive_box, SIGNAL (toggled(bool)), this, SLOT (recognition_changed_slot()));
          recognition_layout->addWidget (competitive_box, 0, 0, 1, 3);

          // Read-only, and deliberately so. Each bundle of a run gets its own
          // neighbours from the atlas index - CST_L's are not ML_L's - so a single
          // editable list here could only be wrong for all but one of them. Choosing
          // by hand is a per-tract decision, and lives with the tract.
          neighbours_label = new QLabel ("");
          neighbours_label->setWordWrap (true);
          neighbours_label->setToolTip (
              tr ("Competing bundles are chosen per bundle, from the ones running through\n"
                  "the same territory. To change them for a tract you have already made,\n"
                  "use Refine in the tract list."));
          recognition_layout->addWidget (neighbours_label, 1, 0, 1, 3);

          QLabel* prune_title = new QLabel (tr ("prune outliers"));
          prune_title->setToolTip (
              tr ("Drop streamlines that sit far from the rest of what was kept, measured\n"
                  "against the result's own spread rather than a fixed distance."));
          recognition_layout->addWidget (prune_title, 2, 0);
          prune_combo = new QComboBox (this);
          prune_combo->addItem (tr ("off"));
          prune_combo->addItem (tr ("low"));
          prune_combo->addItem (tr ("medium"));
          prune_combo->addItem (tr ("high"));
          prune_combo->setToolTip (prune_title->toolTip());
          recognition_layout->addWidget (prune_combo, 2, 1, 1, 2);

          QLabel* strictness_title = new QLabel (tr ("strictness"));
          strictness_title->setToolTip (
              tr ("Drop this share of a run's streamlines - the ones furthest from the atlas\n"
                  "bundle. A share behaves the same on every bundle, where a distance in\n"
                  "millimetres does not: the value that is strict for a densely sampled\n"
                  "bundle rejects everything in a sparse one.\n\n"
                  "0% leaves the distance threshold in charge. This is the starting point for\n"
                  "new runs; each tract can then be adjusted on its own afterwards."));
          recognition_layout->addWidget (strictness_title, 3, 0);
          strictness_spin = new QSpinBox (this);
          // Higher is stricter and keeps less. The first version had this the other
          // way round - it set the share to *keep*, so raising "strictness" produced
          // more streamlines.
          strictness_spin->setRange (0, 90);
          // Half, not nothing. At 0 the millimetre threshold alone decides, and that
          // threshold has a 10 mm floor to absorb registration offset - wide enough
          // that a run keeps a visible tail of streamlines the tract does not want.
          // A share behaves the same way on every bundle, where a millimetre value
          // does not.
          // 0 by default: the millimetre threshold decides, and nothing is dropped
          // for being in the tail of its own distribution unless asked for.
          strictness_spin->setValue (0);
          // Same stops as the per-tract slider, so the arrows step between landmarks
          // rather than crawling a percent at a time; any value can still be typed.
          strictness_spin->setSingleStep (25);
          strictness_spin->setSuffix (tr (" %"));
          strictness_spin->setToolTip (strictness_title->toolTip());
          recognition_layout->addWidget (strictness_spin, 3, 1, 1, 2);

          // --- run ---
          HBoxLayout* run_layout = new HBoxLayout;
          generate_button = new QPushButton (tr ("Generate"), this);
          connect (generate_button, SIGNAL (clicked()), this, SLOT (generate_slot()));
          run_layout->addWidget (generate_button);
          cancel_button = new QPushButton (tr ("Cancel"), this);
          cancel_button->setEnabled (false);
          connect (cancel_button, SIGNAL (clicked()), this, SLOT (cancel_slot()));
          run_layout->addWidget (cancel_button);
          main_box->addLayout (run_layout);

          preview_box = new QCheckBox (tr ("show streamlines as they are selected"), this);
          preview_box->setChecked (true);
          preview_box->setToolTip (
              tr ("Draw the streamlines as tracking selects them, refreshed about once a\n"
                  "second. When auto-tracking a bundle these are the candidates; the ones\n"
                  "that fail the distance metric are removed when matching finishes.\n"
                  "Uploading them costs a little of each refresh, so turn this off for\n"
                  "very large runs."));
          main_box->addWidget (preview_box);
          expert_only.push_back (preview_box);

          show_rejected_button = new QPushButton (tr ("Show rejected as a tract"), this);
          show_rejected_button->setToolTip (
              tr ("List the streamlines the last auto-track run's distance metric threw\n"
                  "away, as a tract of their own. What the threshold removed is the only\n"
                  "way to tell a threshold that is too strict from a bundle that is\n"
                  "genuinely not there."));
          show_rejected_button->setEnabled (false);
          connect (show_rejected_button, SIGNAL (clicked()), this, SLOT (show_rejected_slot()));
          main_box->addWidget (show_rejected_button);
          expert_only.push_back (show_rejected_button);

          progress_bar = new QProgressBar (this);
          progress_bar->setTextVisible (true);
          progress_bar->setRange (0, 100);
          progress_bar->setValue (0);
          progress_bar->setFormat (tr ("idle"));
          main_box->addWidget (progress_bar);
          status_label = new QLabel ("");
          status_label->setWordWrap (true);
          main_box->addWidget (status_label);

          poll_timer = new QTimer (this);
          connect (poll_timer, SIGNAL (timeout()), this, SLOT (poll_progress_slot()));

          //CONF option: MRViewTrackGenMode
          //CONF default: guided
          //CONF Which face the Track generation tool opens with: "guided" shows only
          //CONF the controls a bundle reconstruction needs, "expert" shows all of them.
          mode_combo->setCurrentIndex (
              File::Config::get ("MRViewTrackGenMode", "guided") == "expert" ? 1 : 0);
          connect (mode_combo, SIGNAL (currentIndexChanged(int)), this, SLOT (mode_changed_slot(int)));

          // Narrow the panel. A combo box sizes itself to the widest item it will ever
          // hold, and this one holds bundle names, algorithm names and whole phrases -
          // so between them they were setting the width of the dock, and the dock sets
          // how much of the window is left for the image. Asking them to size to a
          // fixed short length instead lets the layout give them what is going spare
          // and elide the rest; the full text is still one click away in the popup.
          for (QComboBox* combo : findChildren<QComboBox*>()) {
            combo->setSizeAdjustPolicy (QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength (10);
          }
          // The bundle list is the same argument: its entries are short but its
          // tooltip and its scrollbar are not what should set a floor.
          bundle_list->setMinimumWidth (160);

          refresh_sources_slot();
          // The atlas may already be aligned when this tool is opened, in which case
          // no signal is coming and the list has to be built here.
          refresh_bundle_combo();
          apply_guided_defaults();
          update_mode();
        }



        TrackGen::~TrackGen ()
        {
          if (worker.joinable()) {
            state.cancel = true;
            worker.join();
          }
          // Nothing can cancel the index build, but it only reads files and writes to
          // module-level state, so waiting it out is both safe and short.
          if (footprint_worker.joinable())
            footprint_worker.join();
          restore_progress_hooks();
        }



        void TrackGen::restore_progress_hooks ()
        {
          // Only restore what we actually saved: overwriting with nullptr would
          // crash the next ProgressBar anywhere in the application.
          if (saved_progress_display) {
            MR::ProgressBar::display_func = saved_progress_display;
            saved_progress_display = nullptr;
          }
          if (saved_progress_done) {
            MR::ProgressBar::done_func = saved_progress_done;
            saved_progress_done = nullptr;
          }
          if (saved_exception_display) {
            MR::Exception::display_func = saved_exception_display;
            saved_exception_display = nullptr;
          }
          if (saved_report_to_user) {
            MR::report_to_user_func = saved_report_to_user;
            saved_report_to_user = nullptr;
          }
        }



        void TrackGen::refresh_sources_slot ()
        {
          const QString previous = source_combo->currentText();
          source_combo->clear();
          // Do not force the ODF tool open just to look: if it is closed the user
          // can still browse for an image.
          if (ODF* odf = get_tool<ODF> (false)) {
            for (const auto& entry : odf->list_sh_images())
              source_combo->addItem (qstr (entry.first), qstr (entry.second));
          }
          if (!source_combo->count())
            status_label->setText (tr ("No FOD image loaded; press \"Find\" to load one."));
          const int idx = source_combo->findText (previous);
          if (idx >= 0)
            source_combo->setCurrentIndex (idx);
        }



        void TrackGen::find_sources_slot ()
        {
          // "Find" means "go and get me an FOD", whether or not one is already
          // loaded: a second subject, or the right image after the wrong one, is
          // just as common a reason to press it as the first image is. Re-scanning
          // the ODF tool happens anyway, on every press and on every panel update,
          // so it is not what the button is for.
          const int before = source_combo->count();
          refresh_sources_slot();
          if (source_combo->count() > before)
            return;   // the scan alone found something new; that was the ask
          load_fod_source();
        }



        void TrackGen::raise_host_panel ()
        {
          // Whichever tool hosts this widget - it is a section of the Tracts panel
          // now, and was its own dock before - the dock is the first one above us.
          for (QWidget* w = this; w; w = w->parentWidget()) {
            if (QDockWidget* dock = qobject_cast<QDockWidget*> (w)) {
              dock->show();
              dock->raise();
              return;
            }
          }
        }



        bool TrackGen::load_fod_source ()
        {
          ODF* odf = get_tool<ODF> ();
          if (!odf) {
            QMessageBox::warning (this, "Track generation",
                "Could not open the ODF display tool to load an FOD image.");
            return false;
          }

          // Show the tool, so it is clear where the image is being loaded and
          // where to load further ones from later.
          for (QWidget* w = odf; w; w = w->parentWidget()) {
            if (QDockWidget* dock = qobject_cast<QDockWidget*> (w)) {
              dock->show();
              dock->raise();
              break;
            }
          }

          const bool loaded = odf->prompt_load_sh_image();
          refresh_sources_slot();
          // Back to this panel: the ODF tool was opened to load from, not to work
          // in, and leaving it in front strands the user one panel away from the
          // Generate button they were heading for.
          raise_host_panel();
          if (loaded && source_combo->count()) {
            // Select what was just loaded rather than leaving the previous choice
            // in place - loading an image is a statement about which one to use.
            source_combo->setCurrentIndex (source_combo->count() - 1);
            return true;
          }
          if (!source_combo->count())
            status_label->setText (tr ("No FOD image loaded; press \"Find\" to load one."));
          return false;
        }



        bool TrackGen::ensure_fod_source ()
        {
          refresh_sources_slot();
          if (source_combo->count())
            return true;
          return load_fod_source();
        }



        int TrackGen::current_algorithm () const
        {
          // The combo only lists FOD-capable algorithms, so its row is not the
          // engine's index; that is carried in the item data. Row 0 is the
          // "(select an algorithm)" placeholder, which carries none.
          const QVariant data = algorithm_combo->currentData();
          return data.isValid() ? data.toInt() : -1;
        }



        void TrackGen::algorithm_changed_slot (int)
        {
          const int algorithm = current_algorithm();
          if (algorithm < 0) {
            rk4_box->setEnabled (false);
            rk4_box->setChecked (false);
            rk4_box->setToolTip (tr ("Select an algorithm first"));
            return;
          }
          suggest_thresholds_for_bundle();
          const bool rk4_ok = trackgen_algorithm_uses_rk4 (size_t (algorithm));
          rk4_box->setEnabled (rk4_ok);
          if (!rk4_ok) {
            rk4_box->setChecked (false);
            rk4_box->setToolTip (tr ("Not available for this algorithm: it rejects "
                                     "4th-order Runge-Kutta integration"));
          } else {
            rk4_box->setToolTip (tr ("4th-order Runge-Kutta integration: more accurate "
                                     "per step, but roughly four times the cost"));
          }
        }



        std::string TrackGen::bundle_path_for (const std::string& data) const
        {
          if (data.empty())
            return std::string();
          // A built-in bundle lives in the aligned atlas held in memory, so it has to
          // be written out before anything that reads bundles by path can use it.
          if (data.compare (0, 8, "builtin:") == 0)
            return const_cast<TrackGen*> (this)->materialise_builtin_bundle (data.substr (8));
          return data;
        }



        vector<std::string> TrackGen::selected_bundle_names () const
        {
          // Catalogue order, not tick order: a batch should come out in the same
          // order every time it is run, and the set is unordered.
          vector<std::string> out;
          for (const auto& entry : catalogue)
            if (checked_bundles.count (entry.name))
              out.push_back (entry.name);
          return out;
        }



        std::string TrackGen::current_bundle () const
        {
          // The first ticked bundle. Only for the paths that deal in one bundle -
          // the session, the command line, and sizing the thresholds shown in the
          // panel; a run iterates selected_bundle_names() instead.
          for (const auto& entry : catalogue) {
            if (checked_bundles.count (entry.name))
              return bundle_path_for (entry.data);
          }
          return std::string();
        }



        // Atlas bundles offered as regions, so a manual run can seed from (or
        // include/exclude) a bundle's footprint without switching to auto-track.
        // The mask is the same rasterised, dilated territory auto-track builds.
        class TrackGen::BundleRegions : public RegionProvider
        { NOMEMALIGN
          public:
            BundleRegions (TrackGen& tool) : tool (tool) { }

            std::string provider_name () const override { return "Atlas bundle"; }

            void list_regions (vector<RegionRef>& out) const override
            {
              // From the catalogue, not from the bundle drop-down: that one holds only
              // the category on show, so every other category's bundles would be
              // unavailable as regions - and when it is empty (no category selected)
              // walking it with "i != count()" from 1 never terminates, which is
              // exactly how this hung.
              for (size_t i = 0; i != tool.catalogue.size(); ++i) {
                const auto& entry = tool.catalogue[i];
                if (entry.data.empty())
                  continue;
                RegionRef ref;
                ref.provider = provider_name();
                ref.name = entry.category.size() ? entry.category + "/" + entry.name : entry.name;
                ref.key = key_prefix + entry.data;
                ref.colour = QColor (120, 200, 255);
                ref.index = i;
                out.push_back (ref);
              }
            }

            MR::Image<bool> get_region_mask (const RegionRef& ref) const override
            {
              const std::string path = tool.bundle_path_for (data_of (ref));
              if (path.empty())
                throw Exception ("atlas bundle \"" + ref.name + "\" is no longer available");

              vector<MR::DWI::Tractography::Streamline<float>> tracks;
              {
                MR::DWI::Tractography::Properties props;
                MR::DWI::Tractography::Reader<float> reader (path, props);
                MR::DWI::Tractography::Streamline<float> tck;
                while (reader (tck))
                  tracks.push_back (tck);
              }
              if (tracks.empty())
                throw Exception ("atlas bundle \"" + ref.name + "\" is empty");

              MR::Header grid = tool.region_grid();
              auto mask = rasterise_streamlines (tracks, grid);
              // Same dilation auto-tracking uses: an undilated footprint is a
              // one-voxel shell that barely seeds.
              dilate_mask (mask, tool.dilate_button->value());
              return mask;
            }

            bool resolve (const std::string& key, RegionRef& out) const override
            {
              if (key.compare (0, std::strlen (key_prefix), key_prefix) != 0)
                return false;
              vector<RegionRef> all;
              list_regions (all);
              for (const auto& ref : all) {
                if (ref.key == key) { out = ref; return true; }
              }
              return false;
            }

          private:
            TrackGen& tool;
            static constexpr const char* key_prefix = "trackgen-bundle:";
            static std::string data_of (const RegionRef& ref) {
              return ref.key.substr (std::strlen (key_prefix));
            }
        };



        RegionProvider* TrackGen::region_provider ()
        {
          if (!bundle_regions)
            bundle_regions.reset (new BundleRegions (*this));
          return bundle_regions.get();
        }



        MR::Header TrackGen::region_grid () const
        {
          // The FOD is what tracking runs on, so its grid is the natural one for a
          // region derived here; fall back to the viewed image when none is set.
          if (source_combo->count()) {
            try {
              MR::Header H = MR::Header::open (source_combo->currentData().toString().toStdString());
              H.ndim() = 3;
              return H;
            } catch (Exception&) { }
          }
          if (!window().image())
            throw Exception ("no image to build the region on; load an FOD or an image first");
          MR::Header H (window().image()->header());
          H.ndim() = 3;
          return H;
        }



        std::string TrackGen::materialise_builtin_bundle (const std::string& name)
        {
          auto cached = builtin_cache.find (name);
          if (cached != builtin_cache.end())
            return cached->second;

          auto& registration = window().atlas_registration();
          const auto& tracks = registration.bundle (name);
          if (tracks.empty())
            return std::string();

          if (!builtin_dir) {
            builtin_dir.reset (new QTemporaryDir);
            if (!builtin_dir->isValid()) {
              builtin_dir.reset();
              return std::string();
            }
          }
          const std::string path = builtin_dir->filePath (qstr (name + ".tck")).toStdString();
          try {
            MR::DWI::Tractography::Properties properties;
            properties["source"] = "built-in tract atlas";
            properties["atlas_bundle"] = name;
            properties["atlas_alignment"] = registration.note();
            MR::DWI::Tractography::Writer<float> writer (path, properties);
            for (const auto& tck : tracks)
              writer (tck);
          } catch (Exception& e) {
            e.display();
            return std::string();
          }
          builtin_cache[name] = path;
          return path;
        }



        void TrackGen::atlas_registration_changed ()
        {
          // Bundles from a previous alignment are in the wrong space; drop the
          // materialised copies so they are rewritten from the new one.
          builtin_cache.clear();
          refresh_bundle_combo();
        }



        void TrackGen::refresh_bundle_combo ()
        {
          const QString previous_category = category_combo->currentText();

          // Categories first: an atlas laid out as <category>/<bundle>.tck is far
          // too long to browse as one list (102 bundles in the HCP1065 average), so
          // the category narrows it and the search box cuts across both.
          catalogue.clear();
          for (const auto& path : user_bundle_paths) {
            std::string name = Path::basename (path);
            const size_t dot = name.find_last_of ('.');
            if (dot != std::string::npos)
              name = name.substr (0, dot);
            catalogue.push_back ({ "loaded", name, path });
          }
          auto& registration = window().atlas_registration();
          if (registration.state() == AtlasRegistration::State::Ready) {
            for (const auto& ref : registration.catalogue()) {
              catalogue.push_back ({ ref.category.size() ? ref.category : std::string ("atlas"),
                                     "atlas_" + ref.name, "builtin:" + ref.name });
            }
          }

          category_combo->blockSignals (true);
          category_combo->clear();
          category_combo->addItem (tr ("(none - use regions)"));
          vector<std::string> categories;
          for (const auto& entry : catalogue) {
            if (std::find (categories.begin(), categories.end(), entry.category) == categories.end())
              categories.push_back (entry.category);
          }
          for (const auto& category : categories)
            category_combo->addItem (qstr (category));
          const int cat_idx = category_combo->findText (previous_category);
          category_combo->setCurrentIndex (cat_idx >= 0 ? cat_idx : 0);
          category_combo->blockSignals (false);

          refresh_bundles_in_category (bundle_filter->text());

          // The completer searches every bundle, whatever category is showing, so a
          // name is enough to find one without knowing where it lives.
          QStringList all;
          for (const auto& entry : catalogue)
            all << qstr (entry.name);
          bundle_search_model->setStringList (all);

          update_mode();
        }



        void TrackGen::refresh_bundles_in_category (const QString& filter)
        {
          // Rebuilding sets check states, which emits itemChanged; without this the
          // ticked set would be rewritten from a half-built list.
          bundle_list->blockSignals (true);
          bundle_list->clear();
          const std::string category = category_combo->currentIndex() <= 0
                                     ? std::string()
                                     : category_combo->currentText().toStdString();
          auto& registration = window().atlas_registration();
          const std::string needle = filter.toLower().toStdString();

          auto tooltip_for = [&] (const BundleEntry& entry) {
            return qstr (entry.data.compare (0, 8, "builtin:") == 0
                         ? entry.category + " - " + registration.note()
                         : entry.data);
          };
          auto add_divider = [&] (const QString& text) {
            QListWidgetItem* item = new QListWidgetItem (text, bundle_list);
            item->setFlags (Qt::NoItemFlags);           // not checkable, not selectable
            QFont font = item->font();
            font.setItalic (true);
            item->setFont (font);
          };
          auto add_bundle = [&] (const BundleEntry& entry) {
            QListWidgetItem* item = new QListWidgetItem (qstr (entry.name), bundle_list);
            item->setFlags (item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState (checked_bundles.count (entry.name) ? Qt::Checked : Qt::Unchecked);
            item->setData (Qt::UserRole, qstr (entry.data));
            item->setToolTip (tooltip_for (entry));
          };

          // Ticked bundles first, and unconditionally: a tick that stops matching the
          // search - or that lives in another category - is still in the next run, so
          // hiding it hides the run's own contents. Whatever the filter says, what is
          // selected stays on screen.
          const vector<std::string> selected = selected_bundle_names();
          if (selected.size()) {
            add_divider (tr ("\u2014 selected (%1) \u2014").arg (uint64_t (selected.size())));
            for (const auto& entry : catalogue)
              if (checked_bundles.count (entry.name))
                add_bundle (entry);
          }

          // Then the category, narrowed by the search, skipping anything pinned above.
          vector<const BundleEntry*> matches;
          if (category.size()) {
            for (const auto& entry : catalogue) {
              if (entry.category != category || checked_bundles.count (entry.name))
                continue;
              if (needle.size()) {
                std::string lower = entry.name;
                std::transform (lower.begin(), lower.end(), lower.begin(),
                                [] (unsigned char c) { return std::tolower (c); });
                if (lower.find (needle) == std::string::npos)
                  continue;
              }
              matches.push_back (&entry);
            }
          }
          if (matches.size()) {
            if (selected.size())
              add_divider (needle.size() ? tr ("\u2014 matches \u2014") : tr ("\u2014 more \u2014"));
            for (const BundleEntry* entry : matches)
              add_bundle (*entry);
          }

          bundle_list->setEnabled (bundle_list->count());
          bundle_list->blockSignals (false);
          sync_bundle_checks();
        }



        void TrackGen::sync_bundle_checks ()
        {
          const vector<std::string> names = selected_bundle_names();

          // The count lives in two places that are always on screen: the divider row
          // at the top of the list and the Generate button. The old standalone
          // "Selected: a, b, c" line said the same thing a third time, so it is now
          // just the hint for having selected nothing at all.
          selection_label->setText (names.empty()
              ? tr ("nothing selected - tick a bundle, or track from regions below")
              : QString());
          selection_label->setVisible (names.empty());
          clear_selection_button->setEnabled (names.size());

          // Ticking does not rebuild the list: the row would jump out from under the
          // cursor mid-click. So the divider's count is updated in place, and the
          // pinned block reorders on the next change of filter or category.
          for (int i = 0; i != bundle_list->count(); ++i) {
            QListWidgetItem* item = bundle_list->item (i);
            if (item->flags() != Qt::NoItemFlags || !item->text().contains (tr ("selected")))
              continue;
            item->setText (tr ("\u2014 selected (%1) \u2014").arg (uint64_t (names.size())));
            break;
          }

          generate_button->setText (names.empty() ? tr ("Generate")
                                  : names.size() == 1 ? tr ("Reconstruct bundle")
                                  : tr ("Reconstruct %1 bundles").arg (uint64_t (names.size())));
        }



        void TrackGen::bundle_item_changed_slot (QListWidgetItem* item)
        {
          if (!item)
            return;
          const std::string name = item->text().toStdString();
          const bool checked = item->checkState() == Qt::Checked;
          const bool was = checked_bundles.count (name);
          if (checked == was)
            return;
          if (checked)
            checked_bundles.insert (name);
          else
            checked_bundles.erase (name);
          sync_bundle_checks();
          update_mode();
          // Thresholds are shown for the first ticked bundle, which may have just
          // changed.
          suggest_thresholds_for_bundle();
        }



        void TrackGen::clear_selection_slot ()
        {
          checked_bundles.clear();
          refresh_bundles_in_category (bundle_filter->text());
          update_mode();
        }



        void TrackGen::category_changed_slot (int)
        {
          // The filter applies within the new category too, so it is carried over
          // rather than cleared: typing "AF" then switching category should show
          // that category's AF bundles, not all of them.
          refresh_bundles_in_category (bundle_filter->text());
          update_mode();
        }



        void TrackGen::bundle_search_activated (const QString& text)
        {
          // The match may live in another category, so move both combos to it.
          const std::string name = text.toStdString();
          std::string category;
          for (const auto& entry : catalogue)
            if (entry.name == name) { category = entry.category; break; }
          if (category.empty())
            return;
          const int cat_idx = category_combo->findText (qstr (category));
          if (cat_idx < 0)
            return;
          if (category_combo->currentIndex() != cat_idx) {
            category_combo->blockSignals (true);
            category_combo->setCurrentIndex (cat_idx);
            category_combo->blockSignals (false);
          }
          // Searching a bundle ticks it: with several bundles per run, finding one
          // by name and then having to find it again in the list to tick it would be
          // the wrong half of the job.
          checked_bundles.insert (name);
          // Cleared so the list shows the whole category around the new tick, and the
          // box is ready for the next name rather than holding a stale term.
          bundle_filter->clear();
          refresh_bundles_in_category (QString());
          update_mode();
          suggest_thresholds_for_bundle();
        }



        void TrackGen::browse_atlas_slot ()
        {
          // Accept individual bundle files: an atlas is usually a folder of them,
          // but the user may only care about a few.
          const QStringList files = QFileDialog::getOpenFileNames (this,
              tr ("Select atlas bundle files (.tck), already in this subject's space"),
              qstr (current_folder), "Track files (*.tck)");
          if (files.isEmpty())
            return;
          vector<std::string> paths;
          for (const QString& f : files)
            paths.push_back (f.toStdString());
          if (paths.size())
            current_folder = Path::dirname (paths[0]);
          load_atlas_bundles (paths);
        }



        void TrackGen::load_atlas_bundles (const vector<std::string>& paths)
        {
          user_bundle_paths = paths;
          refresh_bundle_combo();
          if (paths.size()) {
            // Show what was just loaded: the "loaded" category, first bundle.
            const int idx = category_combo->findText (tr ("loaded"));
            if (idx > 0)
              category_combo->setCurrentIndex (idx);
          }
        }



        void TrackGen::autotrack_param_edited ()
        {
          if (applying_suggestion)
            return;
          const std::string path = current_bundle();
          if (path.empty())
            return;
          BundleParams& saved = bundle_overrides[path];
          saved.distance = mdf_button->value();
          saved.qb = qb_button->value();
          saved.dilate = dilate_button->value();
        }



        void TrackGen::bundle_filter_changed_slot (const QString& text)
        {
          refresh_bundles_in_category (text);
        }



        void TrackGen::suggest_thresholds_for_bundle ()
        {
          const std::string path = current_bundle();
          if (path.empty())
            return;

          // An edit belongs to the bundle it was made on. Switching to another
          // bundle brings back that bundle's own values - its override if it has
          // one, otherwise the suggestion - rather than carrying the last typed
          // number across every bundle.
          const auto override_entry = bundle_overrides.find (path);
          if (override_entry != bundle_overrides.end()) {
            const BundleParams& saved = override_entry->second;
            applying_suggestion = true;
            if (std::isfinite (saved.distance)) mdf_button->setValue (saved.distance);
            if (std::isfinite (saved.qb))       qb_button->setValue (saved.qb);
            if (std::isfinite (saved.dilate))   dilate_button->setValue (saved.dilate);
            applying_suggestion = false;
            return;
          }

          try {
            vector<MR::DWI::Tractography::Streamline<float>> bundle;
            MR::DWI::Tractography::Properties properties;
            MR::DWI::Tractography::Reader<float> reader (path, properties);
            MR::DWI::Tractography::Streamline<float> tck;
            while (reader (tck))
              bundle.push_back (tck);

            const auto scale = MR::DWI::Tractography::Recognition::suggest_bundle_thresholds (
                bundle, metric_combo->currentIndex());
            suggested_distance = scale.distance * algorithm_distance_factor (current_algorithm());
            suggested_qb = scale.qb_radius;
            // Dilation is about giving tracking room to move inside the territory,
            // so it follows the tracking image's voxel size rather than anything
            // about the bundle: one and a half voxels, which is thick enough for a
            // step to stay inside the mask without swallowing neighbouring tracts.
            suggested_dilate = 3.0f;
            try {
              const std::string source = source_combo->currentData().toString().toStdString();
              if (source.size()) {
                const MR::Header header = MR::Header::open (source);
                const float voxel = float ((header.spacing(0) + header.spacing(1) + header.spacing(2)) / 3.0);
                suggested_dilate = std::min (6.0f, std::max (2.0f, 1.5f * voxel));
              }
            } catch (Exception&) { }
            applying_suggestion = true;
            mdf_button->setValue (suggested_distance);
            qb_button->setValue (suggested_qb);
            dilate_button->setValue (suggested_dilate);
            applying_suggestion = false;
            const float fixed = metric_default_distance (metric_combo->currentIndex());
            status_label->setText (suggested_distance > fixed
                ? QString ("%1: %2 streamlines, sparsely sampled - distance relaxed to %3 mm")
                    .arg (qstr (Path::basename (path))).arg (uint64_t (bundle.size()))
                    .arg (double (suggested_distance), 0, 'f', 1)
                : QString ("%1: %2 streamlines, distance %3 mm")
                    .arg (qstr (Path::basename (path))).arg (uint64_t (bundle.size()))
                    .arg (double (suggested_distance), 0, 'f', 1));
          } catch (Exception& e) {
            // Not worth interrupting for: the fixed defaults still apply.
            DEBUG ("could not size thresholds for \"" + path + "\": " + e[0]);
          }
        }



        float TrackGen::algorithm_distance_factor (int algorithm)
        {
          // How many candidates an algorithm produces, and how closely they hug the
          // fibre orientations, changes what threshold is appropriate. A
          // probabilistic algorithm samples the whole orientation distribution, so
          // it returns far more pathways per seed and can be filtered harder without
          // running out of them; a deterministic one follows the peak and returns
          // fewer, so the same threshold leaves too little.
          //
          // These factors are judgement, not measurement - they have not been
          // calibrated against ground truth, and they only shift the suggestion,
          // which is always editable.
          switch (algorithm) {
            case 0:  return 1.20f;   // FACT: deterministic, and jagged at crossings
            case 5:  return 1.10f;   // SD_STREAM: deterministic, smooth
            case 1:  return 1.00f;   // iFOD1: probabilistic, first order
            case 2:  return 0.90f;   // iFOD2: probabilistic, smoother paths
            default: return 1.00f;
          }
        }



        float TrackGen::metric_default_distance (int metric_index)
        {
          switch (metric_index) {
            case 0:  return 10.0f;   // MDF
            case 1:  return 4.0f;    // closest point, mean
            case 2:  return 4.0f;    // closest point, 90th percentile
            default: return 10.0f;   // Hausdorff
          }
        }



        void TrackGen::metric_changed_slot (int index)
        {
          // The threshold is a distance in mm for every metric, but they put it on
          // quite different scales, so each carries its own default - swapped in
          // unless the user has typed something that is not one of them.
          const float current = mdf_button->value();
          const float wanted = metric_default_distance (index);
          bool user_supplied = std::isfinite (current);
          for (int i = 0; user_supplied && i != metric_combo->count(); ++i) {
            if (current == metric_default_distance (i))
              user_supplied = false;
          }
          if (!user_supplied)
            mdf_button->setValue (wanted);
          // The estimate is made with the selected metric, so it has to be redone.
          suggest_thresholds_for_bundle();
        }



        void TrackGen::update_mode ()
        {
          // Ticking a bundle *is* the switch between the two ways of tracking: the
          // territory and inclusion regions come from the bundle, so the manual
          // region table has nothing to contribute and would only be confusing.
          const bool autotrack = checked_bundles.size();
          autotrack_box->setVisible (autotrack && !guided());
          region_box->setVisible (!autotrack && !guided());
          sync_bundle_checks();   // sets the Generate button's text
          apply_mode();
          update_neighbours_label();
        }



        bool TrackGen::same_tracking_setup (const AutotrackParams& a, const AutotrackParams& b)
        {
          // Everything except .match: those are the settings a re-match is allowed to
          // change. Anything here alters which candidates exist at all.
          return a.algorithm == b.algorithm
              && a.select == b.select
              && a.seeds_per_voxel == b.seeds_per_voxel
              && a.max_seeds_factor == b.max_seeds_factor
              && a.dilate_mm == b.dilate_mm
              && a.endpoint_dilate_mm == b.endpoint_dilate_mm
              && a.use_endpoint_includes == b.use_endpoint_includes
              && a.restrict_to_territory == b.restrict_to_territory
              && a.scalars == b.scalars;
        }



        AutotrackParams TrackGen::collect_autotrack_params () const
        {
          AutotrackParams params;
          params.algorithm = current_algorithm();
          params.nthreads = nthreads_button->value();
          params.seeds_per_voxel = seeds_per_voxel_button->value();
          params.select = std::isfinite (select_button->value())
                        ? size_t (std::max (1.0f, select_button->value())) : 10000;
          params.dilate_mm = dilate_button->value();
          params.endpoint_dilate_mm = endpoint_dilate_button->value();
          params.use_endpoint_includes = endpoint_includes_box->isChecked();
          params.restrict_to_territory = restrict_box->isChecked();
          params.match.max_mdf = mdf_button->value();
          params.match.qb_threshold = qb_button->value();
          params.max_seeds_factor = max_seeds_factor;
          // The post-tracking gates belong to the parameters, so the command-line
          // path (run_and_save) applies exactly what the panel would.
          params.refine = collect_refine_options();
          using Metric = MR::DWI::Tractography::Recognition::BundleMatcher::Metric;
          switch (metric_combo->currentIndex()) {
            case 1:  params.match.metric = Metric::ClosestMean; break;
            case 2:  params.match.metric = Metric::ClosestP90;  break;
            case 3:  params.match.metric = Metric::Hausdorff;   break;
            default: params.match.metric = Metric::MDF;         break;
          }

          // Auto-tracking gets the same tracking parameters as manual mode; that
          // is the main practical gain from having one tool rather than two.
          auto set_if_valid = [&] (const std::string& key, AdjustButton* button) {
            if (std::isfinite (button->value()))
              params.scalars[key] = str (button->value());
          };
          set_if_valid ("threshold", cutoff_button);
          set_if_valid ("step_size", step_button);
          set_if_valid ("max_angle", angle_button);
          set_if_valid ("min_dist",  minlength_button);
          set_if_valid ("max_dist",  maxlength_button);
          if (rk4_box->isChecked())            params.scalars["rk4"] = "1";
          if (unidirectional_box->isChecked()) params.scalars["unidirectional"] = "1";
          return params;
        }



        void TrackGen::add_region_slot ()
        {
          vector<RegionRef> available;
          collect_regions (available);

          // Pick the region and its role in one go, rather than adding it and then
          // editing the role in the table.
          QMenu menu (this);

          // Draw a fresh region without leaving this panel.
          QMenu* fresh = menu.addMenu (tr ("New region (ROI editor)..."));
          for (int r = 0; r != num_roles; ++r) {
            QAction* action = fresh->addAction (tr ("as %1").arg (role_names[r]));
            action->setData (qstr (str(r) + ":" + std::string ("<new>")));
          }
          if (available.size())
            menu.addSeparator();
          build_region_menu (menu, available, [this] (QMenu* sub, const RegionRef& region) {
            for (int r = 0; r != num_roles; ++r) {
              QAction* action = sub->addAction (tr ("as %1").arg (role_names[r]));
              action->setData (qstr (str(r) + ":" + region.key));
            }
          });

          QAction* chosen = menu.exec (add_region_button->mapToGlobal (QPoint (0, add_region_button->height())));
          if (!chosen)
            return;
          const std::string data = chosen->data().toString().toStdString();
          const size_t colon = data.find (':');
          if (colon == std::string::npos)
            return;
          const int role = to<int> (data.substr (0, colon));
          std::string key = data.substr (colon+1);

          if (key == "<new>") {
            // Creating the region opens the ROI editor, so the user can draw into
            // it straight away; it is added here with the chosen role.
            try {
              ROI* roi_tool = get_tool<ROI>();
              if (!roi_tool)
                throw Exception ("could not open the ROI editor");
              Assignment a;
              a.region = roi_tool->create_region();
              a.role = role;
              assignments.push_back (a);
            } catch (Exception& e) {
              QMessageBox::warning (this, "New region", qstr (e[0]));
              return;
            }
            rebuild_region_table();
            return;
          }

          for (const auto& region : available) {
            if (region.key != key)
              continue;
            Assignment a;
            a.region = region;
            a.role = role;
            assignments.push_back (a);
            break;
          }
          rebuild_region_table();
        }



        void TrackGen::remove_region_slot ()
        {
          const int row = region_table->currentRow();
          if (row < 0 || row >= int (assignments.size()))
            return;
          assignments.erase (assignments.begin() + row);
          rebuild_region_table();
        }



        void TrackGen::apply_region_opacities ()
        {
          // Mirror the Tractography tool: a region acting as "exclude" is dimmed so
          // its role is visible in the viewer. Only the ROI editor implements this.
          for (const auto& a : assignments) {
            if (RegionProvider* provider = provider_for (a.region))
              provider->set_region_opacity (a.region,
                  a.role == EXCLUDE ? avoid_region_opacity : 1.0f);
          }
        }



        void TrackGen::rebuild_region_table ()
        {
          region_table->setRowCount (assignments.size());
          for (size_t i = 0; i != assignments.size(); ++i) {
            const Assignment& a = assignments[i];

            QTableWidgetItem* name_item = new QTableWidgetItem (qstr (a.region.name));
            QPixmap swatch (12, 12);
            swatch.fill (a.region.colour);
            name_item->setIcon (QIcon (swatch));
            name_item->setFlags (name_item->flags() & ~Qt::ItemIsEditable);
            region_table->setItem (i, 0, name_item);

            QTableWidgetItem* from_item = new QTableWidgetItem (qstr (a.region.provider));
            from_item->setFlags (from_item->flags() & ~Qt::ItemIsEditable);
            region_table->setItem (i, 1, from_item);

            QComboBox* role = new QComboBox (this);
            for (int r = 0; r != num_roles; ++r)
              role->addItem (role_names[r]);
            role->setCurrentIndex (a.role);
            connect (role, QOverload<int>::of (&QComboBox::currentIndexChanged), this,
                [this, i] (int value) {
                  if (i < assignments.size()) {
                    assignments[i].role = value;
                    apply_region_opacities();
                  }
                });
            region_table->setCellWidget (i, 2, role);
          }
          region_table->resizeColumnToContents (1);
          region_table->resizeColumnToContents (2);
          apply_region_opacities();
        }



        void TrackGen::set_controls_enabled (bool on)
        {
          generate_button->setEnabled (on);
          cancel_button->setEnabled (!on);
          source_combo->setEnabled (on);
          algorithm_combo->setEnabled (on);
          add_region_button->setEnabled (on);
          remove_region_button->setEnabled (on);
          bundle_list->setEnabled (on && bundle_list->count());
          clear_selection_button->setEnabled (on && checked_bundles.size());
          quality_combo->setEnabled (on);
          mode_combo->setEnabled (on);
          competitive_box->setEnabled (on);
          prune_combo->setEnabled (on);
          strictness_spin->setEnabled (on);
        }



        void TrackGen::generate_slot ()
        {
          if (state.running)
            return;

          auto request = std::make_shared<Request>();

          // Nothing loaded: ask for an FOD here rather than sending the user away.
          if (!ensure_fod_source())
            return;
          request->source_path = source_combo->currentData().toString().toStdString();
          request->algorithm = current_algorithm();
          if (request->algorithm < 0) {
            QMessageBox::information (this, "Track generation",
                "Select a tracking algorithm first.");
            return;
          }

          // FACT tracks from peak directions, not from SH. Derive them from the
          // selected FOD into a temporary file, so the same FOD source works for
          // every algorithm on offer. Cached per source: extraction is the
          // expensive part of a FACT run, and the FOD does not change underneath us.
          if (trackgen_algorithm_needs_peaks (size_t (request->algorithm))) {
            auto cached = peaks_cache.find (request->source_path);
            if (cached != peaks_cache.end()) {
              request->source_path = cached->second;
            } else {
              if (!peaks_dir) {
                peaks_dir.reset (new QTemporaryDir);
                if (!peaks_dir->isValid()) {
                  peaks_dir.reset();
                  QMessageBox::warning (this, "Track generation",
                      "Could not create a temporary directory for peak extraction.");
                  return;
                }
              }
              const std::string out = peaks_dir->filePath (
                  qstr (Path::basename (request->source_path) + "_peaks.mif")).toStdString();
              try {
                status_label->setText (tr ("extracting peak directions for FACT..."));
                QApplication::processEvents();
                sh_to_peaks (request->source_path, out);
                peaks_cache[request->source_path] = out;
                request->source_path = out;
              } catch (Exception& e) {
                QMessageBox::warning (this, "Track generation",
                    qstr ("Could not extract peaks for FACT: " + e[0]));
                return;
              }
            }
          }
          request->nthreads = nthreads_button->value();
          request->seeds_per_voxel = seeds_per_voxel_button->value();

          // Blank fields are left unset, so Properties::set() records the
          // algorithm's own default rather than something we invented.
          auto set_if_valid = [&] (const std::string& key, AdjustButton* button) {
            const float value = button->value();
            if (std::isfinite (value))
              request->scalars[key] = str (value);
          };
          set_if_valid ("max_num_tracks", select_button);
          set_if_valid ("threshold",      cutoff_button);
          set_if_valid ("step_size",      step_button);
          set_if_valid ("max_angle",      angle_button);
          set_if_valid ("min_dist",       minlength_button);
          set_if_valid ("max_dist",       maxlength_button);
          if (rk4_box->isChecked())            request->scalars["rk4"] = "1";
          if (stop_box->isChecked())           request->scalars["stop_on_all_include"] = "1";
          if (unidirectional_box->isChecked()) request->scalars["unidirectional"] = "1";

          // Every ticked bundle is one job. Names, paths and per-bundle matching
          // thresholds are resolved here, on the GUI thread, because all three need
          // the catalogue and the atlas cache.
          jobs.clear();
          {
            const vector<std::string> names = selected_bundle_names();
            const AutotrackParams shared = collect_autotrack_params();
            for (const std::string& name : names) {
              const BundleEntry* entry = nullptr;
              for (const auto& candidate : catalogue)
                if (candidate.name == name) { entry = &candidate; break; }
              if (!entry)
                continue;
              BundleJob job;
              job.name = name;
              try {
                job.bundle_path = bundle_path_for (entry->data);
              } catch (Exception& e) {
                QMessageBox::warning (this, "Generate tracks",
                    qstr ("Could not read bundle \"" + name + "\": " + e[0]));
                return;
              }
              if (job.bundle_path.empty())
                continue;
              job.params = shared;
              // A batch must use each bundle's own threshold, not whatever the panel
              // was showing: the suggestion is per bundle, and so are any edits.
              const auto saved = bundle_overrides.find (job.bundle_path);
              if (saved != bundle_overrides.end()) {
                if (std::isfinite (saved->second.distance)) job.params.match.max_mdf = saved->second.distance;
                if (std::isfinite (saved->second.qb))       job.params.match.qb_threshold = saved->second.qb;
                if (std::isfinite (saved->second.dilate))   job.params.dilate_mm = saved->second.dilate;
              } else if (names.size() > 1) {
                // Only worth measuring for a batch; for a single bundle the panel
                // already shows the suggestion and shared carries it.
                try {
                  const auto bundle_tracks = read_bundle (job.bundle_path);
                  const auto scale = MR::DWI::Tractography::Recognition::suggest_bundle_thresholds (
                      bundle_tracks, metric_combo->currentIndex());
                  job.params.match.max_mdf = scale.distance
                                           * algorithm_distance_factor (job.params.algorithm);
                  job.params.match.qb_threshold = scale.qb_radius;
                } catch (Exception&) {
                  // Keep the shared value; a bundle that cannot be measured is not a
                  // reason to abandon the batch.
                }
              }
              job.params.refine = collect_refine_options();
              attach_population_map (job.params, name);
              if (job.params.refine.competitive) {
                // Reading a dozen or more neighbouring bundles takes seconds the
                // first time; say what is happening rather than appearing to hang.
                status_label->setText (tr ("reading bundles that neighbour %1...").arg (qstr (name)));
                QApplication::setOverrideCursor (Qt::WaitCursor);
                QApplication::processEvents();
                job.params.competitors = collect_competitors (name);
                QApplication::restoreOverrideCursor();
              }
              jobs.push_back (std::move (job));
            }
          }
          const bool autotrack = jobs.size();

          // Materialise every region NOW, on the GUI thread: the ROI editor keeps
          // its mask only in a GL texture, so this cannot happen in the worker.
          // In auto-track mode the regions come from the bundle instead.
          bool have_seed = false;
          for (const auto& a : autotrack ? vector<Assignment>() : assignments) {
            RegionProvider* provider = provider_for (a.region);
            if (!provider) {
              QMessageBox::warning (this, "Generate tracks",
                  qstr ("The tool providing region \"" + a.region.name + "\" is no longer open."));
              return;
            }
            try {
              request->regions.push_back ({ a.role, { a.region.label(), provider->get_region_mask (a.region) } });
            } catch (Exception& e) {
              QMessageBox::warning (this, "Generate tracks",
                  qstr ("Could not use region \"" + a.region.name + "\": " + e[0]));
              return;
            }
            if (a.role == SEED)
              have_seed = true;
          }
          if (!autotrack && !have_seed) {
            QMessageBox::warning (this, "Generate tracks",
                "No seed region.\n\nTick an atlas bundle to reconstruct, or add at least one "
                "region and set its role to \"seed\".");
            return;
          }

          // Tracking is the expensive half. If a single bundle is being re-run and
          // the only thing that changed is how candidates are matched, the candidates
          // already in hand can simply be re-filtered - seconds instead of minutes.
          if (jobs.size() == 1) {
            LastRun* cached = last_run_for (jobs[0].name);
            if (cached && cached->valid()
                && cached->source_path == request->source_path
                && cached->bundle_path == jobs[0].bundle_path
                && same_tracking_setup (cached->params, jobs[0].params)) {
              QMessageBox box (this);
              box.setWindowTitle ("Generate tracks");
              box.setText (qstr ("Only the shape-matching settings have changed since the last run."));
              box.setInformativeText (qstr (
                  "The " + str (cached->candidates.size()) + " streamlines that run generated are "
                  "still here, so they can be re-matched at the new settings without tracking again."));
              QPushButton* rematch = box.addButton (tr ("Re-match cached"), QMessageBox::AcceptRole);
              QPushButton* retrack = box.addButton (tr ("Track from scratch"), QMessageBox::DestructiveRole);
              box.addButton (QMessageBox::Cancel);
              box.setDefaultButton (rematch);
              box.exec();
              if (box.clickedButton() == rematch) {
                rematch_cached_candidates (jobs[0].name, true);
                return;
              }
              if (box.clickedButton() != retrack)
                return;
            }
          }

          if (worker.joinable())
            worker.join();
          state.reset();
          state.running = true;
          bundle_generated = 0;
          bundle_effective_distance = 0.0f;
          rejected_results.clear();
          show_rejected_button->setEnabled (false);
          result_error.clear();
          jobs_started = 0;
          jobs_done = 0;
          batch_listed = 0;
          batch_streamlines = 0;
          {
            std::lock_guard<std::mutex> lock (finished_mutex);
            finished.clear();
          }
          state.target_selected = std::isfinite (select_button->value()) ? uint64_t (select_button->value()) : 0;
          preview_tractogram = nullptr;
          preview_count = 0;
          preview_clock.start();
          run_clock.start();
          results.clear();

          set_controls_enabled (false);
          // With a streamline target the bar measures real progress; without one
          // (a blank "select" field) there is nothing to measure, so run it as a
          // busy indicator rather than leaving it parked at zero.
          if (state.target_selected) {
            progress_bar->setRange (0, 100);
            progress_bar->setValue (0);
            progress_bar->setFormat (tr ("starting..."));
          } else {
            // No streamline target to measure against: run it as a busy indicator.
            progress_bar->setRange (0, 0);
            progress_bar->setFormat (tr ("tracking..."));
          }
          status_label->setText (tr ("starting..."));
          poll_timer->start (200);

          pending_name = autotrack ? jobs[0].name
                                   : name_for_run (++tract_iteration, request->scalars);

          // Directional colouring is the most informative, so it is the default for
          // every run, autotrack included. The single exception is a name collision:
          // two tracts of the same name are indistinguishable when both are
          // directional, so the newcomer gets a distinct solid colour. Decide it now,
          // before the preview is listed under this same name. Only listed tracts
          // count - atlas bundles are rendered but never listed, so showing one
          // cannot change how a generated tract is coloured.
          {
            Tractography* tractography = get_tool<Tractography> (false);
            pending_solid_colour = tractography && tractography->has_tractogram_named (pending_name);
          }

          // mrview swaps ProgressBar's display/done hooks for Qt widgets
          // (gui/dialog/dialog.cpp), and the engine creates ProgressBars while
          // opening its source image. Those must not run off the GUI thread, so
          // silence them for the duration of the run and restore afterwards.
          saved_progress_display = MR::ProgressBar::display_func;
          saved_progress_done = MR::ProgressBar::done_func;
          MR::ProgressBar::display_func = [] (const MR::ProgressBar&) { };
          MR::ProgressBar::done_func = [] (const MR::ProgressBar&) { };

          // The same applies, more sharply, to errors and warnings: mrview routes
          // Exception::display and WARN/INFO to Qt dialogs, and building an
          // NSWindow off the main thread aborts the process outright on macOS.
          // (An FOD has no gradient table, so selecting a tensor algorithm used to
          // kill mrview here rather than report the error.) Collect them instead
          // and show them from generation_finished(), on the GUI thread.
          {
            std::lock_guard<std::mutex> lock (worker_message_mutex);
            worker_messages.clear();
          }
          saved_exception_display = MR::Exception::display_func;
          saved_report_to_user = MR::report_to_user_func;
          MR::Exception::display_func = [] (const MR::Exception& E, int) {
            std::lock_guard<std::mutex> lock (worker_message_mutex);
            for (size_t n = 0; n != E.description.size(); ++n)
              worker_messages.push_back (E.description[n]);
          };
          MR::report_to_user_func = [] (const std::string& msg, int type) {
            if (type > 1)   // INFO and DEBUG are not worth surfacing in the GUI
              return;
            std::lock_guard<std::mutex> lock (worker_message_mutex);
            worker_messages.push_back (msg);
          };

          // Record what this run is about to do, per bundle. Candidates are filled
          // in only as each finishes, so a failed or cancelled bundle leaves nothing
          // to re-match.
          for (const auto& job : jobs) {
            LastRun& record = last_runs[job.name];
            record.candidates.clear();
            record.source_path = request->source_path;
            record.bundle_path = job.bundle_path;
            record.bundle_data = job.name;
            record.params = job.params;
          }

          const vector<BundleJob> batch = jobs;
          worker = std::thread ([this, request, batch] () {
            // === pure CPU compute: no GL, no Qt GUI ===
            try {
              if (batch.size()) {
                MR::Header grid = MR::Header::open (request->source_path);
                grid.ndim() = 3;
                for (size_t i = 0; i != batch.size(); ++i) {
                  if (state.cancel)
                    break;
                  const BundleJob& job = batch[i];
                  jobs_started = i + 1;
                  // Each bundle is a run of its own as far as the progress state is
                  // concerned; without this the second bundle's bar would start from
                  // the first one's totals.
                  state.seeds = 0;
                  state.streamlines = 0;
                  state.selected = 0;

                  FinishedBundle out;
                  out.name = job.name;
                  out.params = job.params;
                  AutotrackResult r;
                  try {
                    // Tracked into `results` so the live preview shows this bundle's
                    // candidates arriving; autotrack_bundle then prunes it to the
                    // streamlines that matched.
                    autotrack_bundle (job.bundle_path, request->source_path, grid,
                                      job.params, r, state, &results, &rejected_results);
                    {
                      std::lock_guard<std::mutex> lock (state.results_mutex);
                      out.tracks = results;
                    }
                    out.rejected = rejected_results;
                    out.candidates = out.tracks;
                    out.candidates.insert (out.candidates.end(),
                                           out.rejected.begin(), out.rejected.end());
                    out.generated = r.generated;
                    out.effective_distance = r.effective_distance;
                    out.report = r.refine_report;
                    for (const auto& kv : r.effective_properties)
                      out.properties[kv.first] = kv.second;
                    out.properties["autotrack_bundle"] = job.name;
                    out.properties["autotrack_generated"] = str (r.generated);
                    out.properties["autotrack_mdf"] = str (job.params.match.max_mdf);
                    out.properties["autotrack_mdf_applied"] = str (r.effective_distance);
                    if (job.params.competitors.size()) {
                      std::string names;
                      for (const auto& competitor : job.params.competitors)
                        names += (names.size() ? "," : "") + competitor.name;
                      out.properties["autotrack_competitors"] = names;
                    }
                  } catch (Exception& e) {
                    // One bundle failing must not abandon the rest of the batch -
                    // that is the whole reason for running them in one go.
                    out.error = e[0];
                    for (size_t n = 1; n != e.num(); ++n)
                      out.error += "\n" + e[n];
                  }
                  {
                    std::lock_guard<std::mutex> lock (finished_mutex);
                    finished.push_back (std::move (out));
                  }
                  jobs_done = i + 1;
                }
                QMetaObject::invokeMethod (this, "generation_finished", Qt::QueuedConnection);
                return;
              }

              Properties properties;
              for (const auto& kv : request->scalars)
                properties[kv.first] = kv.second;

              for (auto& entry : request->regions) {
                const int role = entry.first;
                const std::string& name = entry.second.first;
                MR::Image<bool>& mask = entry.second.second;
                switch (role) {
                  case SEED:
                    if (request->seeds_per_voxel)
                      properties.seeds.add (new Seeding::Random_per_voxel (mask, name, request->seeds_per_voxel));
                    else
                      properties.seeds.add (new Seeding::SeedMask (mask, name));
                    break;
                  case INCLUDE:         properties.include.add (MR::DWI::Tractography::ROI (mask, name)); break;
                  case ORDERED_INCLUDE: properties.ordered_include.add (MR::DWI::Tractography::ROI (mask, name)); break;
                  case EXCLUDE:         properties.exclude.add (MR::DWI::Tractography::ROI (mask, name)); break;
                  case MASK:            properties.mask.add (MR::DWI::Tractography::ROI (mask, name)); break;
                }
              }

              run_tracking (request->algorithm, request->source_path, properties,
                            results, state, request->nthreads);

              // Snapshot the provenance while Properties is still alive.
              result_properties.clear();
              static_cast<MR::KeyValues&> (result_properties) = static_cast<const MR::KeyValues&> (properties);
              result_properties.include = properties.include;
              result_properties.exclude = properties.exclude;
              result_properties.mask = properties.mask;
              result_properties.ordered_include = properties.ordered_include;
              for (size_t i = 0; i != properties.seeds.num_seeds(); ++i)
                result_properties.prior_rois.insert ({ "seed", properties.seeds[i]->get_name() });
            } catch (Exception& e) {
              state.error = e[0];
              for (size_t i = 1; i != e.num(); ++i)
                state.error += "\n" + e[i];
            } catch (...) {
              state.error = "unexpected error during track generation";
            }
            QMetaObject::invokeMethod (this, "generation_finished", Qt::QueuedConnection);
          });
        }



        void TrackGen::cancel_slot ()
        {
          state.cancel = true;
          status_label->setText (tr ("cancelling..."));
        }



        void TrackGen::poll_progress_slot ()
        {
          const uint64_t seeds = state.seeds;
          const uint64_t streamlines = state.streamlines;
          const uint64_t selected = state.selected;
          const size_t total_jobs = jobs.size();
          const size_t started = jobs_started;

          // Anything the worker has finished goes into the Tracts list now, so a
          // batch fills in as it runs instead of arriving all at the end.
          drain_finished();

          if (total_jobs > 1 && started) {
            status_label->setText (QString ("%1 (%2 of %3): %4 seeds, %5 streamlines, %6 selected")
                .arg (qstr (jobs[std::min (started, total_jobs) - 1].name))
                .arg (uint64_t (started)).arg (uint64_t (total_jobs))
                .arg (seeds).arg (streamlines).arg (selected));
          } else {
            status_label->setText (QString ("%1 seeds, %2 streamlines, %3 selected")
                .arg (seeds).arg (streamlines).arg (selected));
          }

          if (state.target_selected) {
            // For a batch the bar spans the whole queue: bundles finished, plus how
            // far into the current one we are.
            const double within = std::min (1.0, double (selected) / double (state.target_selected));
            const double overall = total_jobs > 1
                                 ? (double (jobs_done) + within) / double (total_jobs)
                                 : within;
            progress_bar->setRange (0, 100);
            progress_bar->setValue (int (100.0 * overall));
            if (total_jobs > 1) {
              progress_bar->setFormat (QString ("bundle %1 of %2 - %p%  (%3 s)")
                  .arg (uint64_t (std::min (started, total_jobs))).arg (uint64_t (total_jobs))
                  .arg (run_clock.elapsed(), 0, 'f', 0));
            } else {
              progress_bar->setFormat (QString ("%1 of %2 streamlines - %p%  (%3 s)")
                  .arg (selected).arg (uint64_t (state.target_selected))
                  .arg (run_clock.elapsed(), 0, 'f', 0));
            }
          } else {
            // Nothing to measure against, so keep it animating rather than static:
            // a bar parked at one value reads as a hang.
            progress_bar->setRange (0, 0);
            progress_bar->setFormat (QString ("%1 streamlines  (%2 s)")
                .arg (selected).arg (run_clock.elapsed(), 0, 'f', 0));
          }

          update_preview (selected);
        }



        void TrackGen::drain_finished ()
        {
          vector<FinishedBundle> ready;
          {
            std::lock_guard<std::mutex> lock (finished_mutex);
            if (finished.empty())
              return;
            ready.swap (finished);
          }

          Tractography* tractography = get_tool<Tractography>();
          for (FinishedBundle& bundle : ready) {
            if (bundle.error.size()) {
              // Reported, not raised: the rest of the batch is still coming, and a
              // modal dialog per bundle would be unusable.
              WARN ("bundle \"" + bundle.name + "\": " + bundle.error);
              status_label->setText (QString ("%1 failed: %2")
                  .arg (qstr (bundle.name)).arg (qstr (bundle.error)));
              continue;
            }

            // Keep the candidates so a change of matching settings need not re-track.
            LastRun& record = last_runs[bundle.name];
            record.candidates = std::move (bundle.candidates);
            record.name = bundle.name;
            record.params = bundle.params;

            if (bundle.tracks.empty()) {
              status_label->setText (QString ("%1: %2").arg (qstr (bundle.name))
                  .arg (qstr (bundle.report.summary())));
              continue;
            }
            if (!tractography)
              continue;

            MR::DWI::Tractography::Properties props;
            for (const auto& kv : bundle.properties)
              props[kv.first] = kv.second;

            // A name already in the list means two tracts that would be
            // indistinguishable if both were coloured by direction.
            const bool solid = tractography->has_tractogram_named (bundle.name);
            Tractogram* listed = nullptr;
            bool adopted_preview = false;
            // The live preview carries this bundle's streamlines already; finish it
            // rather than listing a second copy of the same run.
            if (preview_tractogram && tractography->contains (preview_tractogram)
                && preview_tractogram->display_name() == bundle.name) {
              preview_tractogram->reload_from_memory (bundle.tracks, bundle.generated);
              listed = preview_tractogram;
              adopted_preview = true;
              preview_tractogram = nullptr;
              preview_count = 0;
            } else {
              listed = tractography->add_tractogram_from_memory (
                  bundle.tracks, props, bundle.name, bundle.generated, solid);
            }
            if (listed) {
              // After reload_from_memory / add, both of which clear them.
              listed->set_rejected_tracks (bundle.rejected);
              // What this tract was recognised as, so the Tracts list can re-derive
              // it at another strictness without tracking again. Only marked valid
              // when the atlas can still supply the bundle to measure against.
              const std::string atlas_name = bundle.name.compare (0, 6, "atlas_") == 0
                                           ? bundle.name.substr (6) : bundle.name;
              if (window().atlas_registration().has_bundle (atlas_name)) {
                Tractogram::Refinement& provenance = listed->refinement();
                provenance.bundle = atlas_name;
                provenance.match = bundle.params.match;
                provenance.options = bundle.params.refine;
                provenance.original = bundle.params.refine;
                provenance.neighbours.clear();   // automatic, per bundle
                provenance.kept = bundle.tracks.size();
                provenance.candidates = bundle.tracks.size() + bundle.rejected.size();
                provenance.valid = true;
              }
              // add_tractogram_from_memory was told to do this itself; a preview was
              // listed before the collision was known, so it has to be done here.
              if (solid && adopted_preview)
                tractography->apply_distinct_colour (listed);
            }
            record.solid_colour = solid;
            rejected_results = bundle.rejected;
            bundle_generated = bundle.generated;
            bundle_effective_distance = bundle.effective_distance;
            ++batch_listed;
            batch_streamlines += bundle.tracks.size();
            status_label->setText (QString ("%1: %2").arg (qstr (bundle.name))
                .arg (qstr (bundle.report.summary())));
          }
          // The tract was listed before its provenance was stamped on it, so the
          // Tracts panel decided there was nothing to refine. Tell it to look again.
          if (tractography)
            tractography->refresh_tract_controls();
          window().updateGL();
        }



        TrackGen::LastRun* TrackGen::last_run_for (const std::string& name)
        {
          const auto it = last_runs.find (name);
          return it == last_runs.end() ? nullptr : &it->second;
        }



        bool TrackGen::rematch_cached_candidates (const std::string& name, bool use_panel_thresholds)
        {
          LastRun* cached = last_run_for (name);
          if (!cached || !cached->valid())
            return false;

          MR::Timer clock;
          AutotrackParams params = cached->params;
          // The panel's distance belongs to whichever bundle it last sized itself
          // for, which in a batch is not this one. So it is only read when the
          // caller is the Generate button - where the panel and the bundle are the
          // same thing. The Clean up controls change their own settings and leave
          // the bundle's own threshold alone.
          if (use_panel_thresholds) {
            if (std::isfinite (mdf_button->value()))
              params.match.max_mdf = mdf_button->value();
            if (std::isfinite (qb_button->value()))
              params.match.qb_threshold = qb_button->value();
          }
          params.refine = collect_refine_options();
          attach_population_map (params, name);
          if (params.refine.competitive)
            params.competitors = collect_competitors (name);
          else
            params.competitors.clear();

          vector<Streamline<float>> kept, rejected;
          float effective = params.match.max_mdf;
          Recognition::RefineReport report;
          try {
            QApplication::setOverrideCursor (Qt::WaitCursor);
            const vector<Streamline<float>> atlas = read_bundle (cached->bundle_path);
            autotrack_match (atlas, params, cached->candidates, kept, rejected, effective, &report);
            QApplication::restoreOverrideCursor();
          } catch (Exception& e) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning (this, "Track generation", qstr (e[0]));
            return false;
          }

          const double elapsed = clock.elapsed();
          bundle_generated = cached->candidates.size();
          bundle_effective_distance = effective;
          rejected_results = std::move (rejected);
          show_rejected_button->setEnabled (rejected_results.size());
          cached->params = params;
          pending_name = cached->name.size() ? cached->name : name;

          const QString detail = QString ("%1 (%2 s, no re-tracking)")
              .arg (qstr (report.summary())).arg (elapsed, 0, 'f', 2);
          if (kept.empty()) {
            status_label->setText (detail);
            return true;
          }

          try {
            Tractography* tractography = get_tool<Tractography>();
            if (!tractography)
              throw Exception ("could not open the Tractography tool");
            MR::DWI::Tractography::Properties props;
            for (const auto& kv : result_properties)
              props[kv.first] = kv.second;
            props["autotrack_mdf"] = str (params.match.max_mdf);
            props["autotrack_mdf_applied"] = str (effective);
            props["autotrack_rematched"] = "1";
            // Update the tract in place when it is still listed, rather than adding
            // a second copy: this runs on every drag of the strictness slider.
            Tractogram* existing = tractography->find_tractogram_named (pending_name);
            if (existing) {
              existing->reload_from_memory (kept, bundle_generated);
              existing->set_rejected_tracks (rejected_results);
              window().updateGL();
            } else {
              const bool solid = tractography->has_tractogram_named (pending_name);
              if (Tractogram* added = tractography->add_tractogram_from_memory (
                      kept, props, pending_name, bundle_generated, solid))
                added->set_rejected_tracks (rejected_results);
            }
            status_label->setText (detail);
          } catch (Exception& e) {
            QMessageBox::warning (this, "Track generation", qstr (e[0]));
            return false;
          }
          return true;
        }



        void TrackGen::show_rejected_slot ()
        {
          if (rejected_results.empty())
            return;
          try {
            Tractography* tractography = get_tool<Tractography>();
            if (!tractography)
              throw Exception ("could not open the Tractography tool");
            MR::DWI::Tractography::Properties props;
            for (const auto& kv : result_properties)
              props[kv.first] = kv.second;
            props["source"] = "track generation (rejected by the shape metric)";
            props["autotrack_rejected"] = str (rejected_results.size());
            // A distinct solid colour: this is meant to be compared against the
            // tract it was rejected from, which is directionally coloured.
            tractography->add_tractogram_from_memory (rejected_results, props,
                pending_name + "_deleted", rejected_results.size(), true);
            status_label->setText (QString ("%1 rejected streamlines listed as \"%2_deleted\"")
                .arg (uint64_t (rejected_results.size())).arg (qstr (pending_name)));
          } catch (Exception& e) {
            QMessageBox::warning (this, "Track generation", qstr (e[0]));
          }
        }



        bool TrackGen::guided () const
        {
          return mode_combo->currentIndex() == 0;
        }



        void TrackGen::apply_mode ()
        {
          const bool is_guided = guided();
          for (QWidget* w : expert_only)
            w->setVisible (!is_guided);
          for (QWidget* w : guided_only)
            w->setVisible (is_guided);
          // Manual ROI tracking is an expert flow, and it is also the only thing the
          // region table is for; in Guided it would offer a way to start a run that
          // Guided cannot then configure.
          if (is_guided)
            region_box->setVisible (false);
        }



        void TrackGen::apply_guided_defaults ()
        {
          if (!guided())
            return;
          // A clinician cannot be asked to pick a tracking algorithm, and the
          // deliberate "(select an algorithm)" placeholder would stop every run with
          // a dialog telling them to set something Guided does not show. iFOD2 is
          // what tckgen itself defaults to.
          //
          // Called from the constructor as well as from the mode switch: relying on
          // mode_changed_slot alone left this undone at startup, because the
          // constructor sets the mode *before* connecting the signal, so the slot
          // never ran and a first Guided run always stopped on the placeholder.
          if (current_algorithm() < 0) {
            const int idx = algorithm_combo->findText ("iFOD2");
            algorithm_combo->setCurrentIndex (idx >= 0 ? idx : 1);
          }
          quality_changed_slot (quality_combo->currentIndex());
        }



        void TrackGen::mode_changed_slot (int)
        {
          apply_guided_defaults();
          update_mode();
        }



        void TrackGen::quality_changed_slot (int index)
        {
          // Presets write into the Expert widgets rather than into a parallel set of
          // values, so a run has one source of truth whichever mode set it up, and
          // switching to Expert shows exactly what Guided asked for.
          switch (index) {
            case 0:   // Fast: enough to see whether a bundle reconstructs at all
              select_button->setValue (500.0f);
              max_seeds_factor = 20;
              break;
            case 2:   // Thorough: the reconstruction you keep
              select_button->setValue (10000.0f);
              max_seeds_factor = 80;
              break;
            default:  // Balanced
              select_button->setValue (2000.0f);
              max_seeds_factor = 40;
              break;
          }
        }



        Recognition::RefineOptions TrackGen::collect_refine_options () const
        {
          Recognition::RefineOptions options;
          options.competitive = competitive_box->isChecked();
          // MEASURED AND LEFT OFF. The point-level test in CompetitorSet::intrusion()
          // exists because competition is a decision about a whole streamline while
          // overlap is a property of points, and with four competitors that gap is
          // real: CST_L bent through the thalamus is kept 399 of 400 by competition
          // and 0 of 400 by the territory test. But with the competitor set this tool
          // actually uses - the whole atlas category - competition already rejects
          // all 400, and it does so for every variant built to defeat it, including
          // excursions short enough that a whole-streamline average should not see
          // them (a 16-vertex excursion: 0 of 400 either way; an 8-vertex one: 134 of
          // 400 either way). Enabling it changed no measurement taken, at any
          // threshold, so it does not ship on. Reachable through tckrefine -territory,
          // because the day a bundle has no category to compete against is the day it
          // is the only thing left.
          options.exclusive_territory = false;
          // The atlas bundle's own length range and paired endpoints: both measured
          // from the bundle, so there is nothing here for the user to set.
          options.length_window = true;
          options.endpoint_gate = true;
          // Measured on held-out atlas halves at a 15 mm threshold, the paired gate
          // costs almost nothing and removes a lot: at 8 mm, CST keeps 100% of its
          // own and ML's leakage falls from 79% to 53%; at 6 mm, 99.75% and 25%; at
          // 4 mm, 99.75% and 6.5%. 8 mm is the choice because those figures are
          // atlas-against-atlas, and a subject's streamlines sit further from the
          // registered atlas than the atlas does from itself - and because
          // competitive assignment removes the remaining leakage anyway, leaving
          // this gate only the doubled-back streamlines to catch. Losing genuine
          // members to a tight radius would be the worse error.
          options.endpoint_radius = std::max (8.0f, endpoint_dilate_button->value());
          const int strictness = strictness_spin->value();
          // 0 means "no share-based limit"; the millimetre threshold then decides,
          // which is what a run that has never touched this should do.
          if (strictness > 0)
            options.keep_fraction = 1.0f - float (strictness) / 100.0f;
          // Per-node scoring, in standard deviations from the bundle core at each
          // point along the streamline. Measured against the whole-streamline
          // median+MAD rule on held-out atlas halves: at matched leakage on AF_L,
          // per-node keeps 99.25% of the bundle's own streamlines against MAD's
          // 94.75%, and it is no worse on CST_L. The thresholds are where the tail
          // comes off without eating the bundle - 2.0 costs 5-8% of genuine
          // streamlines, which is too much to offer as "high".
          options.per_node_outliers = true;
          switch (prune_combo->currentIndex()) {
            case 1:  options.outlier_k = 4.0f; break;   // low
            case 2:  options.outlier_k = 3.0f; break;   // medium
            case 3:  options.outlier_k = 2.5f; break;   // high
            default: break;
          }
          return options;
        }



        void TrackGen::ensure_neighbour_index ()
        {
          if (AtlasTemplate::footprints_ready() || footprint_running)
            return;
          if (AtlasTemplate::load_footprint_cache())
            return;
          // Reading every bundle takes tens of seconds on a directory atlas, so it
          // happens once, in the background, and competition falls back to the other
          // bundles in this run until it lands.
          if (footprint_worker.joinable())
            footprint_worker.join();
          footprint_running = true;
          footprint_worker = std::thread ([this] () {
            try {
              AtlasTemplate::build_footprints();
            } catch (...) { }
            footprint_running = false;
          });
        }



        void TrackGen::attach_population_map (AutotrackParams& params, const std::string& name)
        {
          params.population_map.clear();
          params.population_to_map = transform_type::Identity();

          const std::string atlas_name = name.compare (0, 6, "atlas_") == 0 ? name.substr (6) : name;
          const std::string path = AtlasTemplate::population_map_path (atlas_name);
          if (path.empty())
            return;

          params.population_map = path;
          // The map is in atlas space and the candidates will be in the subject's, so
          // sampling means going back the way the atlas came. Inverting the fit is
          // exact and free; resampling the map into subject space would be neither.
          params.population_to_map = window().atlas_registration().fit().mni_to_subject.inverse();
        }



        vector<Recognition::Competitor> TrackGen::collect_competitors (const std::string& name)
        {
          vector<Recognition::Competitor> out;
          ensure_neighbour_index();

          // Catalogue names carry the "atlas_" prefix this tool adds; the index is
          // keyed by the atlas's own names.
          auto atlas_name_of = [] (const std::string& shown) {
            return shown.compare (0, 6, "atlas_") == 0 ? shown.substr (6) : shown;
          };

          // Per bundle, always: this is called once per job, so each bundle of a
          // batch gets its own competitors. Overriding by hand is a decision about
          // one finished tract, and lives with that tract in the Tracts list.
          vector<std::string> wanted = AtlasTemplate::competitors_for (atlas_name_of (name));
          // Being in the atlas is not the same as being catalogued under a category,
          // and a .tck the user loaded by hand is in no atlas at all - so it has
          // neither siblings nor neighbours and the lookup comes back empty. Falling
          // back on emptiness covers that, and covers the first run, before the
          // footprint index has finished building.
          if (wanted.empty()) {
            // The other bundles of this run: weaker than the index, but the user has
            // just said these are the tracts they care about telling apart.
            for (const std::string& other : selected_bundle_names())
              if (other != name)
                wanted.push_back (atlas_name_of (other));
          }

          auto& registration = window().atlas_registration();
          for (const std::string& atlas_name : wanted) {
            if (atlas_name == atlas_name_of (name))
              continue;
            try {
              if (registration.has_bundle (atlas_name)) {
                // Copied, not referenced: the cache is the GUI thread's and evicts
                // its least recently used bundle when it fills.
                const auto& bundle = registration.bundle (atlas_name);
                if (bundle.size())
                  out.push_back ({ atlas_name, bundle });
                continue;
              }
              // A user-loaded .tck, which the atlas registration knows nothing about.
              for (const auto& entry : catalogue) {
                if (atlas_name_of (entry.name) != atlas_name)
                  continue;
                const std::string path = bundle_path_for (entry.data);
                if (path.size()) {
                  auto bundle = read_bundle (path);
                  if (bundle.size())
                    out.push_back ({ atlas_name, std::move (bundle) });
                }
                break;
              }
            } catch (Exception& e) {
              DEBUG ("competitor \"" + atlas_name + "\" unavailable: " + e[0]);
            }
          }
          return out;
        }



        void TrackGen::recognition_changed_slot ()
        {
          update_neighbours_label();
        }



        void TrackGen::update_neighbours_label ()
        {

          if (!competitive_box->isChecked()) {
            neighbours_label->setText (tr ("no bundles will compete"));
            return;
          }
          const vector<std::string> names = selected_bundle_names();
          if (names.empty()) {
            neighbours_label->setText (tr ("competing bundles: chosen per bundle, once one is ticked"));
            return;
          }
          // Per bundle, and said so per bundle: one number for the batch would hide
          // that CST_L and ML_L compete against different sets.
          auto atlas_name_of = [] (const std::string& shown) {
            return shown.compare (0, 6, "atlas_") == 0 ? shown.substr (6) : shown;
          };
          QString detail;
          size_t answered = 0;
          for (size_t i = 0; i != names.size() && i != 4; ++i) {
            const size_t count = AtlasTemplate::competitors_for (atlas_name_of (names[i])).size();
            if (count)
              ++answered;
            detail += (detail.size() ? ", " : "") + QString ("%1 for %2")
                .arg (uint64_t (count)).arg (qstr (names[i]));
          }
          if (names.size() > 4)
            detail += tr (", ...");


          if (!answered) {
            // Nothing catalogued under a category and no index yet: the run's own
            // other bundles are what collect_competitors() will fall back on.
            neighbours_label->setText (tr ("competing bundles: the other %1 ticked, until the atlas "
                                          "index finishes building")
                .arg (uint64_t (names.size() > 1 ? names.size() - 1 : 0)));
            return;
          }
          neighbours_label->setText (tr ("competing bundles: %1").arg (detail));
        }



        void TrackGen::discard_preview ()
        {
          if (!preview_tractogram)
            return;
          if (Tractography* tractography = get_tool<Tractography> (false)) {
            if (tractography->contains (preview_tractogram))
              tractography->remove_tractogram (preview_tractogram);
          }
          preview_tractogram = nullptr;
          preview_count = 0;
          window().updateGL();
        }



        void TrackGen::update_preview (uint64_t selected)
        {
          if (!preview_box->isChecked() || !selected || selected == preview_count)
            return;
          // The name of the bundle being tracked *now*, not the batch's first: the
          // preview is adopted as that bundle's tract when it finishes, so a stale
          // name would attach one bundle's streamlines to another's tract.
          const size_t started = jobs_started;
          const std::string preview_name = (jobs.size() && started)
                                         ? jobs[std::min (started, jobs.size()) - 1].name
                                         : pending_name;
          if (preview_tractogram && preview_tractogram->display_name() != preview_name) {
            // A new bundle has started; the previous preview has already been taken
            // over by drain_finished(), or belonged to a bundle that matched nothing.
            preview_tractogram = nullptr;
            preview_count = 0;
          }
          // Re-uploading grows with the streamline count, so refresh at a fixed
          // interval rather than on every poll: on a long run the preview would
          // otherwise take a larger and larger share of the GUI thread.
          if (preview_clock.elapsed() < 1.0)
            return;

          vector<MR::DWI::Tractography::Streamline<float>> snapshot;
          {
            std::lock_guard<std::mutex> lock (state.results_mutex);
            if (results.empty())
              return;
            snapshot = results;
          }

          try {
            Tractography* tractography = get_tool<Tractography>();
            if (!tractography)
              return;
            if (!preview_tractogram || !tractography->contains (preview_tractogram)) {
              // First refresh, or the user closed the preview mid-run.
              // Properties is non-copyable (Seeding::List deletes its copy
              // constructor), so build the preview's from the key/value pairs.
              MR::DWI::Tractography::Properties props;
              for (const auto& kv : result_properties)
                props[kv.first] = kv.second;
              props["source"] = "track generation (in progress)";
              preview_tractogram = tractography->add_tractogram_from_memory (
                  snapshot, props, preview_name, snapshot.size(), false);
            } else {
              preview_tractogram->reload_from_memory (snapshot, snapshot.size());
            }
            preview_count = selected;
            preview_clock.start();
            window().updateGL();
          } catch (Exception& e) {
            e.display();
            preview_tractogram = nullptr;
          }
        }



        void TrackGen::generation_finished ()
        {
          const double elapsed = run_clock.elapsed();
          poll_timer->stop();
          if (worker.joinable())
            worker.join();
          state.running = false;

          restore_progress_hooks();

          // Whatever the worker finished after the last poll.
          drain_finished();

          {
            std::lock_guard<std::mutex> lock (worker_message_mutex);
            if (worker_messages.size() && state.error.empty() && result_error.empty()) {
              // Not fatal, or the error paths below would have it; report the first
              // line, which is the one that says what went wrong.
              status_label->setText (qstr (worker_messages.front()));
            }
            worker_messages.clear();
          }

          progress_bar->setRange (0, 100);
          progress_bar->setValue (0);
          progress_bar->setFormat (tr ("idle"));
          set_controls_enabled (true);

          if (state.error.size() || result_error.size()) {
            if (result_error.size() && state.error.empty())
              state.error = result_error;
            result_error.clear();
            status_label->setText (tr ("failed"));
            discard_preview();
            QMessageBox::critical (this, "Generate tracks", qstr (state.error));
            results.clear();
            return;
          }

          // --- a batch of bundles ---
          if (jobs.size()) {
            // Any preview left over belongs to a bundle that matched nothing, or to
            // a cancelled one; either way it is not a result.
            discard_preview();
            const size_t attempted = jobs_done;
            if (!batch_listed) {
              status_label->setText (QString ("no streamlines matched any of the %1 bundle(s) (%2 s)")
                  .arg (uint64_t (attempted)).arg (elapsed, 0, 'f', 1));
            } else if (jobs.size() == 1) {
              // One bundle: keep the per-bundle wording, which says more.
              status_label->setText (QString ("%1: %2 streamlines, in %3 s")
                  .arg (qstr (jobs[0].name)).arg (uint64_t (batch_streamlines))
                  .arg (elapsed, 0, 'f', 1));
            } else {
              status_label->setText (QString ("%1 of %2 bundles reconstructed, %3 streamlines in all, in %4 s")
                  .arg (uint64_t (batch_listed)).arg (uint64_t (jobs.size()))
                  .arg (uint64_t (batch_streamlines)).arg (elapsed, 0, 'f', 1));
            }
            if (state.cancel && attempted < jobs.size()) {
              status_label->setText (status_label->text() + tr (" (cancelled)"));
            }
            show_rejected_button->setEnabled (rejected_results.size());
            results.clear();
            return;
          }

          // --- manual region tracking ---
          if (results.empty()) {
            discard_preview();
            status_label->setText (tr ("no streamlines were selected"));
            return;
          }

          try {
            Tractography* tractography = get_tool<Tractography>();
            if (!tractography)
              throw Exception ("could not open the Tractography tool");
            // Decided in generate_slot(), before the preview took this name.
            const bool solid_colour = pending_solid_colour;
            if (preview_tractogram && tractography->contains (preview_tractogram)) {
              // The preview already carries these streamlines; finish it rather than
              // adding a second copy of the same run.
              preview_tractogram->reload_from_memory (results, state.streamlines);
              if (solid_colour)
                tractography->apply_distinct_colour (preview_tractogram);
              window().updateGL();
            } else {
              tractography->add_tractogram_from_memory (
                  results, result_properties, pending_name, state.streamlines, solid_colour);
            }
            preview_tractogram = nullptr;
            preview_count = 0;
            status_label->setText (QString ("%1 streamlines added to the Tractography tool, in %2 s")
                .arg (uint64_t (results.size())).arg (elapsed, 0, 'f', 1));
          } catch (Exception& e) {
            e.display();
            status_label->setText (tr ("failed to display the generated tracks"));
          }
          results.clear();
        }



        bool TrackGen::add_region_by_name (const std::string& name, int role)
        {
          vector<RegionRef> available;
          collect_regions (available);
          for (const auto& region : available) {
            if (region.name != name && region.label() != name)
              continue;
            Assignment a;
            a.region = region;
            a.role = role;
            assignments.push_back (a);
            rebuild_region_table();
            return true;
          }
          WARN ("no region named \"" + name + "\" is available; is the owning tool open?");
          return false;
        }



        void TrackGen::run_and_save (const std::string& path)
        {
          // Synchronous sibling of generate_slot(), for command-line use: no
          // worker thread, and the result goes straight to a .tck. Everything
          // else (region materialisation, Properties assembly, the engine call)
          // is the same code path the GUI uses.
          if (!source_combo->count())
            throw Exception ("no FOD image available for track generation");

          const vector<std::string> names = selected_bundle_names();
          if (names.size()) {
            // One file per bundle. With a single bundle the name given is used as
            // it stands; with several, the bundle name is inserted before the
            // extension, because one path cannot hold several bundles.
            const bool many = names.size() > 1;
            std::string stem = path, extension = ".tck";
            if (many) {
              const size_t dot = path.find_last_of ('.');
              if (dot != std::string::npos && dot > path.find_last_of ('/') + 1) {
                stem = path.substr (0, dot);
                extension = path.substr (dot);
              }
            }
            const std::string source = source_combo->currentData().toString().toStdString();
            MR::Header grid = MR::Header::open (source);
            grid.ndim() = 3;

            for (const std::string& name : names) {
              const BundleEntry* entry = nullptr;
              for (const auto& candidate : catalogue)
                if (candidate.name == name) { entry = &candidate; break; }
              if (!entry)
                continue;
              const std::string bundle = bundle_path_for (entry->data);
              if (bundle.empty())
                continue;

              AutotrackParams params = collect_autotrack_params();
              const auto saved = bundle_overrides.find (bundle);
              if (saved != bundle_overrides.end()) {
                if (std::isfinite (saved->second.distance)) params.match.max_mdf = saved->second.distance;
                if (std::isfinite (saved->second.qb))       params.match.qb_threshold = saved->second.qb;
                if (std::isfinite (saved->second.dilate))   params.dilate_mm = saved->second.dilate;
              }
              attach_population_map (params, name);
              if (params.refine.competitive)
                params.competitors = collect_competitors (name);

              AutotrackResult r;
              state.reset();
              autotrack_bundle (bundle, source, grid, params, r, state);
              CONSOLE ("trackgen: " + r.name + ": atlas " + str(r.atlas_streamlines)
                       + ", generated " + str(r.generated) + "; " + r.refine_report.summary()
                       + " at " + str(r.effective_distance) + " mm");
              if (r.tracks.empty())
                continue;

              Properties props;
              for (const auto& kv : r.effective_properties)
                props[kv.first] = kv.second;
              props["autotrack_bundle"] = r.name;
              props["autotrack_generated"] = str (r.generated);
              props["autotrack_mdf"] = str (params.match.max_mdf);
              props["autotrack_mdf_applied"] = str (r.effective_distance);
              if (params.competitors.size()) {
                std::string competitor_names;
                for (const auto& competitor : params.competitors)
                  competitor_names += (competitor_names.size() ? "," : "") + competitor.name;
                props["autotrack_competitors"] = competitor_names;
              }

              const std::string out = many ? stem + "_" + name + extension : path;
              MR::DWI::Tractography::Writer<float> writer (out, props);
              for (const auto& tck : r.tracks)
                writer (tck);
              for (uint64_t k = r.tracks.size(); k < r.attempted; ++k)
                writer.skip();
              INFO ("wrote " + str(r.tracks.size()) + " streamlines to \"" + out + "\"");
              try {
                if (Tractography* tractography = get_tool<Tractography>())
                  tractography->add_tractogram_from_memory (r.tracks, props, name, r.attempted, true);
              } catch (Exception& e) { e.display(); }
            }
            return;
          }

          // No bundle resolved and nothing to seed from: the engine has no seeder
          // to ask, and takes the null one straight into a crash. Say what is
          // missing instead - this is how a mistyped -trackgen.bundle arrives here.
          {
            bool has_seed = false;
            for (const auto& a : assignments)
              if (a.role == SEED) { has_seed = true; break; }
            if (!has_seed)
              throw Exception ("nothing to track from: no atlas bundle was selected "
                               "(check the -trackgen.bundle name, and that the atlas has "
                               "been aligned to the image) and no seed region was given");
          }

          Properties properties;
          auto set_if_valid = [&] (const std::string& key, AdjustButton* button) {
            const float value = button->value();
            if (std::isfinite (value))
              properties[key] = str (value);
          };
          set_if_valid ("max_num_tracks", select_button);
          set_if_valid ("threshold",      cutoff_button);
          set_if_valid ("step_size",      step_button);
          set_if_valid ("max_angle",      angle_button);
          set_if_valid ("min_dist",       minlength_button);
          set_if_valid ("max_dist",       maxlength_button);
          if (rk4_box->isChecked())            properties["rk4"] = "1";
          if (stop_box->isChecked())           properties["stop_on_all_include"] = "1";
          if (unidirectional_box->isChecked()) properties["unidirectional"] = "1";

          const size_t seeds_per_voxel = seeds_per_voxel_button->value();
          for (const auto& a : assignments) {
            RegionProvider* provider = provider_for (a.region);
            if (!provider)
              throw Exception ("the tool providing region \"" + a.region.name + "\" is not open");
            auto mask = provider->get_region_mask (a.region);
            const std::string name = a.region.label();
            switch (a.role) {
              case SEED:
                if (seeds_per_voxel)
                  properties.seeds.add (new Seeding::Random_per_voxel (mask, name, seeds_per_voxel));
                else
                  properties.seeds.add (new Seeding::SeedMask (mask, name));
                break;
              case INCLUDE:         properties.include.add (MR::DWI::Tractography::ROI (mask, name)); break;
              case ORDERED_INCLUDE: properties.ordered_include.add (MR::DWI::Tractography::ROI (mask, name)); break;
              case EXCLUDE:         properties.exclude.add (MR::DWI::Tractography::ROI (mask, name)); break;
              case MASK:            properties.mask.add (MR::DWI::Tractography::ROI (mask, name)); break;
            }
          }

          state.reset();
          results.clear();
          run_tracking (current_algorithm(),
                        source_combo->currentData().toString().toStdString(),
                        properties, results, state, size_t (nthreads_button->value()));

          MR::DWI::Tractography::Writer<float> writer (path, properties);
          for (const auto& tck : results)
            writer (tck);
          // Writer only counts what it is handed; state.streamlines is the number
          // actually attempted, which is what tckgen records as total_count.
          for (uint64_t i = results.size(); i < state.streamlines; ++i)
            writer.skip();
          INFO ("wrote " + str(results.size()) + " streamlines to \"" + path + "\"");

          try {
            if (Tractography* tractography = get_tool<Tractography>())
              tractography->add_tractogram_from_memory (results, properties, Path::basename (path),
                                                      state.streamlines, false);
          } catch (Exception& e) {
            e.display();
          }
          results.clear();
        }



        void TrackGen::add_commandline_options (MR::App::OptionList& options)
        {
          using namespace MR::App;
          options
            + OptionGroup ("Track generation tool options")

            + Option ("trackgen.source", "Set the FOD image used for track generation.")
            +   Argument ("image").type_image_in()

            + Option ("trackgen.algorithm", "Select the tracking algorithm (as per tckgen -algorithm).")
            +   Argument ("name").type_text()

            + Option ("trackgen.select", "Set the desired number of streamlines.")
            +   Argument ("number").type_integer (1)

            + Option ("trackgen.seed", "Use the named region as a seed region.").allow_multiple()
            +   Argument ("region").type_text()

            + Option ("trackgen.include", "Use the named region as an inclusion region.").allow_multiple()
            +   Argument ("region").type_text()

            + Option ("trackgen.exclude", "Use the named region as an exclusion region.").allow_multiple()
            +   Argument ("region").type_text()

            + Option ("trackgen.mask", "Use the named region as a tracking mask.").allow_multiple()
            +   Argument ("region").type_text()

            + Option ("trackgen.atlas", "Load .tck bundles from a tract atlas folder, so they can "
                                        "be selected for auto-tracking.")
            +   Argument ("folder").type_text()

            + Option ("trackgen.bundle", "Select an atlas bundle by name, which switches to "
                                         "auto-tracking mode. May be given more than once; "
                                         "each bundle is reconstructed as its own tract.").allow_multiple()
            +   Argument ("name").type_text()

            + Option ("trackgen.mdf", "Set the bundle shape-matching threshold, in mm.")
            +   Argument ("value").type_float (0.0)

            + Option ("trackgen.dilate", "Set how far the bundle footprint is grown, in mm.")
            +   Argument ("value").type_float (0.0)

            + Option ("trackgen.enddilate", "Set the radius of the bundle endpoint regions, in mm.")
            +   Argument ("value").type_float (0.0)

            + Option ("trackgen.noendpoints", "Do not require streamlines to reach both ends of the "
                                              "atlas bundle (for bundles that terminate diffusely).")

            + Option ("trackgen.nomask", "Do not confine tracking to the bundle territory; let "
                                         "streamlines run their natural length and rely on the "
                                         "shape match alone.")

            + Option ("trackgen.seedspervoxel", "Seed this many streamlines per territory voxel "
                                                "instead of seeding randomly.")
            +   Argument ("number").type_integer (1)

            + Option ("trackgen.qb", "Set the QuickBundles radius used to compress the atlas bundle, in mm.")
            +   Argument ("value").type_float (0.0)

            + Option ("trackgen.metric", "Bundle shape metric: 0 = MDF (equal-length bundles), "
                                         "1 = closest point mean, 2 = closest point 90th "
                                         "percentile, 3 = directed Hausdorff (strictest).")
            +   Argument ("index").type_integer (0, 3)

            + Option ("trackgen.maxseeds", "Bound auto-tracking effort: give up after this many "
                                           "times -trackgen.select seeding attempts (default 10). "
                                           "This is what bounds how long a bundle can take.")
            +   Argument ("factor").type_integer (1)

            + Option ("trackgen.step", "Set the tracking step size, in mm.")
            +   Argument ("value").type_float (0.0)

            + Option ("trackgen.angle", "Set the maximum angle between successive steps, in degrees.")
            +   Argument ("value").type_float (0.0)

            + Option ("trackgen.cutoff", "Set the FOD amplitude cutoff for terminating tracks.")
            +   Argument ("value").type_float (0.0)

            + Option ("trackgen.minlength", "Set the minimum streamline length, in mm.")
            +   Argument ("value").type_float (0.0)

            + Option ("trackgen.maxlength", "Set the maximum streamline length, in mm.")
            +   Argument ("value").type_float (0.0)

            + Option ("trackgen.run", "Generate streamlines now with the configured settings, "
                                      "and write them to the given file.")
            +   Argument ("tracks").type_file_out();
        }



        bool TrackGen::process_commandline_option (const MR::App::ParsedOption& opt)
        {
          if (opt.opt->is ("trackgen.source")) {
            const std::string path (opt[0]);
            source_combo->addItem (qstr (Path::basename (path)), qstr (path));
            source_combo->setCurrentIndex (source_combo->count()-1);
            return true;
          }

          if (opt.opt->is ("trackgen.algorithm")) {
            const std::string name (opt[0]);
            for (size_t i = 0; i != trackgen_num_algorithms(); ++i) {
              if (lowercase (trackgen_algorithms[i]) == lowercase (name)) {
                const int row = algorithm_combo->findData (int (i));
                if (row < 0) {
                  WARN ("tracking algorithm \"" + name + "\" cannot track from an FOD image");
                  return true;
                }
                algorithm_combo->setCurrentIndex (row);
                return true;
              }
            }
            WARN ("unknown tracking algorithm \"" + name + "\"");
            return true;
          }

          if (opt.opt->is ("trackgen.select")) {
            select_button->setValue (float (int (opt[0])));
            return true;
          }

          if (opt.opt->is ("trackgen.atlas")) {
            load_atlas_bundles (list_atlas_bundles (opt[0]));
            // Loading an atlas pre-selects its category; leave the mode to
            // -trackgen.bundle so a plain -trackgen.atlas does not silently
            // switch a region-based setup into auto-tracking.
            category_combo->setCurrentIndex (0);
            return true;
          }

          if (opt.opt->is ("trackgen.bundle")) {
            const std::string name (opt[0]);
            // Accept either the displayed name or the bare bundle name, since the
            // atlas ones are shown with an "atlas_" prefix.
            for (const auto& entry : catalogue) {
              if (entry.name == name || entry.name == "atlas_" + name) {
                bundle_search_activated (qstr (entry.name));
                break;
              }
            }
            if (current_bundle().empty())
              WARN ("no atlas bundle named \"" + name + "\"; is -trackgen.atlas given first, "
                    "or the built-in atlas aligned?");
            return true;
          }

          if (opt.opt->is ("trackgen.mdf"))       { mdf_button->setValue (float (opt[0]));       return true; }
          if (opt.opt->is ("trackgen.dilate"))    { dilate_button->setValue (float (opt[0]));    return true; }
          if (opt.opt->is ("trackgen.enddilate")) { endpoint_dilate_button->setValue (float (opt[0])); return true; }
          if (opt.opt->is ("trackgen.qb"))        { qb_button->setValue (float (opt[0]));        return true; }
          if (opt.opt->is ("trackgen.metric"))   { metric_combo->setCurrentIndex (int (opt[0]));return true; }
          if (opt.opt->is ("trackgen.maxseeds")) { max_seeds_factor = size_t (int (opt[0]));    return true; }
          if (opt.opt->is ("trackgen.step"))      { step_button->setValue (float (opt[0]));      return true; }
          if (opt.opt->is ("trackgen.angle"))     { angle_button->setValue (float (opt[0]));     return true; }
          if (opt.opt->is ("trackgen.cutoff"))    { cutoff_button->setValue (float (opt[0]));    return true; }
          if (opt.opt->is ("trackgen.minlength")) { minlength_button->setValue (float (opt[0])); return true; }
          if (opt.opt->is ("trackgen.maxlength")) { maxlength_button->setValue (float (opt[0])); return true; }
          if (opt.opt->is ("trackgen.noendpoints")) { endpoint_includes_box->setChecked (false); return true; }
          if (opt.opt->is ("trackgen.nomask"))      { restrict_box->setChecked (false); return true; }
          if (opt.opt->is ("trackgen.seedspervoxel")) { seeds_per_voxel_button->setValue (int (opt[0])); return true; }

          if (opt.opt->is ("trackgen.seed"))    { add_region_by_name (opt[0], SEED);    return true; }
          if (opt.opt->is ("trackgen.include")) { add_region_by_name (opt[0], INCLUDE); return true; }
          if (opt.opt->is ("trackgen.exclude")) { add_region_by_name (opt[0], EXCLUDE); return true; }
          if (opt.opt->is ("trackgen.mask"))    { add_region_by_name (opt[0], MASK);    return true; }

          if (opt.opt->is ("trackgen.run")) {
            try {
              run_and_save (opt[0]);
            } catch (Exception& e) {
              e.display();
            }
            return true;
          }

          return false;
        }



        void TrackGen::get_session (nlohmann::json& node) const
        {
          node = nlohmann::json::object();
          node["algorithm"] = current_algorithm();
          node["source"] = source_combo->currentData().toString().toStdString();
          nlohmann::json regions = nlohmann::json::array();
          for (const auto& a : assignments)
            regions.push_back ({ { "key", a.region.key },
                                 { "provider", a.region.provider },
                                 { "name", a.region.name },
                                 { "role", a.role } });
          node["regions"] = regions;
          node["bundle"] = current_bundle();
          node["mode"] = guided() ? "guided" : "expert";
          node["quality"] = quality_combo->currentIndex();
          nlohmann::json selected = nlohmann::json::array();
          for (const std::string& name : selected_bundle_names())
            selected.push_back (name);
          node["bundles_selected"] = selected;
          nlohmann::json bundles = nlohmann::json::array();
          for (const auto& path : user_bundle_paths)
            bundles.push_back (path);
          node["atlas_bundles"] = bundles;
        }



        void TrackGen::set_session (const nlohmann::json& node)
        {
          if (!node.is_object())
            return;
          if (node.find ("algorithm") != node.end())
            algorithm_combo->setCurrentIndex (node["algorithm"].get<int>());
          refresh_sources_slot();
          if (node.find ("source") != node.end()) {
            const int idx = source_combo->findData (qstr (node["source"].get<std::string>()));
            if (idx >= 0)
              source_combo->setCurrentIndex (idx);
          }

          assignments.clear();
          if (node.find ("regions") != node.end() && node["regions"].is_array()) {
            vector<RegionRef> available;
            collect_regions (available);
            for (const auto& entry : node["regions"]) {
              const std::string key = entry.value ("key", std::string());
              for (const auto& region : available) {
                if (region.key != key)
                  continue;
                Assignment a;
                a.region = region;
                a.role = entry.value ("role", int (INCLUDE));
                assignments.push_back (a);
                break;
              }
            }
          }
          rebuild_region_table();

          if (node.find ("atlas_bundles") != node.end() && node["atlas_bundles"].is_array()) {
            vector<std::string> paths;
            for (const auto& entry : node["atlas_bundles"])
              paths.push_back (entry.get<std::string>());
            load_atlas_bundles (paths);
          }
          if (node.find ("mode") != node.end())
            mode_combo->setCurrentIndex (node["mode"].get<std::string>() == "expert" ? 1 : 0);
          if (node.find ("quality") != node.end())
            quality_combo->setCurrentIndex (node["quality"].get<int>());

          checked_bundles.clear();
          if (node.find ("bundles_selected") != node.end() && node["bundles_selected"].is_array()) {
            for (const auto& entry : node["bundles_selected"]) {
              const std::string name = entry.get<std::string>();
              // Only names still in the catalogue: an atlas may have been repointed
              // since, and a tick on a bundle that no longer exists would fail at
              // Generate rather than here.
              for (const auto& candidate : catalogue) {
                if (candidate.name == name) { checked_bundles.insert (name); break; }
              }
            }
          } else if (node.find ("bundle") != node.end()) {
            // Written by a build that saved one bundle by path.
            const std::string wanted = node["bundle"].get<std::string>();
            for (const auto& entry : catalogue) {
              if (entry.data == wanted || bundle_path_for (entry.data) == wanted) {
                checked_bundles.insert (entry.name);
                break;
              }
            }
          }
          // Show the category the first ticked bundle lives in, so the restored
          // selection is visible rather than only counted.
          if (checked_bundles.size()) {
            for (const auto& entry : catalogue) {
              if (!checked_bundles.count (entry.name))
                continue;
              const int idx = category_combo->findText (qstr (entry.category));
              if (idx > 0) {
                category_combo->blockSignals (true);
                category_combo->setCurrentIndex (idx);
                category_combo->blockSignals (false);
              }
              break;
            }
          } else {
            category_combo->setCurrentIndex (0);
          }
          refresh_bundles_in_category (QString());
          update_mode();
          suggest_thresholds_for_bundle();
        }


      }
    }
  }
}
