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

#include "gui/mrview/mode/ortho.h"

#include "mrtrix.h"
#include "gui/cursor.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Mode
      {

        bool Ortho::show_as_row = false;
        bool Ortho::show_volume = false;

        //CONF option: MRViewOrthoAsRow
        //CONF Display the 3 orthogonal views of the Ortho mode in a row,
        //CONF rather than as a 2x2 montage
        //CONF default: false

        //CONF option: MRViewOrthoVolume
        //CONF Fill the empty fourth quadrant of the Ortho mode's 2x2 montage with
        //CONF a volume render. No effect with MRViewOrthoAsRow set, which leaves no
        //CONF spare quadrant.
        //CONF default: false

        Ortho::Ortho () :
          projections (4, projection),
          current_plane (0) {
            static bool conf_read = false;
            if (!conf_read) {
              show_as_row = MR::File::Config::get_bool ("MRViewOrthoAsRow", false);
              show_volume = MR::File::Config::get_bool ("MRViewOrthoVolume", false);
            }
            conf_read = true;
          }


        void Ortho::paint (Projection& projection)
        {
          GL::assert_context_is_current();
          // set up OpenGL environment:
          gl::Disable (gl::BLEND);
          gl::Disable (gl::DEPTH_TEST);
          gl::DepthMask (gl::FALSE_);
          gl::ColorMask (gl::TRUE_, gl::TRUE_, gl::TRUE_, gl::TRUE_);

          const GLint w = show_as_row ? width()/3 : width()/2;
          const GLint h = show_as_row ? height() : height()/2;

          // Depth test state may have been altered after drawing each plane
          // so need to guarantee depth test is off for subsequent plane.
          // Ideally, state should be restored by callee but this is safer

          if (show_as_row)
            projections[0].set_viewport (window(), 0, 0, w, h);
          else
            projections[0].set_viewport (window(), w, h, w, h);
          draw_plane (0, slice_shader, projections[0]);

          gl::Disable (gl::DEPTH_TEST);
          if (show_as_row)
            projections[1].set_viewport (window(), w, 0, w, h);
          else
            projections[1].set_viewport (window(), 0, h, w, h);
          draw_plane (1, slice_shader, projections[1]);

          gl::Disable (gl::DEPTH_TEST);
          if (show_as_row)
            projections[2].set_viewport (window(), 2*w, 0, w, h);
          else
            projections[2].set_viewport (window(), 0, 0, w, h);
          draw_plane (2, slice_shader, projections[2]);

          // The fourth quadrant. Bottom-right in the montage, and the one the three
          // planes leave empty; as a row there is no spare quadrant, so nothing to do.
          if (show_volume && !show_as_row && image()) {
            gl::Disable (gl::DEPTH_TEST);
            projections[3].set_viewport (window(), w, 0, w, h);
            if (!volume_pane)
              volume_pane.reset (new Volume);
            volume_pane->paint (projections[3]);
            // Volume::paint leaves depth testing and blending set up for a 3D scene;
            // the frame lines drawn below are 2D and would be depth-tested against it.
            gl::Disable (gl::DEPTH_TEST);
            gl::Disable (gl::BLEND);
            gl::DepthMask (gl::FALSE_);
          }

          projection.set_viewport (window());

          GL::mat4 MV = GL::identity();
          GL::mat4 P = GL::ortho (0, width(), 0, height(), -1.0, 1.0);
          projection.set (MV, P);

          gl::Disable (gl::DEPTH_TEST);

          if (!frame_VB || !frame_VAO) {
            frame_VB.gen();
            frame_VAO.gen();

            frame_VB.bind (gl::ARRAY_BUFFER);
            frame_VAO.bind();

            gl::EnableVertexAttribArray (0);
            gl::VertexAttribPointer (0, 2, gl::FLOAT, gl::FALSE_, 0, (void*)0);

            GLfloat data [] = {
              -1.0f, 0.0f,
              1.0f, 0.0f,
              0.0f, -1.0f,
              0.0f, 1.0f
            };

            GLfloat data_row [] = {
              -1.0f/3.0f, -1.0f,
              -1.0f/3.0f, 1.0f,
              1.0f/3.0f, -1.0f,
              1.0f/3.0f, 2.0f
            };
            gl::BufferData (gl::ARRAY_BUFFER, sizeof(data), ( show_as_row ? data_row : data), gl::STATIC_DRAW);
          }
          else
            frame_VAO.bind();

          if (!frame_program) {
            GL::Shader::Vertex vertex_shader (
                "layout(location=0) in vec2 pos;\n"
                "void main () {\n"
                "  gl_Position = vec4 (pos, 0.0, 1.0);\n"
                "}\n");
            GL::Shader::Fragment fragment_shader (
                "out vec3 color;\n"
                "void main () {\n"
                "  color = vec3 (0.1);\n"
                "}\n");
            frame_program.attach (vertex_shader);
            frame_program.attach (fragment_shader);
            frame_program.link();
          }

          frame_program.start();
          gl::DrawArrays (gl::LINES, 0, 4);
          frame_program.stop();

          gl::Enable (gl::DEPTH_TEST);
          GL::assert_context_is_current();
        }





        const Projection* Ortho::get_current_projection () const
        {
          // 3 is the volume pane, which is a projection like any other - returning it
          // is what lets the mouse work in that quadrant at all.
          if (current_plane < 0 || current_plane > 3)
            return NULL;
          if (current_plane == 3 && !(show_volume && !show_as_row))
            return NULL;
          return &projections[current_plane];
        }



        void Ortho::mouse_press_event ()
        {
          const int x = window().mouse_position().x();
          const int y = window().mouse_position().y();

          if (show_as_row) {
            const GLint w = width()/3;
            if (x < w)
              current_plane = 0;
            else if (x < 2*w)
              current_plane = 1;
            else
              current_plane = 2;
          }
          else {
            const GLint w = width()/2;
            const GLint h = height()/2;
            // Mouse coordinates here are bottom-up, matching the GL viewports above:
            // y < h is the lower half. The lower-right quadrant is the spare one, so
            // it is the volume pane when that is on and nothing when it is not.
            if (x < w)
              current_plane = y < h ? 2 : 1;
            else
              current_plane = y < h ? (show_volume ? 3 : -1) : 0;
          }
        }




        void Ortho::slice_move_event (float x)
        {
          const Projection* proj = get_current_projection();
          if (!proj)
            return;

          if (window().active_camera_interactor() && window().active_camera_interactor()->slice_move_event(*proj, x))
            return;

          const auto &header = image()->header();
          float increment = snap_to_image() ?
            x * header.spacing (current_plane) :
            x * std::pow (header.spacing(0) * header.spacing(1) * header.spacing(2), 1/3.f);
          auto move = get_through_plane_translation (increment, *proj);

          set_focus (focus() + move);
          updateGL();
        }


        void Ortho::panthrough_event ()
        {
          const Projection* proj = get_current_projection();
          if (!proj)
            return;

          if (window().active_camera_interactor() && window().active_camera_interactor()->panthrough_event (*proj))
            return;

          auto move = get_through_plane_translation_FOV (window().mouse_displacement().y(), *proj);

          set_focus (focus() + move);
          updateGL();
        }


        void Ortho::set_show_as_row_slot (bool state)
        {
          GL::Context::Grab context;
          show_as_row = state;
          frame_VB.clear();
          // Laid out as a row there is no fourth quadrant, so the volume toggle has
          // nothing to act on.
          window().refresh_volume_pane_action();
          updateGL();
        }



        void Ortho::set_show_volume_slot (bool state)
        {
          GL::Context::Grab context;
          show_volume = state;
          // Dropped rather than kept: its textures and buffers are the expensive part
          // of it, and a user who turns this off is not about to pay for them.
          if (!state)
            volume_pane.reset();
          updateGL();
        }


      }
    }
  }
}



