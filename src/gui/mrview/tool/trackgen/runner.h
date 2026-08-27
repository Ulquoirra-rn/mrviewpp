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

#ifndef __gui_mrview_tool_trackgen_runner_h__
#define __gui_mrview_tool_trackgen_runner_h__

#include <atomic>
#include <mutex>

#include "types.h"
#include "dwi/tractography/properties.h"
#include "dwi/tractography/streamline.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        // Runs MRtrix's tracking engine in-process and collects the streamlines
        // in RAM instead of writing a .tck.
        //
        // tckgen drives this through Exec<Method>::run(), which builds a
        // Tracking::WriteKernel holding a concrete Writer<> - and Writer throws
        // unless the output path ends in ".tck", with no hook for cancellation.
        // Exec's own constructor is public though, so we assemble the same
        // three-stage pipeline ourselves with a different sink; nothing in the
        // tracking engine has to change.

        //! Shared state between the GUI thread and the tracking worker.
        struct TrackGenState { NOMEMALIGN
          std::atomic<bool> running;
          std::atomic<bool> cancel;
          std::atomic<uint64_t> seeds, streamlines, selected;
          uint64_t target_selected, target_seeds;
          std::string error;          // written by the worker, read after join

          //! Guards the caller's output vector while tracking runs.
          /*! The sink appends under this, and the GUI thread takes a snapshot under
           *  it to draw a live preview. Without it the preview would race a
           *  reallocating vector. */
          std::mutex results_mutex;

          TrackGenState () :
              running (false), cancel (false),
              seeds (0), streamlines (0), selected (0),
              target_selected (0), target_seeds (0) { }

          void reset () {
            cancel = false;
            seeds = 0; streamlines = 0; selected = 0;
            error.clear();
          }
        };


        //! The algorithms tckgen offers, in the same order as cmd/tckgen.cpp.
        extern const char* const trackgen_algorithms[];

        //! Number of entries in trackgen_algorithms.
        size_t trackgen_num_algorithms ();

        //! True if algorithm \a index can track from a spherical harmonic (FOD) image.
        /*! The others need a different input entirely - FACT a peaks image,
         *  Tensor_Det/Tensor_Prob a DWI series with a gradient table - so offering
         *  them for an FOD only produces an error once tracking starts. */
        bool trackgen_algorithm_uses_sh (size_t index);

        //! True if algorithm \a index tracks from a peaks image rather than SH.
        bool trackgen_algorithm_needs_peaks (size_t index);

        //! Write the peaks of an SH image to \a out_path, returning that path.
        /*! FACT needs a peaks image; this is what sh2peaks would produce, done
         *  in-process so no extra executable has to be installed. */
        std::string sh_to_peaks (const std::string& sh_path, const std::string& out_path,
                                 size_t num_peaks = 3);

        //! True if algorithm \a index accepts 4th-order Runge-Kutta integration.
        /*! iFOD2 (and NullDist2, which derives from it) reject it outright. */
        bool trackgen_algorithm_uses_rk4 (size_t index);


        //! Run tracking to completion. Blocks; pure CPU, no GL and no Qt.
        /*! \a properties must outlive the call - the engine's SharedBase holds a
         *  reference to it, not a copy. Streamlines are appended to \a out. */
        void run_tracking (int algorithm,
                           const std::string& source_path,
                           MR::DWI::Tractography::Properties& properties,
                           vector<MR::DWI::Tractography::Streamline<float>>& out,
                           TrackGenState& state,
                           size_t nthreads);

      }
    }
  }
}

#endif
