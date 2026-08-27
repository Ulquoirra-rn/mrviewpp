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

#ifndef __segment_growcut_h__
#define __segment_growcut_h__

#include <functional>

#include "types.h"

namespace MR
{
  namespace Segment
  {

    //! Grow-cut segmentation over a flat 3D grid (Vezhnevets & Konouchine, 2005).
    /*! Each labelled voxel attacks its six neighbours with a strength weighted by
     *  how similar their intensities are; a voxel changes hands when an attack beats
     *  the strength it currently holds. Iterated to convergence, every voxel ends up
     *  with the label of the seed that reached it most easily.
     *
     *  Shared by the mrgrowcut command and MRView++'s ROI editor, which had a copy
     *  each of the same loop.
     *
     *  The iteration is Jacobi - every voxel is computed from the previous state,
     *  never from a neighbour already updated in this sweep - so the result does not
     *  depend on the order voxels are visited, and therefore not on how the work is
     *  divided between threads.
     *
     *  \a intensity, \a label and \a strength are all indexed x + nx*(y + ny*z).
     *  \a label carries the seeds in and the segmentation out; \a strength must be
     *  1 at a seed and 0 elsewhere. \a range is the intensity range of the image.
     *
     *  \a keep_going, if given, is polled once per iteration: return false to stop
     *  early, leaving \a label as whatever it had reached.
     *
     *  Returns the number of iterations run. */
    struct GrowCutResult { NOMEMALIGN
      size_t iterations = 0;
      bool cancelled = false;
      bool converged = false;
    };

    GrowCutResult grow_cut (const vector<float>& intensity,
                            vector<uint32_t>& label,
                            vector<float>& strength,
                            ssize_t nx, ssize_t ny, ssize_t nz,
                            float range,
                            int max_iterations = 1000,
                            const std::function<bool(size_t)>& keep_going = nullptr);

  }
}

#endif
