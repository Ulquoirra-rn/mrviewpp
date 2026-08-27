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

#ifndef __gui_mrview_atlas_template_h__
#define __gui_mrview_atlas_template_h__

#include <map>
#include <set>
#include <string>

#include "header.h"
#include "image.h"
#include "types.h"
#include "dwi/tractography/streamline.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {

      //! The bundled tract atlas, and the fit that places it in a subject's space.
      /*! The atlas (yeh2022.trx, 87 named bundles from Yeh's population-averaged
       *  atlas) is streamline geometry in MNI space, so placing it in a subject
       *  requires an MNI-to-subject transform.
       *
       *  That transform is computed by antsRegistration, which is shipped
       *  alongside mrview. MRtrix's own linear registration is deliberately not
       *  used: it offers only a mean-squared metric (the cross-correlation
       *  alternative is an unimplemented stub), and mean-squared cannot solve a
       *  template-to-subject fit, because it can always lower its cost by
       *  shrinking the template out of overlap. Measured across two subjects and
       *  more than twenty configurations, it collapsed the template to a plane
       *  (determinant 0) or rotated it 90 degrees in nearly every case.
       *  antsRegistration uses mutual information, which is what makes the same
       *  fit tractable, and is the tool that produced the transforms this code
       *  was validated against. */
      namespace AtlasTemplate
      {

        //! File names within MRView++'s data directory.
        extern const char* const atlas_filename;
        extern const char* const template_filename;

        //! Registration quality, in ascending order of cost.
        enum class Quality { Affine, Nonlinear };


        //! True if \a header looks like an FOD image (4D with an SH volume count).
        bool is_fod_image (const MR::Header& header);

        //! Absolute path of the antsRegistration executable, or empty if absent.
        /*! Searched alongside mrview first (where the packaged copy lives), then
         *  the MRViewAntsPath config option, then ANTSPATH, then PATH. */
        std::string ants_path ();


        //! Read an ITK/ANTs affine (.mat) as a point transform in RAS.
        /*! ANTs stores the transform in LPS, about a centre of rotation, and in
         *  the direction that maps its fixed image to its moving image. This
         *  returns the equivalent RAS point transform, unchanged in direction, so
         *  the caller decides whether to invert it. False if the file cannot be
         *  read or holds no affine. */
        bool read_ants_affine (const std::string& path, transform_type& out);


        //! Outcome of fitting the atlas template to a subject image.
        struct Fit { NOMEMALIGN
          //! Maps a point in atlas (MNI) space to subject scanner space.
          transform_type mni_to_subject = transform_type::Identity();
          //! False when no usable fit was obtained and the identity is being used
          //  instead; \a note then says why. A silently misplaced atlas is worse
          //  than an atlas that is honestly labelled unregistered.
          bool refined = false;
          float translation_mm = 0.0f, rotation_deg = 0.0f;
          float min_scale = 1.0f, max_scale = 1.0f;
          std::string note;
        };


        //! Fit the bundled MNI template to \a subject_path via antsRegistration.
        /*! Pure CPU and safe to call off the GUI thread; it runs a subprocess and
         *  blocks until it finishes. Never throws for a poor fit - an implausible
         *  result is reported through Fit::note with \a refined false. Throws only
         *  if the bundled data cannot be read. */
        Fit fit_to_image (const std::string& subject_path, Quality quality = Quality::Affine);


        //! One bundle of the atlas, as catalogued but not yet read.
        struct BundleRef { NOMEMALIGN
          std::string category;   //!< subfolder it came from; empty for a .trx group
          std::string name;       //!< bundle name, unique across the atlas
          std::string path;       //!< .tck on disk; empty means it lives in the .trx
        };

        //! Root of a tract atlas laid out as <category>/<bundle>.tck, or empty.
        /*! Resolved from the MRVIEWPP_TRACT_ATLAS environment variable, then the
         *  MRViewTractAtlasPath config option, then a "tracts" directory beside the
         *  bundled data. Empty means no directory atlas is configured and the
         *  bundled .trx is used instead. */
        std::string atlas_directory ();

        //! Every bundle the atlas offers, sorted by category then name.
        /*! Reading the catalogue is a directory scan (or a .trx index read); no
         *  streamlines are loaded. */
        vector<BundleRef> list_bundles ();

        //! Read one bundle, in MNI space, keeping at most  max_streamlines.
        /*! A directory atlas can be very large - the HCP1065 average puts 34,000
         *  streamlines and 110 MB into AF_L alone - and holding all of it would cost
         *  gigabytes for no gain: the shape matcher compresses the bundle to a few
         *  dozen centroids anyway, and the territory it rasterises is saturated long
         *  before the last streamline. So streamlines are taken at an even stride
         *  while reading, never all held at once. */
        vector<MR::DWI::Tractography::Streamline<float>> load_bundle (
            const BundleRef&, size_t max_streamlines = 0);

        //! Default cap on streamlines read per bundle (MRViewTractAtlasMaxStreamlines).
        size_t default_max_streamlines ();

        //! Coarse occupancy of a bundle in atlas space: the cells it runs through.
        /*! Cells are packed integer coordinates on an 8 mm grid, which is enough to
         *  answer "do these two bundles run through the same territory" and small
         *  enough that every bundle's signature fits in a few hundred numbers. */
        using BundleFootprint = std::set<uint64_t>;

        //! Read the cached footprint index, if one matching this atlas exists.
        /*! Cheap - one small JSON file. Returns false when there is no usable cache,
         *  in which case build_footprints() has to run before neighbours_of() will
         *  answer anything. */
        bool load_footprint_cache ();

        //! True once an index is in hand, from the cache or from a build.
        bool footprints_ready ();

        //! Compute the index over every bundle, and cache it.
        /*! Expensive: it reads the whole atlas once, which for a directory atlas
         *  means every .tck (1.3 GB for the HCP1065 average). Blocks, touches no Qt
         *  and no GL, and is meant to be called from a worker thread; the result is
         *  published under a lock, so neighbours_of() stays safe to call while it
         *  runs - it simply answers nothing until this finishes. */
        void build_footprints ();

        //! Bundles running through the same territory as  bundle, closest first.
        /*! Ranked by what fraction of  bundle's own footprint they cover, which is
         *  the question that matters: a bundle that shares this one's course can
         *  steal its streamlines, whether or not it is of a similar size. Empty when
         *  no index is available.
         *
         *   max_count bounds it because each neighbour costs a matcher and a
         *  distance per candidate; measured at 1.25 s for four neighbours against 400
         *  candidates. */
        vector<std::string> neighbours_of (const std::string& bundle,
                                           float min_overlap = 0.15f,
                                           size_t max_count = 6);

        //! Directory holding the per-tract population probability maps, or empty.
        /*! Resolved from the MRVIEWPP_TRACT_PROB_ATLAS environment variable, then the
         *  MRViewTractProbAtlasPath config option, then the conventional places beside
         *  a directory atlas. */
        std::string population_directory ();

        //! Report, once, whether population maps were found and where.
        void announce_population_directory ();

        //! Population probability map for \a bundle, as a path; empty when there is none.
        /*! The maps are named in full ("Corticospinal_Tract_L") where the tract atlas
         *  uses abbreviations ("CST_L"), so this translates. Coverage is partial by
         *  nature - the HCP1065 release has 67 maps against the atlas's 102 bundles,
         *  and among the missing are ML, DRTT, DTT, NDTT, AR and the cingulum
         *  subdivisions - so an empty result is ordinary, not an error. */
        std::string population_map_path (const std::string& bundle);

        //! Every other bundle filed under the same atlas category as \a bundle.
        /*! From the catalogue alone - no footprints, no streamlines read - so it
         *  answers immediately and on the first run. Empty for a flat atlas with no
         *  categories, and for a bundle the atlas does not hold. */
        vector<std::string> category_siblings (const std::string& bundle);

        //! The bundles that should compete with \a bundle for streamlines.
        /*! The whole of its category, plus any territory neighbour from outside it.
         *  The category is taken whole rather than ranked and cut: bundles of one
         *  class are the ones confusable with each other, and a fixed set is
         *  predictable in a way that a top-N overlap list is not. */
        vector<std::string> competitors_for (const std::string& bundle,
                                             float min_overlap = 0.15f,
                                             size_t max_extra = 24);

        //! Load the atlas, one entry per named bundle, in MNI space.
        void load_bundles (std::map<std::string, vector<MR::DWI::Tractography::Streamline<float>>>& out);

        //! Apply \a transform to every vertex, in place.
        void transform_bundles (std::map<std::string, vector<MR::DWI::Tractography::Streamline<float>>>& bundles,
                                const transform_type& transform);

      }

    }
  }
}

#endif
