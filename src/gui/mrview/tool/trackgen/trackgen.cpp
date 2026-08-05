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

#include <QFileDialog>
#include <QHeaderView>
#include <QMenu>
#include <QMessageBox>
#include <QTimer>

#include "app.h"
#include "progressbar.h"
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

          // --- source + algorithm ---
          QGroupBox* source_box = new QGroupBox (tr ("Source"));
          main_box->addWidget (source_box);
          GridLayout* source_layout = new GridLayout;
          source_box->setLayout (source_layout);

          source_layout->addWidget (new QLabel (tr ("FOD image")), 0, 0);
          source_combo = new QComboBox (this);
          source_combo->setToolTip (tr ("An SH image loaded in the ODF display tool"));
          source_layout->addWidget (source_combo, 0, 1);
          QPushButton* refresh = new QPushButton (tr ("Refresh"), this);
          connect (refresh, SIGNAL (clicked()), this, SLOT (refresh_sources_slot()));
          source_layout->addWidget (refresh, 0, 2);

          source_layout->addWidget (new QLabel (tr ("algorithm")), 1, 0);
          algorithm_combo = new QComboBox (this);
          for (size_t i = 0; i != trackgen_num_algorithms(); ++i)
            algorithm_combo->addItem (trackgen_algorithms[i]);
          algorithm_combo->setCurrentIndex (2);   // iFOD2, as per tckgen
          source_layout->addWidget (algorithm_combo, 1, 1, 1, 2);

          source_layout->addWidget (new QLabel (tr ("atlas bundle")), 2, 0);
          bundle_combo = new QComboBox (this);
          bundle_combo->setToolTip (
              tr ("Select an atlas bundle to auto-track it: its footprint becomes the\n"
                  "tracking territory and its ends become inclusion regions, and the\n"
                  "result is filtered to the streamlines matching its shape.\n\n"
                  "Leave this on \"(none)\" to track from the regions below instead."));
          bundle_combo->addItem (tr ("(none - track from regions below)"));
          connect (bundle_combo, SIGNAL (currentIndexChanged(int)), this, SLOT (bundle_changed_slot(int)));
          source_layout->addWidget (bundle_combo, 2, 1);
          atlas_button = new QPushButton (tr ("Load..."), this);
          atlas_button->setToolTip (tr ("Load .tck bundles from a tract atlas, already in this subject's space"));
          connect (atlas_button, SIGNAL (clicked()), this, SLOT (browse_atlas_slot()));
          source_layout->addWidget (atlas_button, 2, 2);

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
                            "Its scale depends on the metric below, so it is reset when you\n"
                            "switch metric."));
          add_at_param (1, 1, tr ("QB radius (mm)"), qb_button, 0.5f, 8.0f,
                        tr ("clustering radius used to compress the atlas bundle before matching"));

          endpoint_includes_box = new QCheckBox (tr ("require both endpoint regions"), this);
          endpoint_includes_box->setChecked (true);
          endpoint_includes_box->setToolTip (
              tr ("Streamlines must reach both ends of the atlas bundle.\n"
                  "Turn this off for bundles that fan out or terminate diffusely."));
          at_layout->addWidget (endpoint_includes_box, 2, 0, 1, 4);

          restrict_box = new QCheckBox (tr ("confine tracking to bundle territory"), this);
          restrict_box->setChecked (true);
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
          metric_combo->setCurrentIndex (1);   // real atlas bundles are rarely length-uniform
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

          progress_bar = new QProgressBar (this);
          progress_bar->setRange (0, 100);
          progress_bar->hide();
          main_box->addWidget (progress_bar);
          status_label = new QLabel ("");
          status_label->setWordWrap (true);
          main_box->addWidget (status_label);

          poll_timer = new QTimer (this);
          connect (poll_timer, SIGNAL (timeout()), this, SLOT (poll_progress_slot()));

          refresh_sources_slot();
          update_mode();
        }



        TrackGen::~TrackGen ()
        {
          if (worker.joinable()) {
            state.cancel = true;
            worker.join();
          }
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
            status_label->setText (tr ("Load an FOD image in the ODF display tool, then press Refresh."));
          const int idx = source_combo->findText (previous);
          if (idx >= 0)
            source_combo->setCurrentIndex (idx);
        }



        std::string TrackGen::current_bundle () const
        {
          // Index 0 is the "(none)" entry, which means manual mode.
          if (bundle_combo->currentIndex() <= 0)
            return std::string();
          return bundle_combo->currentData().toString().toStdString();
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
          bundle_combo->blockSignals (true);
          bundle_combo->clear();
          bundle_combo->addItem (tr ("(none - track from regions below)"));
          for (const auto& path : paths) {
            std::string name = Path::basename (path);
            const size_t dot = name.find_last_of ('.');
            if (dot != std::string::npos)
              name = name.substr (0, dot);
            bundle_combo->addItem (qstr (name), qstr (path));
            // Bundle name in the list, full path on hover - as in the tool lists.
            bundle_combo->setItemData (bundle_combo->count()-1, qstr (path), Qt::ToolTipRole);
          }
          bundle_combo->blockSignals (false);
          if (paths.size())
            bundle_combo->setCurrentIndex (1);
          else
            update_mode();
        }



        void TrackGen::bundle_changed_slot (int)
        {
          update_mode();
        }



        void TrackGen::metric_changed_slot (int index)
        {
          // The threshold is a distance in mm either way, but the two metrics put
          // it on quite different scales, so carry the matching default across -
          // unless the user has already typed something of their own.
          constexpr float mdf_default = 10.0f, closest_default = 4.0f;
          const float current = mdf_button->value();
          if (index > 0 && (!std::isfinite (current) || current == mdf_default))
            mdf_button->setValue (closest_default);
          else if (index == 0 && (!std::isfinite (current) || current == closest_default))
            mdf_button->setValue (mdf_default);
        }



        void TrackGen::update_mode ()
        {
          // Selecting a bundle *is* the mode switch: the territory and inclusion
          // regions are derived from the bundle, so the manual region table has
          // nothing to contribute and would only be confusing.
          const bool autotrack = current_bundle().size();
          autotrack_box->setVisible (autotrack);
          region_box->setVisible (!autotrack);
          generate_button->setText (autotrack ? tr ("Auto-track bundle") : tr ("Generate"));
        }



        AutotrackParams TrackGen::collect_autotrack_params () const
        {
          AutotrackParams params;
          params.algorithm = algorithm_combo->currentIndex();
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
          std::string current_group;
          for (const auto& region : available) {
            if (region.provider != current_group) {
              current_group = region.provider;
              menu.addSection (qstr (current_group));
            }
            QMenu* sub = menu.addMenu (qstr (region.name));
            QPixmap swatch (12, 12);
            swatch.fill (region.colour);
            sub->setIcon (QIcon (swatch));
            for (int r = 0; r != num_roles; ++r) {
              QAction* action = sub->addAction (tr ("as %1").arg (role_names[r]));
              action->setData (qstr (str(r) + ":" + region.key));
            }
          }

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
        }



        void TrackGen::generate_slot ()
        {
          if (state.running)
            return;

          auto request = std::make_shared<Request>();

          if (!source_combo->count()) {
            QMessageBox::warning (this, "Generate tracks",
                "No FOD image is available.\n\nLoad one in the ODF display tool, then press Refresh.");
            return;
          }
          request->source_path = source_combo->currentData().toString().toStdString();
          request->algorithm = algorithm_combo->currentIndex();
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

          const std::string bundle = current_bundle();

          // Materialise every region NOW, on the GUI thread: the ROI editor keeps
          // its mask only in a GL texture, so this cannot happen in the worker.
          // In auto-track mode the regions come from the bundle instead.
          bool have_seed = false;
          for (const auto& a : bundle.size() ? vector<Assignment>() : assignments) {
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
          if (bundle.empty() && !have_seed) {
            QMessageBox::warning (this, "Generate tracks",
                "No seed region.\n\nAdd at least one region and set its role to \"seed\", "
                "or select an atlas bundle to auto-track.");
            return;
          }

          if (worker.joinable())
            worker.join();
          state.reset();
          state.running = true;
          bundle_generated = 0;
          result_error.clear();
          state.target_selected = std::isfinite (select_button->value()) ? uint64_t (select_button->value()) : 0;
          results.clear();

          set_controls_enabled (false);
          progress_bar->setValue (0);
          progress_bar->show();
          status_label->setText (tr ("starting..."));
          poll_timer->start (200);

          pending_name = bundle.size()
                       ? bundle_combo->currentText().toStdString()
                       : name_for_run (++tract_iteration, request->scalars);

          // mrview swaps ProgressBar's display/done hooks for Qt widgets
          // (gui/dialog/dialog.cpp), and the engine creates ProgressBars while
          // opening its source image. Those must not run off the GUI thread, so
          // silence them for the duration of the run and restore afterwards.
          saved_progress_display = MR::ProgressBar::display_func;
          saved_progress_done = MR::ProgressBar::done_func;
          MR::ProgressBar::display_func = [] (const MR::ProgressBar&) { };
          MR::ProgressBar::done_func = [] (const MR::ProgressBar&) { };

          const AutotrackParams at_params = collect_autotrack_params();

          worker = std::thread ([this, request, bundle, at_params] () {
            // === pure CPU compute: no GL, no Qt GUI ===
            try {
              if (bundle.size()) {
                // Auto-track: the bundle supplies the territory and end regions.
                MR::Header grid = MR::Header::open (request->source_path);
                grid.ndim() = 3;
                AutotrackResult r;
                autotrack_bundle (bundle, request->source_path, grid, at_params, r, state);
                results = std::move (r.tracks);
                result_properties.clear();
                for (const auto& kv : r.effective_properties)
                  result_properties[kv.first] = kv.second;
                result_properties["autotrack_bundle"] = r.name;
                result_properties["autotrack_generated"] = str (r.generated);
                result_properties["autotrack_mdf"] = str (at_params.match.max_mdf);
                bundle_generated = r.generated;
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
          status_label->setText (QString ("%1 seeds, %2 streamlines, %3 selected")
              .arg (seeds).arg (streamlines).arg (selected));
          if (state.target_selected)
            progress_bar->setValue (int (std::min (100.0, 100.0 * double (selected) / double (state.target_selected))));
        }



        void TrackGen::generation_finished ()
        {
          poll_timer->stop();
          if (worker.joinable())
            worker.join();
          state.running = false;

          restore_progress_hooks();

          progress_bar->hide();
          set_controls_enabled (true);

          if (state.error.size() || result_error.size()) {
            if (result_error.size() && state.error.empty())
              state.error = result_error;
            result_error.clear();
            status_label->setText (tr ("failed"));
            QMessageBox::critical (this, "Generate tracks", qstr (state.error));
            results.clear();
            return;
          }

          if (results.empty()) {
            status_label->setText (tr ("no streamlines were selected"));
            return;
          }

          try {
            Tractography* tractography = get_tool<Tractography>();
            if (!tractography)
              throw Exception ("could not open the Tractography tool");
            // Autotrack bundles get distinct solid colours so they can be told
            // apart; a plain tracking run is more informative coloured by direction.
            tractography->add_tractogram_from_memory (results, result_properties, pending_name,
                                                      state.streamlines, bundle_generated > 0);
            if (bundle_generated)
              status_label->setText (QString ("%1 of %2 generated streamlines matched the bundle")
                  .arg (uint64_t (results.size())).arg (uint64_t (bundle_generated)));
            else
              status_label->setText (QString ("%1 streamlines added to the Tractography tool")
                  .arg (uint64_t (results.size())));
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

          const std::string bundle = current_bundle();
          if (bundle.size()) {
            // Auto-track the selected bundle.
            MR::Header grid = MR::Header::open (source_combo->currentData().toString().toStdString());
            grid.ndim() = 3;
            AutotrackResult r;
            state.reset();
            autotrack_bundle (bundle, source_combo->currentData().toString().toStdString(),
                              grid, collect_autotrack_params(), r, state);
            CONSOLE ("trackgen: " + r.name + ": atlas " + str(r.atlas_streamlines)
                     + ", generated " + str(r.generated) + ", recognised " + str(r.tracks.size()));
            if (r.tracks.empty())
              return;
            Properties props;
            for (const auto& kv : r.effective_properties)
              props[kv.first] = kv.second;
            props["autotrack_bundle"] = r.name;
            props["autotrack_generated"] = str (r.generated);
            props["autotrack_mdf"] = str (collect_autotrack_params().match.max_mdf);
            MR::DWI::Tractography::Writer<float> writer (path, props);
            for (const auto& tck : r.tracks)
              writer (tck);
            for (uint64_t k = r.tracks.size(); k < r.attempted; ++k)
              writer.skip();
            try {
              if (Tractography* tractography = get_tool<Tractography>())
                tractography->add_tractogram_from_memory (r.tracks, props, r.name, r.attempted, true);
            } catch (Exception& e) { e.display(); }
            return;
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
          run_tracking (algorithm_combo->currentIndex(),
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
                                         "auto-tracking mode.")
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
                algorithm_combo->setCurrentIndex (i);
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
            // Loading an atlas pre-selects its first bundle; leave the mode to
            // -trackgen.bundle so a plain -trackgen.atlas does not silently
            // switch a region-based setup into auto-tracking.
            bundle_combo->setCurrentIndex (0);
            return true;
          }

          if (opt.opt->is ("trackgen.bundle")) {
            const std::string name (opt[0]);
            const int idx = bundle_combo->findText (qstr (name));
            if (idx > 0)
              bundle_combo->setCurrentIndex (idx);
            else
              WARN ("no atlas bundle named \"" + name + "\" is loaded");
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
          node["algorithm"] = algorithm_combo->currentIndex();
          node["source"] = source_combo->currentData().toString().toStdString();
          nlohmann::json regions = nlohmann::json::array();
          for (const auto& a : assignments)
            regions.push_back ({ { "key", a.region.key },
                                 { "provider", a.region.provider },
                                 { "name", a.region.name },
                                 { "role", a.role } });
          node["regions"] = regions;
          node["bundle"] = current_bundle();
          nlohmann::json bundles = nlohmann::json::array();
          for (int i = 1; i < bundle_combo->count(); ++i)
            bundles.push_back (bundle_combo->itemData (i).toString().toStdString());
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
          bundle_combo->setCurrentIndex (0);
          if (node.find ("bundle") != node.end()) {
            const std::string wanted = node["bundle"].get<std::string>();
            for (int i = 1; i < bundle_combo->count(); ++i)
              if (bundle_combo->itemData (i).toString().toStdString() == wanted)
                bundle_combo->setCurrentIndex (i);
          }
          update_mode();
        }


      }
    }
  }
}
