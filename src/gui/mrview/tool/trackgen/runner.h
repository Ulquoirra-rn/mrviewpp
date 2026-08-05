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
