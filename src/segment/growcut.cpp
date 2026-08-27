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

#include "segment/growcut.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace MR
{
  namespace Segment
  {

    namespace {

      //! One voxel that is about to change hands.
      struct Change { NOMEMALIGN
        size_t index;
        uint32_t label;
        float strength;
        bool label_changed;
      };

      unsigned int worker_count (size_t work)
      {
        unsigned int threads = std::thread::hardware_concurrency();
        if (!threads)
          threads = 1;
        // Below a few thousand voxels the hand-off costs more than the work.
        return std::max (1u, std::min (threads, (unsigned int) (work / 4096 + 1)));
      }

    }



    GrowCutResult grow_cut (const vector<float>& intensity,
                            vector<uint32_t>& label,
                            vector<float>& strength,
                            ssize_t nx, ssize_t ny, ssize_t nz,
                            float range,
                            int max_iterations,
                            const std::function<bool(size_t)>& keep_going)
    {
      GrowCutResult result;
      const size_t N = size_t(nx) * size_t(ny) * size_t(nz);
      if (!N || intensity.size() < N || label.size() < N || strength.size() < N)
        return result;
      if (!(range > 0.0f))
        range = 1.0f;

      const ssize_t stride_y = nx, stride_z = nx * ny;

      // Only voxels next to something that changed can change, so the sweep follows
      // the front rather than crossing the whole image every time. The first pass is
      // over everything, because at that point anything might move.
      //
      // This is what makes it quick: on a 200x200x160 image seeded with two small
      // blobs the front is a shell of a few thousand voxels, against 6.4 million
      // swept per iteration by the plain version, for hundreds of iterations.
      vector<size_t> active (N);
      for (size_t i = 0; i != N; ++i)
        active[i] = i;

      vector<uint32_t> visited (N, 0);       // iteration number a voxel was queued for
      vector<Change> pending;
      vector<vector<Change>> per_thread;
      vector<size_t> next_active;

      int iteration = 0;
      for (; iteration != max_iterations && active.size(); ++iteration) {
        if (keep_going && !keep_going (size_t (iteration))) {
          result.cancelled = true;
          break;
        }

        const unsigned int threads = worker_count (active.size());
        per_thread.assign (threads, vector<Change>());

        auto sweep = [&] (unsigned int t) {
          const size_t from = active.size() * t / threads;
          const size_t to   = active.size() * (t+1) / threads;
          vector<Change>& out = per_thread[t];
          out.reserve ((to - from) / 4 + 8);
          for (size_t k = from; k != to; ++k) {
            const size_t p = active[k];
            const ssize_t z = ssize_t (p / size_t (stride_z));
            const ssize_t y = ssize_t ((p / size_t (stride_y)) % size_t (ny));
            const ssize_t x = ssize_t (p % size_t (nx));

            uint32_t best_label = label[p];
            float best_strength = strength[p];
            const float cp = intensity[p];

            // Unrolled so the bounds test is one comparison per neighbour rather
            // than a loop over an offset table; this is the innermost work.
            // +x, -x, +y, -y, +z, -z, and the order matters: the running best is
            // beaten only strictly, so when two neighbours attack with exactly equal
            // strength the first one asked keeps the voxel. Changing the order
            // changes the labelling of every tied voxel - measured at 2,316 of
            // 868,352 on one image - so it matches what the plain loop did.
            const ssize_t neighbours[6] = { x < nx-1   ? ssize_t(p) + 1        : -1,
                                            x > 0      ? ssize_t(p) - 1        : -1,
                                            y < ny-1   ? ssize_t(p) + stride_y : -1,
                                            y > 0      ? ssize_t(p) - stride_y : -1,
                                            z < nz-1   ? ssize_t(p) + stride_z : -1,
                                            z > 0      ? ssize_t(p) - stride_z : -1 };
            for (int n = 0; n != 6; ++n) {
              const ssize_t q = neighbours[n];
              if (q < 0)
                continue;
              const float sq = strength[q];
              if (sq <= best_strength)
                continue;
              const float attack = (1.0f - std::abs (cp - intensity[q]) / range) * sq;
              if (attack > best_strength) {
                best_strength = attack;
                best_label = label[q];
              }
            }
            if (best_label != label[p] || best_strength != strength[p])
              out.push_back ({ p, best_label, best_strength, best_label != label[p] });
          }
        };

        if (threads == 1) {
          sweep (0);
        } else {
          vector<std::thread> workers;
          workers.reserve (threads);
          for (unsigned int t = 0; t != threads; ++t)
            workers.emplace_back (sweep, t);
          for (auto& worker : workers)
            worker.join();
        }

        // Applied only now, so every voxel this sweep was computed from the state at
        // the start of it - the Jacobi step the plain version got from its second
        // buffer.
        pending.clear();
        for (auto& chunk : per_thread)
          pending.insert (pending.end(), chunk.begin(), chunk.end());

        bool any_label_changed = false;
        for (const Change& c : pending) {
          label[c.index] = c.label;
          strength[c.index] = c.strength;
          any_label_changed = any_label_changed || c.label_changed;
        }

        // The plain version stops when no *label* moved in a full sweep, even if
        // strengths were still settling. Matched here, so the two agree voxel for
        // voxel rather than approximately.
        if (!any_label_changed) {
          result.converged = true;
          ++iteration;
          break;
        }

        next_active.clear();
        next_active.reserve (pending.size() * 7);
        const uint32_t stamp = uint32_t (iteration) + 1;
        auto queue = [&] (size_t q) {
          if (visited[q] == stamp)
            return;
          visited[q] = stamp;
          next_active.push_back (q);
        };
        for (const Change& c : pending) {
          const size_t p = c.index;
          const ssize_t z = ssize_t (p / size_t (stride_z));
          const ssize_t y = ssize_t ((p / size_t (stride_y)) % size_t (ny));
          const ssize_t x = ssize_t (p % size_t (nx));
          queue (p);
          if (x > 0)     queue (p - 1);
          if (x < nx-1)  queue (p + 1);
          if (y > 0)     queue (p - size_t (stride_y));
          if (y < ny-1)  queue (p + size_t (stride_y));
          if (z > 0)     queue (p - size_t (stride_z));
          if (z < nz-1)  queue (p + size_t (stride_z));
        }
        active.swap (next_active);
      }

      result.iterations = size_t (iteration);
      if (!result.cancelled && active.empty())
        result.converged = true;
      return result;
    }

  }
}
