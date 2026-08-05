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

#ifndef __dwi_tractography_recognition_mdf_h__
#define __dwi_tractography_recognition_mdf_h__

#include <cmath>
#include <limits>

#include "types.h"
#include "dwi/tractography/streamline.h"

namespace MR
{
  namespace DWI
  {
    namespace Tractography
    {
      namespace Recognition
      {

        //! A streamline reduced to a fixed number of points: P rows of (x,y,z).
        /*! Comparing bundles means comparing many streamline pairs, and a pair
         *  comparison is only O(P) once both have the same number of points. */
        using FixedTrack = Eigen::Matrix<float, Eigen::Dynamic, 3>;


        //! Resample a streamline to \a num_points equally spaced by arc length.
        /*! The first and last vertices are preserved exactly. Streamlines with
         *  fewer than two vertices give an empty result. */
        void to_fixed (const Streamline<float>& in, size_t num_points, FixedTrack& out);


        //! Flip-invariant mean direct-flip distance between two fixed tracks, in mm.
        /*! This is the QuickBundles / RecoBundles metric: the mean point-to-point
         *  distance, evaluated both in the given order and with one track
         *  reversed, taking the smaller. The flip term is what makes it agnostic
         *  to the arbitrary direction in which a streamline was tracked.
         *
         *  Both tracks must have the same number of points. */
        inline float mdf_flip (const FixedTrack& a, const FixedTrack& b)
        {
          assert (a.rows() == b.rows());
          const Eigen::Index P = a.rows();
          float direct = 0.0f, flipped = 0.0f;
          for (Eigen::Index i = 0; i != P; ++i) {
            direct  += (a.row(i) - b.row(i)).norm();
            flipped += (a.row(i) - b.row(P-1-i)).norm();
          }
          return std::min (direct, flipped) / float (P);
        }


        //! True if \a b is closer to \a a when reversed.
        inline bool is_flipped (const FixedTrack& a, const FixedTrack& b)
        {
          assert (a.rows() == b.rows());
          const Eigen::Index P = a.rows();
          float direct = 0.0f, flipped = 0.0f;
          for (Eigen::Index i = 0; i != P; ++i) {
            direct  += (a.row(i) - b.row(i)).norm();
            flipped += (a.row(i) - b.row(P-1-i)).norm();
          }
          return flipped < direct;
        }


        //! Total length of a streamline, in mm.
        inline float track_length (const Streamline<float>& tck)
        {
          float length = 0.0f;
          for (size_t i = 1; i < tck.size(); ++i)
            length += (tck[i] - tck[i-1]).norm();
          return length;
        }

      }
    }
  }
}

#endif
