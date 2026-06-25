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
#include "image.h"
#include "datatype.h"
#include "progressbar.h"

using namespace MR;
using namespace App;

// clang-format off
void usage ()
{
  AUTHOR = "MRtrix3 contributors";

  SYNOPSIS = "Interactive grow-cut segmentation from user-provided seeds";

  DESCRIPTION
  + "This command performs multi-label segmentation using the grow-cut "
    "cellular-automaton algorithm (Vezhnevets & Konouchine, 2005). Starting "
    "from a sparse label image of seeds, each labelled voxel competes to "
    "assign its label to its neighbours, weighted by intensity similarity, "
    "until the labelling converges. Every voxel in the output receives the "
    "label of the seed region that wins the competition."

  + "The seed image should contain integer labels: 0 marks unlabelled voxels "
    "(to be segmented), and values 1, 2, ... mark the seeds of each region. "
    "The input intensity image and the seed image must have matching spatial "
    "dimensions.";

  ARGUMENTS
  + Argument ("input", "the input intensity image").type_image_in()
  + Argument ("seeds", "the seed label image (0 = unlabelled, 1.. = seed labels)").type_image_in()
  + Argument ("output", "the output label image").type_image_out();

  OPTIONS
  + Option ("iterations", "the maximum number of iterations (default: run until convergence, capped at 1000)")
  +   Argument ("number").type_integer (1, 100000);
}
// clang-format on


void run ()
{
  auto image = Image<float>::open (argument[0]);
  auto seed  = Image<uint32_t>::open (argument[1]);

  for (size_t a = 0; a != 3; ++a) {
    if (image.size (a) != seed.size (a))
      throw Exception ("input intensity and seed images do not have matching spatial dimensions");
  }

  const size_t nx = image.size (0), ny = image.size (1), nz = image.size (2);
  const size_t N = nx * ny * nz;
  auto idx = [&] (size_t x, size_t y, size_t z) { return x + nx * (y + ny * z); };

  vector<float> intensity (N);
  vector<uint32_t> label (N);
  vector<float> strength (N, 0.0f);

  // load intensities + seeds into flat buffers, and find the intensity range
  float vmin = std::numeric_limits<float>::infinity();
  float vmax = -std::numeric_limits<float>::infinity();
  for (size_t z = 0; z != nz; ++z) {
    image.index (2) = z; seed.index (2) = z;
    for (size_t y = 0; y != ny; ++y) {
      image.index (1) = y; seed.index (1) = y;
      for (size_t x = 0; x != nx; ++x) {
        image.index (0) = x; seed.index (0) = x;
        const float v = image.value();
        const size_t i = idx (x, y, z);
        intensity[i] = v;
        if (std::isfinite (v)) { vmin = std::min (vmin, v); vmax = std::max (vmax, v); }
        const uint32_t s = seed.value();
        label[i] = s;
        strength[i] = s ? 1.0f : 0.0f;
      }
    }
  }
  const float range = (vmax > vmin) ? (vmax - vmin) : 1.0f;

  const int max_iter = get_options ("iterations").size() ? int (get_options ("iterations")[0][0]) : 1000;

  // 6-connected neighbourhood
  const int dx[6] = { 1, -1, 0, 0, 0, 0 };
  const int dy[6] = { 0, 0, 1, -1, 0, 0 };
  const int dz[6] = { 0, 0, 0, 0, 1, -1 };

  vector<uint32_t> next_label (label);
  vector<float> next_strength (strength);

  ProgressBar progress ("performing grow-cut segmentation");
  int iter = 0;
  bool changed = true;
  for (; iter != max_iter && changed; ++iter) {
    changed = false;
    for (size_t z = 0; z != nz; ++z) {
      for (size_t y = 0; y != ny; ++y) {
        for (size_t x = 0; x != nx; ++x) {
          const size_t p = idx (x, y, z);
          uint32_t best_label = label[p];
          float best_strength = strength[p];
          const float cp = intensity[p];
          for (int n = 0; n != 6; ++n) {
            const long qx = long (x) + dx[n], qy = long (y) + dy[n], qz = long (z) + dz[n];
            if (qx < 0 || qy < 0 || qz < 0 || size_t (qx) >= nx || size_t (qy) >= ny || size_t (qz) >= nz)
              continue;
            const size_t q = idx (qx, qy, qz);
            if (strength[q] <= best_strength)
              continue;
            const float g = 1.0f - std::abs (cp - intensity[q]) / range;   // similarity weight in [0,1]
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
  INFO ("grow-cut converged after " + str (iter) + " iterations");

  Header header_out (image);
  header_out.ndim() = 3;
  header_out.datatype() = DataType::UInt32;
  header_out.datatype().set_byte_order_native();
  auto out = Image<uint32_t>::create (argument[2], header_out);
  for (size_t z = 0; z != nz; ++z) {
    out.index (2) = z;
    for (size_t y = 0; y != ny; ++y) {
      out.index (1) = y;
      for (size_t x = 0; x != nx; ++x) {
        out.index (0) = x;
        out.value() = label[idx (x, y, z)];
      }
    }
  }
}
