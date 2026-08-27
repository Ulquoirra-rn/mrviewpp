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

#ifndef __gui_mrview_mode_volume_h__
#define __gui_mrview_mode_volume_h__

#include "app.h"
#include "file/config.h"
#include "gui/mrview/mode/base.h"
#include "gui/opengl/transformation.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool { class View; }



      namespace Mode
      {

        class Volume : public Base
        { MEMALIGN(Volume)
          public:
            Volume () :
              Base (FocusContrast | MoveTarget | TiltRotate | ShaderTransparency | ShaderThreshold | ShaderClipping),
              volume_shader (*this) {
                static bool conf_read = false;
                if (!conf_read)
                  xray_strength = MR::File::Config::get_float ("MRViewVolumeXray", 0.0f);
                conf_read = true;
              }

            virtual void paint (Projection& projection);
            virtual void tilt_event ();

            //! How far to see through the render, 0 to 1.
            /*! Scales the image's contribution to the ray-cast and nothing else, so
             *  anything drawn before the volume - tracts, meshes - shows through in
             *  proportion, and overlays, which are composited inside the ray-cast,
             *  come through with it. At 1 the render is gone and only what is inside
             *  it remains; at 0 nothing changes.
             *
             *  Static because it is a property of how the scene is being read rather
             *  than of one mode object, and the mode is rebuilt whenever the view
             *  mode changes. */
            static float xray_strength;

          protected:
            GL::VertexBuffer volume_VB, volume_VI;
            GL::VertexArrayObject volume_VAO;
            GL::Texture depth_texture;
            vector<GL::vec4> clip;

            class Shader : public Displayable::Shader { MEMALIGN(Shader)
              public:
                Shader (const Volume& mode) : mode (mode), active_clip_planes (0), cliphighlight (true), clipintersectionmode (false) { }
                virtual std::string vertex_shader_source (const Displayable& object);
                virtual std::string fragment_shader_source (const Displayable& object);
                virtual bool need_update (const Displayable& object) const;
                virtual void update (const Displayable& object);
                const Volume& mode;
                size_t active_clip_planes;
                //! What the source was generated for, one entry per overlay.
                /*! Compared rather than trusted to a flag. The Overlay tool announces a
                 *  change by setting update_overlays on the Window's *current* mode,
                 *  which is not this one when Ortho paints a volume into its spare
                 *  quadrant - so the flag never arrives. First the count went unnoticed
                 *  and the overlay was collected every frame and composited by nothing;
                 *  then the colourmap went unnoticed and it drew in whatever map the
                 *  source happened to be built with, orange where the colourbar said
                 *  blue. Everything the source generation branches on is in here. The
                 *  clip planes were already handled by comparison; this now matches. */
                vector<uint32_t> overlay_state;

                //! The signature compared above: colourmap and the flags that alter the source.
                static vector<uint32_t> overlay_state_of (const Volume& mode);
                bool cliphighlight;
                bool clipintersectionmode;
            } volume_shader;

            Tool::View* get_view_tool () const;
            vector< std::pair<GL::vec4,bool> > get_active_clip_planes () const;
            vector<GL::vec4*> get_clip_planes_to_be_edited () const;
            bool get_cliphighlightstate () const;
            bool get_clipintersectionmodestate () const;
        };

      }
    }
  }
}

#endif





