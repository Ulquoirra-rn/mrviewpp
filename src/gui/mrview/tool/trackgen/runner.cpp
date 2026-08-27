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

#include <algorithm>

#include "gui/mrview/tool/trackgen/runner.h"

#include "algo/loop.h"
#include "math/SH.h"
#include "dwi/directions/predefined.h"

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



        bool trackgen_algorithm_uses_sh (size_t index)
        {
          // Indices follow trackgen_algorithms above:
          //   0 FACT          peaks image - usable, via sh_to_peaks() below
          //   1 iFOD1         SH
          //   2 iFOD2         SH
          //   3 NullDist1     null distribution, not a real tracking result
          //   4 NullDist2     null distribution, not a real tracking result
          //   5 SD_STREAM     SH
          //   6 SeedTest      generates no streamlines
          //   7 Tensor_Det    DWI series with a gradient table
          //   8 Tensor_Prob   DWI series with a gradient table
          switch (index) {
            case 0: case 1: case 2: case 5: return true;
            default: return false;
          }
        }



        bool trackgen_algorithm_needs_peaks (size_t index)
        {
          return index == 0;   // FACT
        }



        std::string sh_to_peaks (const std::string& sh_path, const std::string& out_path,
                                 size_t num_peaks)
        {
          // What sh2peaks does: a Newton search along each of a fixed set of 60
          // directions, keeping the distinct maxima. Done in-process rather than by
          // shelling out, so FACT works without sh2peaks being installed.
          auto sh = Image<float>::open (sh_path);
          const int lmax = Math::SH::LforN (sh.size(3));
          if (lmax <= 0)
            throw Exception ("\"" + sh_path + "\" is not a spherical harmonic image");

          Header header (sh);
          header.size(3) = 3 * num_peaks;
          header.datatype() = DataType::Float32;
          auto out = Image<float>::create (out_path, header);

          const Eigen::MatrixXd directions = DWI::Directions::electrostatic_repulsion_60();
          Math::SH::PrecomputedAL<float> precomputer (lmax);

          Eigen::VectorXf values (sh.size(3));
          for (auto l = Loop (0, 3) (sh, out); l; ++l) {
            for (ssize_t v = 0; v != sh.size(3); ++v) {
              sh.index(3) = v;
              values[v] = sh.value();
            }

            vector<std::pair<float, Eigen::Vector3f>> peaks;
            for (ssize_t d = 0; d != directions.rows(); ++d) {
              const double azimuth = directions (d, 0), elevation = directions (d, 1);
              Eigen::Vector3f direction (std::cos (azimuth) * std::sin (elevation),
                                         std::sin (azimuth) * std::sin (elevation),
                                         std::cos (elevation));
              const float amplitude = Math::SH::get_peak (values, lmax, direction, &precomputer);
              // sh2peaks applies no amplitude threshold by default, so neither do we.
              if (!std::isfinite (amplitude))
                continue;
              // Newton searches from neighbouring start directions converge on the
              // same maximum; keep only the distinct ones. The 0.99 cutoff is
              // sh2peaks' DOT_THRESHOLD - a looser one merges genuinely separate
              // crossing-fibre peaks.
              bool duplicate = false;
              for (const auto& existing : peaks) {
                if (std::abs (direction.dot (existing.second)) > 0.99f) {
                  duplicate = true;
                  break;
                }
              }
              if (!duplicate)
                peaks.push_back ({ amplitude, direction });
            }
            std::sort (peaks.begin(), peaks.end(),
                       [] (const std::pair<float, Eigen::Vector3f>& a,
                           const std::pair<float, Eigen::Vector3f>& b) { return a.first > b.first; });

            for (size_t n = 0; n != num_peaks; ++n) {
              const bool have = n < peaks.size();
              for (size_t axis = 0; axis != 3; ++axis) {
                out.index(3) = 3*n + axis;
                out.value() = have ? peaks[n].first * peaks[n].second[axis] : NaN;
              }
            }
          }
          return out_path;
        }



        bool trackgen_algorithm_uses_rk4 (size_t index)
        {
          // iFOD2 and NullDist2 throw "4th-order Runge-Kutta integration not valid"
          // (algorithms/iFOD2.h); FACT does the same but is not offered anyway.
          return !(index == 2 || index == 4 || index == 0);
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
                    {
                      std::lock_guard<std::mutex> lock (state.results_mutex);
                      out.push_back (Streamline<float> (tck));
                    }
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
