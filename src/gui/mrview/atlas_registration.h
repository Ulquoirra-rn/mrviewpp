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

#ifndef __gui_mrview_atlas_registration_h__
#define __gui_mrview_atlas_registration_h__

#include <atomic>
#include <map>
#include <string>
#include <thread>

#include <QObject>

#include "timer.h"
#include "types.h"
#include "dwi/tractography/streamline.h"
#include "gui/mrview/atlas_template.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {

      //! Aligns the built-in tract atlas to an image, off the GUI thread.
      /*! One instance lives on the Window and owns the result, so every tool sees
       *  the same registration rather than each running its own. Registration takes
       *  a few seconds (antsRegistration, rigid then affine), which is too long to
       *  block the GUI but short enough that no progress reporting beyond a status
       *  string is warranted.
       *
       *  The worker touches no Qt widgets and no GL: it runs a subprocess and
       *  transforms streamline vertices. Results are published back on the GUI
       *  thread via a queued invocation, and \a changed() is emitted for whatever
       *  state was reached, including failure. */
      class AtlasRegistration : public QObject
      { NOMEMALIGN
        Q_OBJECT
        public:
          enum class State { Idle, Running, Ready, Failed };

          explicit AtlasRegistration (QObject* parent = nullptr);
          ~AtlasRegistration ();

          //! Start aligning the atlas to \a image_path.
          /*! Does nothing if that image is already registered or in flight, unless
           *  \a force. A request while another is running is ignored rather than
           *  queued: the user can always ask again. */
          void request (const std::string& image_path,
                        AtlasTemplate::Quality quality = AtlasTemplate::Quality::Affine,
                        bool force = false);

          //! Drop any result, so the next request starts fresh.
          void reset ();

          State state () const { return state_; }
          bool busy () const { return state_ == State::Running; }

          //! Image the current result belongs to; empty when there is none.
          const std::string& registered_image () const { return registered_image_; }
          //! Human-readable outcome, suitable for a status line or tooltip.
          const std::string& note () const { return note_; }

          //! Every bundle the atlas offers, whether or not it has been read.
          /*! Available as soon as the atlas is catalogued, which does not need a
           *  registration - so the list can be shown before, and while, one runs. */
          const vector<AtlasTemplate::BundleRef>& catalogue () const { ensure_catalogue(); return catalogue_; }

          //! One bundle, in the subject's space; empty if it cannot be read.
          /*! Read on demand and cached, because a directory atlas is far too large to
           *  hold whole: the HCP1065 average is 1.3 GB across 102 bundles. The cache
           *  is bounded and drops the least recently used bundle when it is full.
           *
           *  Returns the bundle in atlas space when no registration is Ready, so
           *  callers still get something sensible - check state() to know which. */
          const vector<MR::DWI::Tractography::Streamline<float>>& bundle (const std::string& name);

          //! True if  name is in the catalogue.
          bool has_bundle (const std::string& name) const;

          //! The fit currently in hand; the identity when none is Ready.
          /*! Needed by anything that has to go the other way - a population map stays
           *  in atlas space and is sampled by taking subject points back into it,
           *  which is the inverse of this. */
          const AtlasTemplate::Fit& fit () const { return fit_; }

          //! True if antsRegistration can be found; false means registration is
          //  unavailable and the atlas can only be shown unregistered.
          static bool available ();

        signals:
          void changed ();

        private slots:
          void worker_finished ();

        private:
          std::thread worker_;
          std::atomic<State> state_ { State::Idle };

          //! Cache bound, in streamlines: about 100 MB of vertices.
          static constexpr size_t max_cached_streamlines = 200000;

          std::string pending_image_, registered_image_, note_;
          //! Wall-clock of the fit, so the status says how long it took.
          MR::Timer worker_clock_;
          AtlasTemplate::Fit fit_;
          // Filled on demand, including from const query methods - which is what
          // makes it mutable: asking whether the atlas holds a bundle should not
          // depend on something else having listed the atlas first.
          mutable vector<AtlasTemplate::BundleRef> catalogue_;

          //! Bundles read so far, already in subject space.
          std::map<std::string, vector<MR::DWI::Tractography::Streamline<float>>> cache_;
          //! Least-recently-used order over cache_, most recent last.
          vector<std::string> cache_order_;
          size_t cached_streamlines_ = 0;
          void ensure_catalogue () const;
          void drop_cache ();

          // Written by the worker, read on the GUI thread after it is joined.
          std::string worker_error_;
          AtlasTemplate::Fit worker_fit_;

          void join ();
      };

    }
  }
}

#endif
