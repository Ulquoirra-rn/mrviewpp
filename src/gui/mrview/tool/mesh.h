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

#ifndef __gui_mrview_tool_mesh_h__
#define __gui_mrview_tool_mesh_h__

#include "gui/mrview/tool/base.h"
#include "gui/color_button.h"
#include "gui/projection.h"
#include "gui/opengl/gl.h"
#include "gui/opengl/shader.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        class Mesh : public Base
        { MEMALIGN(Mesh)
            Q_OBJECT

          public:

            class Model;

            Mesh (Dock* parent);
            virtual ~Mesh ();

            void draw (const Projection& transform, bool is_3D, int axis, int slice) override;

          private slots:
            void mesh_open_slot ();
            void mesh_close_slot ();
            void hide_all_slot ();
            void toggle_shown_slot (const QModelIndex&, const QModelIndex&);
            void selection_changed_slot (const QItemSelection&, const QItemSelection&);
            void opacity_slot (int opacity);
            void colour_button_slot ();
            void wireframe_slot (bool);

          protected:
            Model* mesh_list_model;
            QListView* mesh_list_view;
            QPushButton* hide_all_button;
            QColorButton* colour_button;
            QSlider* opacity_slider;
            QCheckBox* wireframe_checkbox;

            // GL state, lazily compiled on first draw (needs a current GL context):
            GL::Shader::Program shader_program;
            bool shader_compiled;
            void compile_shader ();

            void add_meshes (vector<std::string>& list);
            void dropEvent (QDropEvent* event) override;
        };

      }
    }
  }
}

#endif
