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

#include "mrtrix.h"
#include "surface/mesh.h"
#include "gui/mrview/window.h"
#include "gui/mrview/tool/mesh.h"
#include "gui/dialog/file.h"
#include "gui/mrview/tool/list_model_base.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        // A single loaded surface mesh, owning its own GPU buffers.
        // Vertices are taken verbatim in scanner (real/RAS) space, which is the
        // same world space mrview's MVP maps; GIfTI/MZ3/VTK/OBJ/STL all resolve
        // to that space via Surface::Mesh.
        class MeshItem : public Displayable
        { MEMALIGN(MeshItem)
          public:
            MeshItem (const std::string& filename) :
                Displayable (filename),
                count (0)
            {
              MR::Surface::Mesh mesh (filename);
              if (!mesh.num_vertices())
                throw Exception ("mesh \"" + filename + "\" contains no vertices");
              if (!mesh.have_normals())
                mesh.calculate_normals();

              count = 3 * mesh.num_triangles();

              vector<float> vertices, normals;
              vertices.reserve (3 * mesh.num_vertices());
              normals.reserve (3 * mesh.num_vertices());
              for (size_t v = 0; v != mesh.num_vertices(); ++v) {
                for (size_t a = 0; a != 3; ++a) {
                  vertices.push_back (float (mesh.vert (v)[a]));
                  normals.push_back  (float (mesh.norm (v)[a]));
                }
              }
              vector<unsigned int> indices;
              indices.reserve (3 * mesh.num_triangles());
              for (size_t i = 0; i != mesh.num_triangles(); ++i)
                for (size_t v = 0; v != 3; ++v)
                  indices.push_back (mesh.tri (i)[v]);

              GL::Context::Grab context;
              GL::assert_context_is_current();

              vertex_buffer.gen();
              vertex_buffer.bind (gl::ARRAY_BUFFER);
              if (vertices.size())
                gl::BufferData (gl::ARRAY_BUFFER, vertices.size() * sizeof (float), &vertices[0], gl::STATIC_DRAW);

              normal_buffer.gen();
              normal_buffer.bind (gl::ARRAY_BUFFER);
              if (normals.size())
                gl::BufferData (gl::ARRAY_BUFFER, normals.size() * sizeof (float), &normals[0], gl::STATIC_DRAW);

              vertex_array_object.gen();
              vertex_array_object.bind();
              vertex_buffer.bind (gl::ARRAY_BUFFER);
              gl::EnableVertexAttribArray (0);
              gl::VertexAttribPointer (0, 3, gl::FLOAT, gl::FALSE_, 0, (void*)(0));
              normal_buffer.bind (gl::ARRAY_BUFFER);
              gl::EnableVertexAttribArray (1);
              gl::VertexAttribPointer (1, 3, gl::FLOAT, gl::FALSE_, 0, (void*)(0));

              index_buffer.gen();
              index_buffer.bind();
              if (indices.size())
                gl::BufferData (gl::ELEMENT_ARRAY_BUFFER, indices.size() * sizeof (unsigned int), &indices[0], gl::STATIC_DRAW);
              GL::assert_context_is_current();
            }

            ~MeshItem () {
              GL::Context::Grab context;
              vertex_buffer.clear();
              normal_buffer.clear();
              vertex_array_object.clear();
              index_buffer.clear();
            }

            void render () const {
              if (!count)
                return;
              GL::assert_context_is_current();
              vertex_buffer.bind (gl::ARRAY_BUFFER);
              normal_buffer.bind (gl::ARRAY_BUFFER);
              vertex_array_object.bind();
              index_buffer.bind();
              gl::DrawElements (gl::TRIANGLES, count, gl::UNSIGNED_INT, (void*)0);
              GL::assert_context_is_current();
            }

          private:
            GLsizei count;
            GL::VertexBuffer vertex_buffer, normal_buffer;
            GL::VertexArrayObject vertex_array_object;
            GL::IndexBuffer index_buffer;
        };



        class Mesh::Model : public ListModelBase
        { MEMALIGN(Mesh::Model)
          public:
            Model (QObject* parent) : ListModelBase (parent) { }

            void add_items (vector<std::string>& filenames) {
              // Distinct default colours, cycled, so successive meshes are
              // visually separable on load.
              static const GLubyte palette[][3] = {
                {230, 159,   0}, {  0, 158, 115}, { 86, 180, 233},
                {204, 121, 167}, {213,  94,   0}, {  0, 114, 178}, {240, 228,  66}
              };
              for (size_t i = 0; i < filenames.size(); ++i) {
                MeshItem* item = nullptr;
                try {
                  item = new MeshItem (filenames[i]);
                  const size_t c = items.size() % 7;
                  item->set_colour (std::array<GLubyte,3> { palette[c][0], palette[c][1], palette[c][2] });
                  item->show = true;
                  beginInsertRows (QModelIndex(), items.size(), items.size());
                  items.push_back (std::unique_ptr<Displayable> (item));
                  endInsertRows();
                } catch (Exception& e) {
                  delete item;
                  e.display();
                }
              }
            }

            MeshItem* get (QModelIndex& index) {
              return dynamic_cast<MeshItem*> (items[index.row()].get());
            }
        };



        Mesh::Mesh (Dock* parent) :
            Base (parent),
            shader_compiled (false)
        {
          VBoxLayout* main_box = new VBoxLayout (this);

          HBoxLayout* hlayout = new HBoxLayout;
          hlayout->setContentsMargins (0, 0, 0, 0);
          hlayout->setSpacing (0);

          QPushButton* open_button = new QPushButton (this);
          open_button->setToolTip (tr ("Open surface mesh"));
          open_button->setIcon (QIcon (":/open.svg"));
          connect (open_button, SIGNAL (clicked()), this, SLOT (mesh_open_slot ()));
          hlayout->addWidget (open_button, 1);

          QPushButton* close_button = new QPushButton (this);
          close_button->setToolTip (tr ("Close surface mesh"));
          close_button->setIcon (QIcon (":/close.svg"));
          connect (close_button, SIGNAL (clicked()), this, SLOT (mesh_close_slot ()));
          hlayout->addWidget (close_button, 1);

          hide_all_button = new QPushButton (this);
          hide_all_button->setToolTip (tr ("Hide all meshes"));
          hide_all_button->setIcon (QIcon (":/hide.svg"));
          hide_all_button->setCheckable (true);
          connect (hide_all_button, SIGNAL (clicked()), this, SLOT (hide_all_slot ()));
          hlayout->addWidget (hide_all_button, 1);

          main_box->addLayout (hlayout, 0);

          mesh_list_view = new QListView (this);
          mesh_list_view->setSelectionMode (QAbstractItemView::ExtendedSelection);
          mesh_list_view->setDragEnabled (true);
          mesh_list_view->viewport()->setAcceptDrops (true);
          mesh_list_view->setDropIndicatorShown (true);
          mesh_list_view->setDragDropMode (QAbstractItemView::InternalMove);
          mesh_list_view->setContextMenuPolicy (Qt::CustomContextMenu);
          mesh_list_model = new Model (this);
          mesh_list_view->setModel (mesh_list_model);
          main_box->addWidget (mesh_list_view, 1);

          connect (mesh_list_model, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
                   this, SLOT (toggle_shown_slot (const QModelIndex&, const QModelIndex&)));
          connect (mesh_list_view->selectionModel(),
                   SIGNAL (selectionChanged (const QItemSelection&, const QItemSelection&)),
                   this, SLOT (selection_changed_slot (const QItemSelection&, const QItemSelection&)));

          QGroupBox* display_box = new QGroupBox (tr ("Display"));
          main_box->addWidget (display_box);
          VBoxLayout* display_layout = new VBoxLayout;
          display_box->setLayout (display_layout);

          HBoxLayout* colour_layout = new HBoxLayout;
          colour_layout->addWidget (new QLabel (tr ("colour")), 0);
          colour_button = new QColorButton;
          connect (colour_button, SIGNAL (clicked()), this, SLOT (colour_button_slot ()));
          colour_layout->addWidget (colour_button, 1);
          display_layout->addLayout (colour_layout);

          display_layout->addWidget (new QLabel (tr ("opacity")));
          opacity_slider = new QSlider (Qt::Horizontal);
          opacity_slider->setRange (0, 1000);
          opacity_slider->setSliderPosition (1000);
          connect (opacity_slider, SIGNAL (valueChanged (int)), this, SLOT (opacity_slot (int)));
          display_layout->addWidget (opacity_slider);

          wireframe_checkbox = new QCheckBox (tr ("wireframe"));
          connect (wireframe_checkbox, SIGNAL (toggled (bool)), this, SLOT (wireframe_slot (bool)));
          display_layout->addWidget (wireframe_checkbox);

          HBoxLayout* checkall_layout = new HBoxLayout;
          QPushButton* check_all_button = new QPushButton (tr ("Check all"), this);
          check_all_button->setObjectName ("batchbtn");
          check_all_button->setToolTip (tr ("Show every mesh by checking its box"));
          connect (check_all_button, &QPushButton::clicked, this, [this]{ mesh_list_model->check_all(); window().updateGL(); });
          checkall_layout->addWidget (check_all_button, 1);
          QPushButton* uncheck_all_button = new QPushButton (tr ("Uncheck all"), this);
          uncheck_all_button->setObjectName ("batchbtn");
          uncheck_all_button->setToolTip (tr ("Hide every mesh by unchecking its box"));
          connect (uncheck_all_button, &QPushButton::clicked, this, [this]{ mesh_list_model->uncheck_all(); window().updateGL(); });
          checkall_layout->addWidget (uncheck_all_button, 1);
          main_box->addLayout (checkall_layout, 0);
        }



        Mesh::~Mesh ()
        {
          GL::Context::Grab context;
          if (shader_program)
            shader_program.clear();
        }



        void Mesh::compile_shader ()
        {
          GL::Shader::Vertex vertex_shader (
            "layout (location = 0) in vec3 vertexPosition;\n"
            "layout (location = 1) in vec3 vertexNormal;\n"
            "uniform mat4 MVP;\n"
            "uniform mat4 MV;\n"
            "out vec3 frag_normal;\n"
            "void main () {\n"
            "  gl_Position = MVP * vec4 (vertexPosition, 1.0);\n"
            "  frag_normal = mat3 (MV) * vertexNormal;\n"
            "}\n");

          // Two-sided headlight shading; no geometry shader (the geometry-shader
          // path is what crashes the macOS GL->Metal translation layer).
          GL::Shader::Fragment fragment_shader (
            "uniform vec3 surface_colour;\n"
            "uniform float surface_opacity;\n"
            "in vec3 frag_normal;\n"
            "out vec4 final_colour;\n"
            "void main () {\n"
            "  vec3 n = normalize (frag_normal);\n"
            "  float lambert = abs (n.z);\n"
            "  float intensity = 0.35 + 0.65 * lambert;\n"
            "  final_colour = vec4 (surface_colour * intensity, surface_opacity);\n"
            "}\n");

          shader_program.attach (vertex_shader);
          shader_program.attach (fragment_shader);
          shader_program.link();
          shader_compiled = true;
        }



        void Mesh::draw (const Projection& transform, bool /*is_3D*/, int /*axis*/, int /*slice*/)
        {
          if (hide_all_button->isChecked())
            return;

          bool any = false;
          for (size_t i = 0; i < mesh_list_model->items.size(); ++i)
            if (mesh_list_model->items[i] && mesh_list_model->items[i]->show) { any = true; break; }
          if (!any)
            return;

          GL::assert_context_is_current();
          if (!shader_compiled)
            compile_shader();

          shader_program.start();
          gl::UniformMatrix4fv (gl::GetUniformLocation (shader_program, "MVP"), 1, gl::FALSE_, transform.modelview_projection());
          gl::UniformMatrix4fv (gl::GetUniformLocation (shader_program, "MV"),  1, gl::FALSE_, transform.modelview());
          const GLint colour_loc  = gl::GetUniformLocation (shader_program, "surface_colour");
          const GLint opacity_loc = gl::GetUniformLocation (shader_program, "surface_opacity");

          const float opacity = opacity_slider->value() / 1000.0f;

          gl::Enable (gl::DEPTH_TEST);
          gl::DepthMask (opacity < 1.0f ? gl::FALSE_ : gl::TRUE_);
          if (opacity < 1.0f) {
            gl::Enable (gl::BLEND);
            gl::BlendFunc (gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
          } else {
            gl::Disable (gl::BLEND);
          }
          gl::PolygonMode (gl::FRONT_AND_BACK, wireframe_checkbox->isChecked() ? gl::LINE : gl::FILL);

          for (size_t i = 0; i < mesh_list_model->items.size(); ++i) {
            MeshItem* item = dynamic_cast<MeshItem*> (mesh_list_model->items[i].get());
            if (!item || !item->show)
              continue;
            const std::array<GLubyte,3>& c = item->colour;
            gl::Uniform3f (colour_loc, c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f);
            gl::Uniform1f (opacity_loc, opacity);
            item->render();
          }

          gl::PolygonMode (gl::FRONT_AND_BACK, gl::FILL);
          gl::DepthMask (gl::TRUE_);
          gl::Disable (gl::BLEND);
          shader_program.stop();
          GL::assert_context_is_current();
        }



        void Mesh::add_meshes (vector<std::string>& list)
        {
          mesh_list_model->add_items (list);
          window().updateGL();
        }



        void Mesh::get_session (nlohmann::json& node) const
        {
          vector<std::string> files;
          for (size_t i = 0; i < mesh_list_model->items.size(); ++i) {
            const Displayable* d = mesh_list_model->items[i].get();
            if (d)
              files.push_back (d->get_filename());
          }
          node = files;
        }



        void Mesh::set_session (const nlohmann::json& node)
        {
          if (!node.is_array())
            return;
          vector<std::string> files;
          for (const auto& f : node)
            files.push_back (f.get<std::string>());
          add_meshes (files);
        }



        void Mesh::mesh_open_slot ()
        {
          vector<std::string> list = Dialog::File::get_files (this,
              "Select surface meshes to open",
              "Surface meshes (*.vtk *.obj *.stl *.gii *.mz3 *.fs)");
          if (list.empty())
            return;
          add_meshes (list);
        }



        void Mesh::mesh_close_slot ()
        {
          QModelIndexList indexes = mesh_list_view->selectionModel()->selectedIndexes();
          while (!indexes.empty()) {
            mesh_list_model->remove_item (indexes.first());
            indexes = mesh_list_view->selectionModel()->selectedIndexes();
          }
          window().updateGL();
        }



        void Mesh::hide_all_slot ()
        {
          window().updateGL();
        }



        void Mesh::toggle_shown_slot (const QModelIndex& index, const QModelIndex& index2)
        {
          if (index.row() == index2.row()) {
            mesh_list_view->setCurrentIndex (index);
          } else {
            for (size_t i = 0; i < mesh_list_model->items.size(); ++i) {
              if (mesh_list_model->items[i]->show)
                mesh_list_view->setCurrentIndex (mesh_list_model->index (i, 0));
            }
          }
          window().updateGL();
        }



        void Mesh::selection_changed_slot (const QItemSelection&, const QItemSelection&)
        {
          QModelIndexList indices = mesh_list_view->selectionModel()->selectedIndexes();
          if (indices.empty())
            return;
          MeshItem* item = mesh_list_model->get (indices[0]);
          if (item) {
            const std::array<GLubyte,3>& c = item->colour;
            colour_button->setColor (QColor (c[0], c[1], c[2]));
          }
        }



        void Mesh::opacity_slot (int)
        {
          window().updateGL();
        }



        void Mesh::colour_button_slot ()
        {
          QModelIndexList indices = mesh_list_view->selectionModel()->selectedIndexes();
          if (indices.empty())
            return;
          const QColor colour = colour_button->color();
          for (int i = 0; i < indices.size(); ++i) {
            MeshItem* item = mesh_list_model->get (indices[i]);
            if (item)
              item->set_colour (colour);
          }
          window().updateGL();
        }



        void Mesh::wireframe_slot (bool)
        {
          window().updateGL();
        }



        void Mesh::dropEvent (QDropEvent* event)
        {
          const QMimeData* mimeData = event->mimeData();
          if (mimeData->hasUrls()) {
            vector<std::string> list;
            QList<QUrl> urlList = mimeData->urls();
            for (int i = 0; i < urlList.size(); ++i)
              list.push_back (urlList.at (i).path().toUtf8().constData());
            add_meshes (list);
            event->acceptProposedAction();
          }
        }


      }
    }
  }
}
