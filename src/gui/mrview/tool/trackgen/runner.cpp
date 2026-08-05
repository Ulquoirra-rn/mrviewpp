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

#include "gui/mrview/tool/trackgen/runner.h"

#include "thread_queue.h"

#include "dwi/tractography/tracking/early_exit.h"
#include "dwi/tractography/tracking/exec.h"
#include "dwi/tractography/tracking/generated_track.h"
#include "dwi/tractography/tracking/method.h"
#include "dwi/tractography/tracking/shared.h"
#include "dwi/tractography/tracking/tractography.h"

#include "dwi/tractography/algorithms/fact.h"
#include "dwi/tractography/algorithms/iFOD1.h"
#include "dwi/tractography/algorithms/iFOD2.h"
#include "dwi/tractography/algorithms/nulldist.h"
#include "dwi/tractography/algorithms/sd_stream.h"
#include "dwi/tractography/algorithms/seedtest.h"
#include "dwi/tractography/algorithms/tensor_det.h"
#include "dwi/tractography/algorithms/tensor_prob.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        using namespace MR::DWI::Tractography;
        using namespace MR::DWI::Tractography::Tracking;
        using namespace MR::DWI::Tractography::Algorithms;


        // Same order and spelling as cmd/tckgen.cpp:48, so the index the GUI
        // stores in its session file means the same thing as -algorithm does.
        const char* const trackgen_algorithms[] = {
          "FACT", "iFOD1", "iFOD2", "NullDist1", "NullDist2",
          "SD_STREAM", "SeedTest", "Tensor_Det", "Tensor_Prob", nullptr
        };

        size_t trackgen_num_algorithms ()
        {
          size_t n = 0;
          while (trackgen_algorithms[n]) ++n;
          return n;
        }



        namespace
        {

          // The pipeline sink. Mirrors Tracking::WriteKernel::operator()
          // (write_kernel.cpp:30-50) but appends to a vector instead of a
          // Writer<>, tracks progress in atomics the GUI thread can poll, and
          // honours a cancel flag. Returning false halts the whole pipeline.
          //
          // Single-instance and non-copyable, like WriteKernel: Thread::run_queue
          // only ever calls it from one thread, so no locking is needed.
          class MemorySink { MEMALIGN(MemorySink)
            public:
              MemorySink (const SharedBase& shared,
                          vector<Streamline<float>>& out,
                          TrackGenState& state) :
                  S (shared),
                  out (out),
                  state (state),
                  early_exit (shared) { }

              MemorySink (const MemorySink&) = delete;
              MemorySink& operator= (const MemorySink&) = delete;

              bool complete () const {
                return (S.max_num_tracks && state.selected >= S.max_num_tracks)
                    || (S.max_num_seeds  && state.seeds    >= S.max_num_seeds);
              }

              bool operator() (const GeneratedTrack& tck)
              {
                if (state.cancel || complete())
                  return false;

                switch (tck.get_status()) {
                  case GeneratedTrack::status_t::INVALID:
                    break;
                  case GeneratedTrack::status_t::ACCEPTED:
                    ++state.selected; ++state.streamlines; ++state.seeds;
                    out.push_back (Streamline<float> (tck));
                    break;
                  case GeneratedTrack::status_t::TRACK_REJECTED:
                    ++state.streamlines; ++state.seeds;
                    break;
                  case GeneratedTrack::status_t::SEED_REJECTED:
                    ++state.seeds;
                    break;
                }

                if (early_exit (state.seeds, state.selected)) {
                  WARN ("track generation terminating prematurely: unlikely to reach the target number of streamlines");
                  return false;
                }
                return !state.cancel && !complete();
              }

            private:
              const SharedBase& S;
              vector<Streamline<float>>& out;
              TrackGenState& state;
              EarlyExit early_exit;
          };



          template <class Method>
            void run_algorithm (const std::string& source_path,
                                Properties& properties,
                                vector<Streamline<float>>& out,
                                TrackGenState& state,
                                size_t nthreads)
            {
              // Both of these must be locals: SharedBase keeps a reference to
              // properties, and every Exec copy keeps a reference to shared.
              typename Method::Shared shared (source_path, properties);
              Exec<Method> tracker (shared);
              MemorySink sink (shared, out, state);
              Thread::run_queue (Thread::multi (tracker, nthreads),
                                 Thread::batch (GeneratedTrack(), TRACKING_BATCH_SIZE),
                                 sink);
            }

        }



        void run_tracking (int algorithm,
                           const std::string& source_path,
                           Properties& properties,
                           vector<Streamline<float>>& out,
                           TrackGenState& state,
                           size_t nthreads)
        {
          if (!nthreads)
            nthreads = Thread::number_of_threads();

          switch (algorithm) {
            case 0: run_algorithm<FACT>        (source_path, properties, out, state, nthreads); break;
            case 1: run_algorithm<iFOD1>       (source_path, properties, out, state, nthreads); break;
            case 2: run_algorithm<iFOD2>       (source_path, properties, out, state, nthreads); break;
            case 3: run_algorithm<NullDist1>   (source_path, properties, out, state, nthreads); break;
            case 4: run_algorithm<NullDist2>   (source_path, properties, out, state, nthreads); break;
            case 5: run_algorithm<SDStream>    (source_path, properties, out, state, nthreads); break;
            case 6: run_algorithm<Seedtest>    (source_path, properties, out, state, nthreads); break;
            case 7: run_algorithm<Tensor_Det>  (source_path, properties, out, state, nthreads); break;
            case 8: run_algorithm<Tensor_Prob> (source_path, properties, out, state, nthreads); break;
            default:
              throw Exception ("unknown tracking algorithm index " + str(algorithm));
          }
        }


      }
    }
  }
}
