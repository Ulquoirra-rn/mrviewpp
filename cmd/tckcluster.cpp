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

#include "command.h"
#include "progressbar.h"
#include "file/path.h"

#include "dwi/tractography/file.h"
#include "dwi/tractography/properties.h"
#include "dwi/tractography/recognition/cluster.h"

using namespace MR;
using namespace MR::DWI;
using namespace MR::DWI::Tractography;
using namespace MR::DWI::Tractography::Recognition;
using namespace App;

const char* const method_choices[] = { "em", "kmeans", "hierarchical", nullptr };

void usage ()
{
  AUTHOR = "MRView++ contributors";

  SYNOPSIS = "Split a tractogram into clusters by endpoint, midpoint and length";

  DESCRIPTION
  + "Each streamline is reduced to ten numbers in millimetres - its two endpoints, "
    "its midpoint and its length - and those are clustered directly, which is how "
    "DSI Studio groups tracts. Endpoint order is arbitrary in a track file, so "
    "streamlines are oriented consistently first."

  + "One file is written per non-empty cluster, named <prefix>1.tck, <prefix>2.tck "
    "and so on, largest cluster first. This is the command-line equivalent of "
    "\"Cluster into bundles\" in MRView++'s Tracts tool.";

  ARGUMENTS
  + Argument ("tracks", "the input track file").type_tracks_in()
  + Argument ("prefix", "prefix for the output track files").type_text();

  OPTIONS
  + Option ("clusters", "number of clusters to split into (default: 8). Fewer are "
                        "written if the features do not separate that far.")
  +   Argument ("count").type_integer (2)

  + Option ("method", "clustering method: \"em\" (default) fits each cluster its own "
                      "full covariance, which suits the elongated, unequally sized "
                      "clouds that bundles form; \"kmeans\" treats every cluster as "
                      "equally spread and is what seeds EM; \"hierarchical\" compares "
                      "every pair of streamlines with the whole-streamline MDF "
                      "distance and merges by average linkage, at O(N^2) cost.")
  +   Argument ("name").type_choice (method_choices)

  + Option ("iterations", "maximum iterations (default: 100).")
  +   Argument ("count").type_integer (1)

  + Option ("restarts", "how many times to restart k-means from a different seeding, "
                        "keeping the tightest result (default: 5).")
  +   Argument ("count").type_integer (1)

  + Option ("points", "how many points to sample along each streamline, ends included "
                      "(default: 3, i.e. the two ends and the middle).")
  +   Argument ("count").type_integer (2)

  + Option ("length_weight", "weight on the length difference relative to a point "
                             "distance in mm (default: 0.25; 0 ignores length).")
  +   Argument ("value").type_float (0.0);
}


void run ()
{
  const size_t num_clusters = get_option_value ("clusters", 8);
  const size_t iterations = get_option_value ("iterations", 100);
  const int method_index = get_option_value ("method", 0);
  const ClusterMethod method = method_index == 2 ? ClusterMethod::Hierarchical
                             : method_index == 1 ? ClusterMethod::KMeans
                             : ClusterMethod::EM;

  Properties properties;
  vector<Streamline<float>> tracks;
  {
    Reader<float> reader (argument[0], properties);
    Streamline<float> tck;
    ProgressBar progress ("reading streamlines");
    while (reader (tck)) {
      tracks.push_back (tck);
      ++progress;
    }
  }
  if (tracks.empty())
    throw Exception ("no streamlines in \"" + std::string (argument[0]) + "\"");

  ClusterOptions options;
  options.method = method;
  options.max_iterations = iterations;
  options.restarts = get_option_value ("restarts", 5);
  options.num_points = get_option_value ("points", cluster_default_points);
  options.length_weight = get_option_value ("length_weight", cluster_default_length_weight);
  const ClusterResult clustered = cluster_streamlines (tracks, num_clusters, options);

  vector<vector<size_t>> members (clustered.num_clusters);
  for (size_t i = 0; i != tracks.size(); ++i)
    if (clustered.assignment[i] != ClusterResult::invalid)
      members[clustered.assignment[i]].push_back (i);

  vector<size_t> order (members.size());
  for (size_t i = 0; i != order.size(); ++i) order[i] = i;
  std::sort (order.begin(), order.end(), [&members] (size_t a, size_t b) {
    return members[a].size() > members[b].size();
  });

  const std::string prefix (argument[1]);
  size_t written = 0;
  for (size_t k : order) {
    if (members[k].empty())
      continue;
    // Properties is not copyable (Seeding::List owns its seeders), so carry over
    // the key/value pairs and the ROI provenance rather than the whole object.
    Properties out_properties;
    static_cast<KeyValues&> (out_properties) = static_cast<const KeyValues&> (properties);
    out_properties.comments = properties.comments;
    out_properties.prior_rois = properties.prior_rois;
    out_properties["cluster_method"] = method == ClusterMethod::EM ? "EM" : "k-means";
    out_properties["cluster_count"] = str (num_clusters);
    const std::string path = prefix + str (++written) + ".tck";
    Writer<float> writer (path, out_properties);
    for (size_t i : members[k])
      writer (tracks[i]);
    CONSOLE (path + ": " + str (members[k].size()) + " streamlines");
  }

  CONSOLE (str (written) + " clusters from " + str (tracks.size()) + " streamlines in "
           + str (clustered.iterations) + " iterations; mean distance to centre "
           + str (clustered.mean_distance) + " mm");
  if (!clustered.converged)
    WARN ("clustering stopped at the iteration limit rather than settling; "
          "raise -iterations for a stable grouping");
}
