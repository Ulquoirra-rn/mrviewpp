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

#include <memory>

#include "command.h"
#include "progressbar.h"
#include "file/path.h"

#include "dwi/tractography/file.h"
#include "dwi/tractography/properties.h"
#include "dwi/tractography/recognition/refine.h"

using namespace MR;
using namespace MR::DWI;
using namespace MR::DWI::Tractography;
using namespace MR::DWI::Tractography::Recognition;
using namespace App;

const char* const metric_choices[] = { "mdf", "mean", "p90", "hausdorff", nullptr };

void usage ()
{
  AUTHOR = "MRView++ contributors";

  SYNOPSIS = "Keep the streamlines that belong to a reference bundle";

  DESCRIPTION
  + "Five tests are applied in turn, cheapest first: the streamline's length "
    "against the reference bundle's own length distribution; its two ends against "
    "the bundle's two terminal clouds; its shape distance to the bundle; whether a "
    "competing bundle is a better match for it; and whether it is an outlier among "
    "what survived."

  + "The fourth is the one that separates bundles sharing a territory. A threshold "
    "on distance-to-CST cannot exclude the medial lemniscus, because much of ML is "
    "closer to CST than the looser members of CST are; asking which of the two each "
    "streamline is closest to can. Give each competing bundle with -competitor."

  + "This is the command-line form of what MRView++'s Track generation tool applies "
    "after tracking, and exists so the effect of each test can be measured rather "
    "than assumed.";

  ARGUMENTS
  + Argument ("reference", "the bundle to recognise, as a track file").type_tracks_in()
  + Argument ("input", "the candidate streamlines").type_tracks_in()
  + Argument ("output", "the streamlines kept").type_tracks_out();

  OPTIONS
  + Option ("competitor", "a bundle that may claim a streamline away from the "
                          "reference. May be given more than once.").allow_multiple()
  +   Argument ("tracks").type_tracks_in()

  + Option ("margin", "how much closer a competitor must be before it takes a "
                      "streamline, as a fraction of the distance to the reference "
                      "(default: 0.9). At 1.0 the nearest bundle simply wins, which "
                      "makes near-ties arbitrary; lower keeps more with the reference.")
  +   Argument ("value").type_float (0.0, 1.0)

  + Option ("no_competition", "measure the competitors but do not act on them, so "
                              "the report says what competition would have removed.")

  + Option ("hard_competition", "let a competitor reject a streamline outright, instead "
                                "of adding to its score. The default folds competition "
                                "into the same ranking strictness cuts: a streamline a "
                                "neighbour fits better is penalised by how much better, "
                                "so it is dropped before an equally distant streamline "
                                "nobody contests, rather than regardless of everything else.")

  + Option ("competitive_weight", "how heavily that penalty weighs against the distance "
                                  "(default: 1.0). 0 measures the competitors without "
                                  "letting them move anything.")
  +   Argument ("value").type_float (0.0, 100.0)

  + Option ("metric", "how shape distance is measured: \"hausdorff\" (default) is "
                      "the strictest, rejecting a streamline for a single excursion; "
                      "\"p90\" is the same but robust to one stray vertex; \"mean\" "
                      "is the most tolerant; \"mdf\" compares at matched relative "
                      "positions and needs streamlines of similar length.")
  +   Argument ("name").type_choice (metric_choices)

  + Option ("distance", "acceptance distance in mm (default: 10 for mdf, 4 for the "
                        "others). 0 keeps everything that passed the other tests.")
  +   Argument ("value").type_float (0.0)

  + Option ("strictness", "keep the closest this fraction of candidates instead of "
                          "applying a distance in mm - 0.8 keeps the closest 80%. "
                          "Behaves the same way on every bundle, where a millimetre "
                          "threshold does not.")
  +   Argument ("fraction").type_float (0.0, 1.0)

  + Option ("outliers", "drop streamlines further than median + k.MAD from the rest "
                        "of what was kept. Lower k is stricter; 3.0, 2.0 and 1.5 are "
                        "what the GUI offers as low, medium and high.")
  +   Argument ("k").type_float (0.0)

  + Option ("endpoints", "require each end at the *corresponding* end of the "
                         "reference bundle, within this radius in mm. Without it, "
                         "both ends need only be near some terminus, so a streamline "
                         "that leaves and returns to the same end passes.")
  +   Argument ("radius").type_float (0.0)

  + Option ("population", "a population probability map for the reference bundle, as a "
                          "NIfTI volume in the same space as the streamlines. A "
                          "streamline spending more than -population_outside of its "
                          "course where fewer than -population_prob of the population "
                          "has the tract is rejected. This asks where a streamline "
                          "goes rather than whether it looks like an atlas streamline, "
                          "so it does not inherit the atlas's tracking method.")
  +   Argument ("image").type_image_in()

  + Option ("population_prob", "probability below which a voxel counts as outside the "
                               "tract (default: 0.05).")
  +   Argument ("value").type_float (0.0, 1.0)

  + Option ("population_outside", "share of the streamline's course allowed outside "
                                  "(default: 0.1). 1 disables the test.")
  +   Argument ("share").type_float (0.0, 1.0)

  + Option ("population_trim", "share of each end left out of the measurement (default: "
                               "0.1). A tract's ends fan into cortex, where no two "
                               "subjects agree and the population probability is near "
                               "zero.")
  +   Argument ("share").type_float (0.0, 0.4)

  + Option ("self_claim", "share of the reference bundle's own held-out streamlines a "
                          "competitor may take before it is dropped as not a rival "
                          "(default: 0.5). 1 keeps every competitor, which is how to "
                          "see what this is worth: a category holds bundles at more "
                          "than one granularity, and SLF1_L takes all 400 of SLF_L.")
  +   Argument ("share").type_float (0.0, 1.0)

  + Option ("territory", "reject a streamline that spends more than this share of its "
                         "course on a neighbour's own ground. Competition decides per "
                         "streamline; this asks per sample, which is the resolution at "
                         "which two tracts are seen to overlap. Needs -competitor.")
  +   Argument ("fraction").type_float (0.0, 1.0)

  + Option ("territory_margin", "how much nearer a competitor a sample must be before "
                                "it counts as that competitor's ground, in mm "
                                "(default: 4). Ground the two bundles genuinely share "
                                "counts for neither.")
  +   Argument ("mm").type_float (0.0)

  + Option ("fit", "fit the reference bundle onto the candidates - a rigid transform "
                   "plus a uniform scale - before measuring shape, instead of "
                   "loosening the threshold to absorb a registration offset. This is "
                   "what RecoBundles does between its reduction and pruning passes.")

  + Option ("no_loosen", "do not let a poorly placed reference bundle loosen the "
                         "acceptance distance. The default does, which buys "
                         "sensitivity and pays in specificity.")

  + Option ("per_node", "score outliers by their worst point against the bundle core "
                        "rather than by one distance for the whole streamline. Needs "
                        "-outliers, whose value then reads as standard deviations.")

  + Option ("no_length_window", "use the fixed 0.5-1.75x length ratios against the "
                                "reference bundle's extremes, instead of 0.5-1.25x "
                                "against its 2nd-98th percentiles.")

  + Option ("qb_radius", "radius in mm used to compress the reference bundle before "
                         "matching (default: 8).")
  +   Argument ("value").type_float (0.0)

  + Option ("rejected", "also write out the streamlines that were discarded.")
  +   Argument ("tracks").type_tracks_out();
}


namespace
{
  vector<Streamline<float>> read_tracks (const std::string& path, Properties& properties)
  {
    vector<Streamline<float>> out;
    Reader<float> reader (path, properties);
    Streamline<float> tck;
    while (reader (tck))
      out.push_back (tck);
    return out;
  }
}


void run ()
{
  const int metric_index = get_option_value ("metric", 3);   // hausdorff

  BundleMatcher::Params match;
  match.metric = BundleMatcher::Metric (metric_index);
  match.qb_threshold = get_option_value ("qb_radius", 8.0);
  match.max_mdf = get_option_value ("distance",
      metric_index == int (BundleMatcher::Metric::MDF) ? 10.0 : 4.0);

  RefineOptions options;
  options.length_window = !get_options ("no_length_window").size();
  auto endpoints = get_options ("endpoints");
  if (endpoints.size()) {
    options.endpoint_gate = true;
    options.endpoint_radius = float (endpoints[0][0]);
  }
  auto strictness = get_options ("strictness");
  if (strictness.size())
    options.keep_fraction = float (strictness[0][0]);
  auto outliers = get_options ("outliers");
  if (outliers.size())
    options.outlier_k = float (outliers[0][0]);
  options.local_registration = get_options ("fit").size();
  auto population_prob = get_options ("population_prob");
  if (population_prob.size())
    options.population_min_probability = float (population_prob[0][0]);
  auto population_outside_opt = get_options ("population_outside");
  if (population_outside_opt.size())
    options.population_max_outside = float (population_outside_opt[0][0]);
  auto population_trim = get_options ("population_trim");
  if (population_trim.size())
    options.population_trim = float (population_trim[0][0]);

  std::unique_ptr<PopulationMap> population;
  auto population_opt = get_options ("population");
  if (population_opt.size())
    population.reset (new PopulationMap (std::string (population_opt[0][0])));

  auto self_claim = get_options ("self_claim");
  if (self_claim.size())
    options.competitor_self_claim_limit = float (self_claim[0][0]);

  auto territory = get_options ("territory");
  if (territory.size()) {
    options.exclusive_territory = true;
    options.exclusive_fraction = float (territory[0][0]);
  }
  auto territory_margin = get_options ("territory_margin");
  if (territory_margin.size())
    options.exclusive_margin = float (territory_margin[0][0]);

  options.per_node_outliers = get_options ("per_node").size();
  options.adaptive_loosening = !get_options ("no_loosen").size();
  options.competitive_margin = get_option_value ("margin", 0.9);

  Properties reference_properties, input_properties;
  const vector<Streamline<float>> reference = read_tracks (argument[0], reference_properties);
  if (reference.empty())
    throw Exception ("no streamlines in reference bundle \"" + std::string (argument[0]) + "\"");
  const vector<Streamline<float>> candidates = read_tracks (argument[1], input_properties);
  if (candidates.empty())
    throw Exception ("no streamlines in \"" + std::string (argument[1]) + "\"");

  vector<Competitor> competitors;
  for (const auto& opt : get_options ("competitor")) {
    Competitor competitor;
    competitor.name = Path::basename (opt[0]);
    const size_t dot = competitor.name.find_last_of ('.');
    if (dot != std::string::npos)
      competitor.name = competitor.name.substr (0, dot);
    Properties ignored;
    competitor.bundle = read_tracks (opt[0], ignored);
    // Braces are not optional around WARN: the macro expands to a bare "if",
    // so an unbraced else here binds to *its* if and the push never runs.
    if (competitor.bundle.empty()) {
      WARN ("competitor \"" + competitor.name + "\" is empty and will be ignored");
    } else {
      competitors.push_back (std::move (competitor));
    }
  }
  options.competitive = competitors.size() && !get_options ("no_competition").size();
  options.competitive_scoring = !get_options ("hard_competition").size();
  options.competitive_weight = get_option_value ("competitive_weight", options.competitive_weight);

  vector<Streamline<float>> kept, rejected;
  RefineReport report;
  vector<RefineStage> stages;
  refine_bundle (reference, match, options, competitors, candidates, kept, rejected, report,
                 &stages, population.get());

  CONSOLE (report.summary());
  if (options.local_registration) {
    CONSOLE (report.fit_applied
        ? "local fit applied: " + str (report.fit_translation, 3) + " mm, "
          + str (report.fit_rotation, 3) + " deg, scale " + str (report.fit_scale, 4)
        : std::string ("local fit refused (outside the plausible bounds, or too few candidates)"));
  }
  CONSOLE ("length window " + str (report.min_length, 4) + " to " + str (report.max_length, 4)
           + " mm; acceptance distance " + str (report.applied_distance, 3) + " mm"
           + (report.outlier_cutoff ? "; outlier cut-off " + str (report.outlier_cutoff, 3) + " mm" : ""));
  // The fraction is what a measurement run wants: pointed at a held-out half of the
  // reference bundle it is a sensitivity, and pointed at a different bundle it is a
  // false-positive rate.
  CONSOLE ("kept fraction: " + str (100.0 * double (report.kept) / double (report.input), 4) + "%");

  // When -no_competition was given the competitors were still measured, so say what
  // acting on them would have cost - that comparison is the whole point of having
  // both modes.
  if (competitors.size() && !options.competitive) {
    RefineOptions with = options;
    with.competitive = true;
    vector<Streamline<float>> k2, r2;
    RefineReport report2;
    refine_bundle (reference, match, with, competitors, candidates, k2, r2, report2,
                   nullptr, population.get());
    CONSOLE ("with competition it would keep " + str (report2.kept) + " ("
             + str (100.0 * double (report2.kept) / double (report2.input), 4) + "%)");
  }

  auto write = [&] (const std::string& path, const vector<Streamline<float>>& tracks) {
    // Properties is not copyable (Seeding::List owns its seeders), so carry the
    // key/value pairs across rather than the whole object.
    Properties out;
    static_cast<KeyValues&> (out) = static_cast<const KeyValues&> (input_properties);
    out.comments = input_properties.comments;
    out.prior_rois = input_properties.prior_rois;
    out["refine_reference"] = Path::basename (argument[0]);
    out["refine_distance"] = str (report.applied_distance);
    if (competitors.size() && options.competitive) {
      std::string names;
      for (const auto& c : competitors)
        names += (names.size() ? "," : "") + c.name;
      out["refine_competitors"] = names;
    }
    Writer<float> writer (path, out);
    for (const auto& tck : tracks)
      writer (tck);
  };
  write (argument[2], kept);
  auto rejected_option = get_options ("rejected");
  if (rejected_option.size())
    write (rejected_option[0][0], rejected);
}
