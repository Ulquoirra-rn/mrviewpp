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

#include "gui/mrview/atlas_registration.h"

#include <QMetaObject>

#include "exception.h"
#include "file/path.h"
#include "gui/mrview/data_path.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {

      AtlasRegistration::AtlasRegistration (QObject* parent) :
        QObject (parent) { }



      AtlasRegistration::~AtlasRegistration ()
      {
        join();
      }



      void AtlasRegistration::join ()
      {
        if (worker_.joinable())
          worker_.join();
      }



      bool AtlasRegistration::available ()
      {
        if (find_data_file (AtlasTemplate::atlas_filename).empty())
          return false;
        if (find_data_file (AtlasTemplate::template_filename).empty())
          return false;
        const std::string ants = AtlasTemplate::ants_path();
        // A bare name means the search fell through to PATH; we cannot tell whether
        // it is there without running it, so assume it is and report the failure
        // properly if it is not.
        return ants.size();
      }



      void AtlasRegistration::reset ()
      {
        join();
        state_ = State::Idle;
        registered_image_.clear();
        note_.clear();
        drop_cache();
        fit_ = AtlasTemplate::Fit();
        emit changed();
      }



      void AtlasRegistration::request (const std::string& image_path,
                                       AtlasTemplate::Quality quality,
                                       bool force)
      {
        if (image_path.empty())
          return;
        if (state_ == State::Running)
          return;
        if (!force && registered_image_ == image_path &&
            (state_ == State::Ready || state_ == State::Failed))
          return;

        join();
        ensure_catalogue();
        pending_image_ = image_path;
        worker_error_.clear();
        worker_fit_ = AtlasTemplate::Fit();
        state_ = State::Running;
        note_ = "aligning the built-in atlas...";
        emit changed();

        // Copy what the worker needs; it must not touch members the GUI thread may
        // read while it runs. The worker computes the transform only - bundles are
        // read afterwards, one at a time, as they are asked for.
        const std::string path = pending_image_;
        worker_clock_.start();
        worker_ = std::thread ([this, path, quality] () {
          try {
            worker_fit_ = AtlasTemplate::fit_to_image (path, quality);
          } catch (Exception& e) {
            worker_error_ = e[0];
          } catch (std::exception& e) {
            worker_error_ = e.what();
          }
          QMetaObject::invokeMethod (this, "worker_finished", Qt::QueuedConnection);
        });
      }



      void AtlasRegistration::ensure_catalogue () const
      {
        if (catalogue_.empty()) {
          try {
            catalogue_ = AtlasTemplate::list_bundles();
          } catch (Exception& e) {
            e.display();
          }
        }
      }



      bool AtlasRegistration::has_bundle (const std::string& name) const
      {
        // A session restored before anything listed the atlas would otherwise answer
        // "no bundle by that name" for every bundle, and every restored tract would
        // come back unrefinable.
        ensure_catalogue();
        for (const auto& ref : catalogue_)
          if (ref.name == name)
            return true;
        return false;
      }



      void AtlasRegistration::drop_cache ()
      {
        cache_.clear();
        cache_order_.clear();
        cached_streamlines_ = 0;
      }



      const vector<MR::DWI::Tractography::Streamline<float>>&
        AtlasRegistration::bundle (const std::string& name)
      {
        static const vector<MR::DWI::Tractography::Streamline<float>> empty;
        ensure_catalogue();

        auto cached = cache_.find (name);
        if (cached != cache_.end()) {
          // Refresh its position in the LRU order.
          for (size_t i = 0; i != cache_order_.size(); ++i) {
            if (cache_order_[i] == name) {
              cache_order_.erase (cache_order_.begin() + i);
              break;
            }
          }
          cache_order_.push_back (name);
          return cached->second;
        }

        const AtlasTemplate::BundleRef* ref = nullptr;
        for (const auto& entry : catalogue_)
          if (entry.name == name) { ref = &entry; break; }
        if (!ref)
          return empty;

        vector<MR::DWI::Tractography::Streamline<float>> tracks;
        try {
          tracks = AtlasTemplate::load_bundle (*ref);
        } catch (Exception& e) {
          e.display();
          return empty;
        }
        if (tracks.empty())
          return empty;

        if (state_ == State::Ready && fit_.refined) {
          const Eigen::Transform<float, 3, Eigen::Affine> T = fit_.mni_to_subject.cast<float>();
          for (auto& tck : tracks)
            for (auto& vertex : tck)
              vertex = (T * vertex).eval();
        }

        // Bound the cache by streamline count rather than bundle count: bundles vary
        // by two orders of magnitude in size, so a count of bundles says nothing
        // about the memory held.
        cached_streamlines_ += tracks.size();
        cache_[name] = std::move (tracks);
        cache_order_.push_back (name);
        while (cached_streamlines_ > max_cached_streamlines && cache_order_.size() > 1) {
          const std::string oldest = cache_order_.front();
          cache_order_.erase (cache_order_.begin());
          const auto it = cache_.find (oldest);
          if (it != cache_.end()) {
            cached_streamlines_ -= it->second.size();
            cache_.erase (it);
          }
        }
        return cache_[name];
      }



      void AtlasRegistration::worker_finished ()
      {
        join();

        registered_image_ = pending_image_;
        fit_ = worker_fit_;

        // Whatever the outcome, anything cached was transformed with the previous
        // fit and must not be reused.
        drop_cache();

        if (worker_error_.size()) {
          state_ = State::Failed;
          note_ = worker_error_;
        } else if (!fit_.refined) {
          // The fit ran but was implausible, or antsRegistration was unavailable.
          // Deliberately no fallback to an unregistered atlas here: the caller
          // decides whether showing it in template space is useful.
          state_ = State::Failed;
          note_ = fit_.note.size() ? fit_.note : std::string ("registration failed");
        } else {
          state_ = State::Ready;
          note_ = "aligned in " + str (worker_clock_.elapsed(), 3) + " s (" + fit_.note + ")";
        }
        emit changed();
      }


    }
  }
}
