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

#ifndef __gui_mrview_mode_ortho_h__
#define __gui_mrview_mode_ortho_h__

#include <memory>

#include "app.h"
#include "gui/mrview/mode/slice.h"
#include "gui/mrview/mode/volume.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Mode
      {

        class Ortho : public Slice
        { MEMALIGN(Ortho)
            Q_OBJECT

          public:
            Ortho ();
            virtual void paint (Projection& projection);

            virtual void mouse_press_event ();
            virtual void slice_move_event (float x);
            virtual void panthrough_event ();
            virtual const Projection* get_current_projection () const;
            virtual void request_update_mode_gui (ModeGuiVisitor& visitor) const {
                 visitor.update_ortho_mode_gui(*this); }

            static bool show_as_row;
            //! Fill the empty quadrant of the 2x2 montage with a volume render.
            /*! The montage has three planes and four quadrants, so one is always
             *  blank. A volume render is the obvious thing to put there: it is the
             *  view the three slices cannot give, it needs no screen of its own, and
             *  it costs nothing when off.
             *
             *  Only in the 2x2 montage - laid out as a row there is no spare
             *  quadrant to fill. */
            static bool show_volume;

          public slots:
            void set_show_as_row_slot (bool state);
            void set_show_volume_slot (bool state);

          protected:
            //! Four: the three planes, and the volume pane in the spare quadrant.
            vector<Projection> projections;
            //! 0-2 for a plane, 3 for the volume pane, -1 for none.
            int current_plane;
            //! Built on first use, so a session that never turns it on never pays.
            /*! A second Mode::Base costs a Projection and a flag word - all the view
             *  state it reads lives on the Window - so this is a light object that
             *  happens to know how to draw a volume. */
            std::unique_ptr<Volume> volume_pane;
            GL::VertexBuffer frame_VB;
            GL::VertexArrayObject frame_VAO;
            GL::Shader::Program frame_program;
        };

      }
    }
  }
}

#endif




