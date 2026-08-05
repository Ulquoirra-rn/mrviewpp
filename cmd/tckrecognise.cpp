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

#include <cmath>

#include "command.h"
#include "progressbar.h"

#include "file/ofstream.h"

#include "dwi/tractography/file.h"
#include "dwi/tractography/properties.h"
#include "dwi/tractography/recognition/bundle_matcher.h"

using namespace MR;
using namespace App;
using namespace MR::DWI::Tractography;
using namespace MR::DWI::Tractography::Recognition;

const char* const metric_choices[] = { "mdf", "closest", "closest90", "hausdorff", nullptr };

void usage ()
{
  AUTHOR = "MRView++ contributors";

  SYNOPSIS = "Extract the streamlines that match the shape of a reference bundle";

  DESCRIPTION
  + "This selects, from a set of candidate streamlines, those whose shape matches "
    "a reference (atlas) bundle. It is the recognition half of atlas-driven "
    "automatic tract reconstruction: generate streamlines generously within a "
    "bundle's territory, then keep only the ones that actually look like the bundle."

  + "The reference bundle must already be in the same space as the candidates; no "
    "registration is performed."

  + "Each candidate is scored by the flip-invariant mean direct-flip distance (MDF) "
    "to the closest reference shape, after both are resampled to a fixed number of "
    "points. Streamlines scoring at or below -threshold are written out. To keep the "
    "comparison fast the reference bundle is first compressed with QuickBundles, and "
    "candidates are rejected early on length and endpoint position.";

  ARGUMENTS
  + Argument ("tracks", "the candidate track file").type_tracks_in()
  + Argument ("reference", "the reference bundle to match against").type_tracks_in()
  + Argument ("output", "the selected streamlines").type_tracks_out();

  OPTIONS
  + Option ("threshold", "maximum MDF distance for a streamline to be accepted, in mm "
                         "(default: 10)")
  +   Argument ("value").type_float (0.0)

  + Option ("num_points", "number of points each streamline is resampled to before "
                          "comparison (default: 20)")
  +   Argument ("count").type_integer (2)

  + Option ("qb_threshold", "QuickBundles clustering radius used to compress the "
                            "reference bundle, in mm (default: 8)")
  +   Argument ("value").type_float (0.0)

  + Option ("endpoint_dist", "reject a candidate whose endpoints are further than this "
                             "from any reference endpoint, in mm; 0 disables "
                             "(default: 20)")
  +   Argument ("value").type_float (0.0)

  + Option ("length_ratio", "accepted length range, as multiples of the reference "
                            "bundle's own minimum and maximum length "
                            "(default: 0.5 1.75)")
  +   Argument ("min").type_float (0.0)
  +   Argument ("max").type_float (0.0)

  + Option ("metric", "shape distance to use. \"mdf\" compares matched relative positions "
                      "after resampling, and suits a bundle whose streamlines are all a similar "
                      "length. The other three all measure each candidate vertex's distance to "
                      "the nearest bundle vertex - needing no correspondence, so they tolerate a "
                      "wide spread of lengths - and differ only in how they reduce those "
                      "distances: \"closest\" takes the mean (default, most tolerant), "
                      "\"closest90\" the 90th percentile (a robust Hausdorff), and "
                      "\"hausdorff\" the maximum, i.e. the directed Hausdorff distance "
                      "(strictest; one excursion rejects the streamline).")
  +   Argument ("name").type_choice (metric_choices)

  + Option ("distances", "write the distance of every candidate to a text file, "
                         "for choosing a threshold")
  +   Argument ("file").type_file_out();
}



void run ()
{
  BundleMatcher::Params params;
  params.max_mdf = get_option_value ("threshold", params.max_mdf);
  params.num_points = get_option_value ("num_points", params.num_points);
  params.qb_threshold = get_option_value ("qb_threshold", params.qb_threshold);
  params.max_endpoint_dist = get_option_value ("endpoint_dist", params.max_endpoint_dist);
  auto opt = get_options ("metric");
  if (opt.size()) {
    switch (int (opt[0][0])) {
      case 1: params.metric = BundleMatcher::Metric::ClosestMean; break;
      case 2: params.metric = BundleMatcher::Metric::ClosestP90;  break;
      case 3: params.metric = BundleMatcher::Metric::Hausdorff;   break;
      default: params.metric = BundleMatcher::Metric::MDF;        break;
    }
  }

  opt = get_options ("length_ratio");
  if (opt.size()) {
    params.min_length_ratio = opt[0][0];
    params.max_length_ratio = opt[0][1];
  }

  vector<Streamline<float>> reference;
  {
    Properties properties;
    Reader<float> reader (argument[1], properties);
    Streamline<float> tck;
    ProgressBar progress ("reading reference bundle");
    while (reader (tck)) {
      reference.push_back (tck);
      ++progress;
    }
  }
  if (reference.empty())
    throw Exception ("reference bundle \"" + std::string (argument[1]) + "\" is empty");

  BundleMatcher matcher (reference, params);
  INFO ("reference bundle: " + str(reference.size()) + " streamlines, "
        + str(matcher.num_references()) + " reference shapes, lengths "
        + str(matcher.atlas_min_length(), 4) + "-" + str(matcher.atlas_max_length(), 4) + " mm");

  std::unique_ptr<File::OFStream> distances;
  if (get_options ("distances").size())
    distances.reset (new File::OFStream (get_options ("distances")[0][0]));

  Properties properties;
  Reader<float> reader (argument[0], properties);
  properties["recognition_reference"] = std::string (argument[1]);
  properties["recognition_threshold"] = str (params.max_mdf);
  Writer<float> writer (argument[2], properties);

  size_t total = 0, accepted = 0;
  {
    Streamline<float> tck;
    ProgressBar progress ("matching streamlines against reference bundle");
    while (reader (tck)) {
      ++total;
      float d = 0.0f;
      const bool keep = matcher.matches (tck, d);
      if (distances)
        (*distances) << (std::isfinite (d) ? str(d) : "inf") << "\n";
      if (keep) {
        writer (tck);
        ++accepted;
      } else {
        writer.skip();
      }
      ++progress;
    }
  }

  INFO ("accepted " + str(accepted) + " of " + str(total) + " streamlines");
  if (!accepted)
    WARN ("no streamlines matched the reference bundle; consider raising -threshold");
}
