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

#ifndef __gui_mrview_tool_roi_editor_undoentry_h__
#define __gui_mrview_tool_roi_editor_undoentry_h__

#include <array>
#include <atomic>
#include <cassert>

#include "types.h"

#include "gui/opengl/shader.h"
#include "gui/opengl/gl.h"


namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {


        class ROI_Item;


        struct ROI_UndoEntry { MEMALIGN(ROI_UndoEntry)

          // slab_slices is the number of slices the edit spans along the drawing
          // axis (1 = the traditional single-slice behaviour); the slab is centred
          // on the given slice and clipped to the volume.
          ROI_UndoEntry (ROI_Item&, int current_axis, int current_slice, int slab_slices = 1);
          ROI_UndoEntry (ROI_Item&);            // whole-volume entry: captures the current ROI as "before"
          ROI_UndoEntry (const ROI_UndoEntry&) = delete;
          ROI_UndoEntry (ROI_UndoEntry&&);
          ~ROI_UndoEntry();

          // For whole-volume entries: capture the current ROI as "after" (call after editing).
          void capture_after (ROI_Item&);

          ROI_UndoEntry& operator= (const ROI_UndoEntry&) = delete;
          ROI_UndoEntry& operator= (ROI_UndoEntry&&);

          void draw_line (ROI_Item&, const Eigen::Vector3f&, const Eigen::Vector3f&, const bool);
          void draw_thick_line (ROI_Item&, const Eigen::Vector3f&, const Eigen::Vector3f&, const bool, const float);
          void draw_circle (ROI_Item&, const Eigen::Vector3f&, const bool, const float);
          void draw_rectangle (ROI_Item&, const Eigen::Vector3f&, const Eigen::Vector3f&, const bool);
          void draw_fill (ROI_Item&, const Eigen::Vector3f&, const bool);

          void undo (ROI_Item& roi);
          void redo (ROI_Item& roi);
          void copy (ROI_Item& roi, ROI_UndoEntry& source);

          std::array<GLint,3> from, size;
          std::array<GLint,2> tex_size, slice_axes;
          GLint slab_axis = 2;    //!< the axis the edit spans; size[slab_axis] slices
          GLint slab_reference = 0;   //!< index within the slab of the drawn slice
          vector<GLubyte> before, after;

          //! Propagate this stroke through the slab.
          /*! Only the voxels the stroke actually changed on the drawn slice are
           *  copied to the other slices, so whatever was already drawn on them is
           *  preserved. A no-op for a single-slice edit. */
          void replicate_slab ();

          //! Offset into before/after of voxel (u,v) on slab slice \a s.
          size_t offset_of (GLint s, GLint u, GLint v) const {
            // slab_axis must be the one axis that is not in-plane, otherwise the
            // assignments below collide and leave an entry unset. Zero-initialise
            // as well, so that a future slip degrades to a wrong-but-in-bounds
            // index rather than a heap overflow.
            assert (slab_axis != slice_axes[0] && slab_axis != slice_axes[1]);
            std::array<GLint,3> idx = { { 0, 0, 0 } };
            idx[slab_axis] = s;
            idx[slice_axes[0]] = u;
            idx[slice_axes[1]] = v;
            return size_t (idx[0]) + size_t (size[0]) * (size_t (idx[1]) + size_t (size[1]) * size_t (idx[2]));
          }

          class Shared
          { MEMALIGN(Shared)
            public:
              Shared();
              ~Shared();
              GL::Shader::Program program;
              GL::VertexBuffer vertex_buffer;
              GL::VertexArrayObject vertex_array_object;
              void operator++ ();
              bool operator-- ();
            private:
              std::atomic<uint32_t> count;
          };
          static std::unique_ptr<Shared> shared;

        };





      }
    }
  }
}

#endif


