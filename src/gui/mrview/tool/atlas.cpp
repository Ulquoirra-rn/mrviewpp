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

#include <map>

#include "header.h"
#include "image.h"
#include "transform.h"
#include "connectome/lut.h"

#include "gui/mrview/window.h"
#include "gui/mrview/mode/base.h"
#include "gui/mrview/gui_image.h"
#include "gui/mrview/tool/atlas.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLineEdit>
#include "gui/mrview/tool/list_model_base.h"
#include "gui/dialog/file.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        // Slice shader for label volumes: the 3D texture holds a compact region
        // index per voxel (sampled nearest-neighbour so indices are never blended),
        // and `lut` is a 1-row RGB palette texture indexed by that region index.
        // Regions that are ticked in the panel - flagged by the alpha channel of
        // their palette entry - are drawn at full opacity, as are the regions
        // matching `focus` (crosshair) and `hover` (mouse); every other region is
        // drawn at `base_alpha * dim_alpha`.
        class Atlas::Shader : public Displayable::Shader
        { NOMEMALIGN
          public:
            std::string vertex_shader_source (const Displayable&) override
            {
              return
                "layout(location = 0) in vec3 vertpos;\n"
                "layout(location = 1) in vec3 texpos;\n"
                "uniform mat4 MVP;\n"
                "out vec3 texcoord;\n"
                "void main() {\n"
                "  gl_Position = MVP * vec4 (vertpos, 1);\n"
                "  texcoord = texpos;\n"
                "}\n";
            }

            std::string fragment_shader_source (const Displayable&) override
            {
              return
                "uniform sampler3D tex;\n"
                "uniform sampler2D lut;\n"
                "uniform int focus;\n"
                "uniform int hover;\n"
                "uniform float base_alpha;\n"
                "uniform float dim_alpha;\n"
                "in vec3 texcoord;\n"
                "out vec4 color;\n"
                "void main() {\n"
                "  if (texcoord.s < 0.0 || texcoord.s > 1.0 ||\n"
                "      texcoord.t < 0.0 || texcoord.t > 1.0 ||\n"
                "      texcoord.p < 0.0 || texcoord.p > 1.0) discard;\n"
                "  float v = texture (tex, texcoord.stp).r;\n"
                "  int index = int (v + 0.5);\n"
                "  if (index <= 0) discard;\n"
                "  vec4 entry = texelFetch (lut, ivec2 (index, 0), 0);\n"
                "  vec3 rgb = entry.rgb;\n"
                // The palette's alpha channel is not opacity: it is the region's tick
                // in the panel's list. Carrying the selection there costs no uniform
                // and no second texture, and it is a set rather than one index.
                "  bool is_active = (entry.a > 0.5) || (index == focus) || (index == hover);\n"
                "  float a = is_active ? base_alpha : base_alpha * dim_alpha;\n"
                "  if (a <= 0.0) discard;\n"
                "  color = vec4 (rgb, a);\n"
                "}\n";
            }

            // The source above depends on nothing that can change at runtime, so
            // the program only ever needs compiling once.
            bool need_update (const Displayable&) const override { return false; }
        };



        // A single loaded atlas: a volume of compact region indices (rendered
        // through Atlas::Shader) plus the label names, per-label centroids and the
        // index<->label mapping used for the crosshair/hover read-out and jumping.
        //
        // The indices are held in a plain float array owned by this class rather
        // than an MR::Image scratch buffer: MRView::Image reaches its data through
        // MR::Image<cfloat>, and Image<>::Buffer::get_data_pointer() hands back the
        // raw pointer for *any* scratch image without checking the datatype, so
        // writing through such an accessor stores 8-byte complex values into a
        // 4-byte float buffer (every other voxel, and 2x past the end of the
        // allocation). Owning the array also halves the memory and skips a copy on
        // texture upload.
        class Atlas::Item : public ImageBase
        { MEMALIGN(Atlas::Item)
          public:
            Item (MR::Header&& grid_header, const std::string& label_path, const MR::Connectome::LUT& lut) :
                ImageBase (std::move (grid_header)),
                focus_index (0),
                hover_index (0),
                dim_factor (0.4f),
                texture_dirty (true)
            {
              auto labels_img = MR::Image<float>::open (label_path);
              const MR::Transform T (labels_img);

              std::map<uint32_t, std::array<float,3>> lut_colours;
              for (const auto& entry : lut) {
                const uint32_t label = entry.first;
                names[label] = entry.second.get_name();
                const auto& c = entry.second.get_colour();
                // Pure black is taken as "this LUT carries no colour", not as a
                // colour. Two of MRtrix's five LUT formats - the basic one (index +
                // name) and AAL (two names + index) - have no colour columns at all,
                // and LUT_node then defaults every entry to (0,0,0). Honouring that
                // literally paints every region black, on a black background, in the
                // slice view, the volume render and every region swatch at once; the
                // atlas looks unloaded rather than uncoloured. Falling through to the
                // per-label fallback below gives such a LUT the same distinct colours
                // a label absent from the LUT already gets.
                if (c[0] || c[1] || c[2])
                  lut_colours[label] = { c[0]/255.0f, c[1]/255.0f, c[2]/255.0f };
              }

              // Index 0 is background; its palette entry is never sampled.
              index_to_label.push_back (0);
              palette.assign (4, 0.0f);

              std::map<uint32_t, std::pair<Eigen::Vector3d, size_t>> accum;

              const ssize_t nx = header().size(0), ny = header().size(1), nz = header().size(2);
              indices.assign (nx*ny*nz, 0.0f);

              for (ssize_t z = 0; z != nz; ++z) { labels_img.index(2) = z;
                for (ssize_t y = 0; y != ny; ++y) { labels_img.index(1) = y;
                  for (ssize_t x = 0; x != nx; ++x) { labels_img.index(0) = x;
                    const float raw = labels_img.value();
                    if (!std::isfinite (raw) || raw < 0.5f)
                      continue;
                    const uint32_t label = uint32_t (std::round (raw));
                    size_t index;
                    const auto it = label_to_index.find (label);
                    if (it != label_to_index.end()) {
                      index = it->second;
                    } else {
                      index = index_to_label.size();
                      label_to_index[label] = index;
                      index_to_label.push_back (label);
                      const auto c = lut_colours.find (label);
                      // labels absent from the LUT get a deterministic visible colour
                      const std::array<float,3> rgb = c != lut_colours.end() ? c->second
                        : std::array<float,3> { ((label*97)%256)/255.0f, ((label*57)%256)/255.0f, ((label*131)%256)/255.0f };
                      palette.push_back (rgb[0]);
                      palette.push_back (rgb[1]);
                      palette.push_back (rgb[2]);
                      palette.push_back (0.0f);   // alpha = "ticked", initially not
                    }
                    indices[x + nx*(y + ny*z)] = float (index);
                    auto& a = accum[label];
                    a.first += T.voxel2scanner * Eigen::Vector3d (x, y, z);
                    a.second += 1;
                  }
                }
              }

              for (const auto& kv : accum) {
                if (kv.second.second)
                  centroids[kv.first] = (kv.second.first / double (kv.second.second)).cast<float>();
              }

              set_interpolate (false);
              value_min = 0.0f;
              value_max = float (index_to_label.size());
              min_max_set();
              alpha = 1.0f;
            }

            ~Item () {
              GL::Context::Grab context;
              lut_texture.clear();
              shader.clear();
            }

            void update_texture3D () override
            {
              bind();
              if (!texture_dirty)
                return;
              format = gl::RED;
              internal_format = gl::R32F;
              type = gl::FLOAT;
              allocate();
              value_min = 0.0f;
              value_max = float (index_to_label.size());
              min_max_set();
              upload_data ({ { 0, 0, 0 } },
                           { { header().size(0), header().size(1), header().size(2) } },
                           indices.data());
              texture_dirty = false;
            }

            // Never used: the atlas is always drawn through render_atlas() below,
            // which goes via the 3D texture. Implemented rather than asserted so
            // that an inherited render2D() call cannot corrupt GL state.
            void update_texture2D (const int plane, const int slice) override
            {
              if (!texture2D[plane])
                texture2D[plane].gen (gl::TEXTURE_3D);
              texture2D[plane].bind();
              gl::PixelStorei (gl::UNPACK_ALIGNMENT, 1);
              texture2D[plane].set_interp (interpolation);
              if (tex_positions[plane] == slice)
                return;
              tex_positions[plane] = slice;

              int x, y;
              get_axes (plane, x, y);
              const ssize_t xsize = header().size(x), ysize = header().size(y);
              vector<float> data (xsize*ysize, 0.0f);
              if (slice >= 0 && slice < header().size (plane)) {
                std::array<ssize_t,3> v;
                v[plane] = slice;
                for (v[y] = 0; v[y] != ysize; ++v[y])
                  for (v[x] = 0; v[x] != xsize; ++v[x])
                    data[v[x] + v[y]*xsize] = index_at_voxel (v[0], v[1], v[2]);
              }
              gl::TexImage3D (gl::TEXTURE_3D, 0, gl::R32F, xsize, ysize, 1, 0, gl::RED, gl::FLOAT, data.data());
            }

            // Compact region index at a scanner-space point (0 = outside / background).
            size_t region_index_at (const Eigen::Vector3f& world) const
            {
              const Eigen::Vector3f v = scanner2voxel() * world;
              return size_t (index_at_voxel (std::lround (v[0]), std::lround (v[1]), std::lround (v[2])));
            }

            uint32_t label_of (size_t index) const {
              return index < index_to_label.size() ? index_to_label[index] : 0;
            }

            size_t index_of (uint32_t label) const {
              const auto it = label_to_index.find (label);
              return it == label_to_index.end() ? 0 : it->second;
            }

            std::string name_of_label (uint32_t label) const {
              const auto it = names.find (label);
              return it == names.end() ? ("label " + str(label)) : it->second;
            }

            std::array<float,3> colour_of_index (size_t index) const {
              if (!index || 4*index+2 >= palette.size())
                return { 0.0f, 0.0f, 0.0f };
              return { palette[4*index], palette[4*index+1], palette[4*index+2] };
            }

            void render_atlas (const Projection& projection, float depth)
            {
              update_texture3D();
              update_lut_texture();

              shader.start (*this);
              projection.set (shader);

              gl::Uniform1i (gl::GetUniformLocation (shader, "tex"), 0);
              gl::Uniform1i (gl::GetUniformLocation (shader, "lut"), 1);
              gl::Uniform1i (gl::GetUniformLocation (shader, "focus"), int (focus_index));
              gl::Uniform1i (gl::GetUniformLocation (shader, "hover"), int (hover_index));
              gl::Uniform1f (gl::GetUniformLocation (shader, "base_alpha"), alpha);
              gl::Uniform1f (gl::GetUniformLocation (shader, "dim_alpha"), dim_factor);

              gl::ActiveTexture (gl::TEXTURE1);
              lut_texture.bind();
              gl::ActiveTexture (gl::TEXTURE0);
              texture().bind();

              set_vertices_for_slice_render (projection, depth);
              draw_vertices();
              shader.stop();
            }

            //! Binary mask of one region, on the atlas grid.
            MR::Image<bool> region_mask (uint32_t label) const
            {
              const auto it = label_to_index.find (label);
              if (it == label_to_index.end())
                throw Exception ("no region with label " + str(label) + " in atlas \"" + label_path + "\"");
              const float wanted = float (it->second);

              MR::Header H (header());
              H.ndim() = 3;
              H.datatype() = MR::DataType::Bit;
              auto out = MR::Image<bool>::scratch (H, name_of_label (label));
              const ssize_t nx = H.size(0), ny = H.size(1);
              for (auto l = MR::Loop (0, 3) (out); l; ++l)
                out.value() = (indices[out.index(0) + nx*(out.index(1) + ny*out.index(2))] == wanted);
              return out;
            }

            //! Is this region ticked in the panel's list?
            bool is_checked (size_t index) const {
              return index && 4*index+3 < palette.size() && palette[4*index+3] > 0.5f;
            }

            //! Tick or untick a region. Only the palette changes, so both the slice
            //! shader and the volume ray-cast pick it up from a texture they already
            //! sample - no new uniform, and no per-region work in either.
            void set_checked (size_t index, bool on) {
              if (!index || 4*index+3 >= palette.size())
                return;
              const float wanted = on ? 1.0f : 0.0f;
              if (palette[4*index+3] == wanted)
                return;
              palette[4*index+3] = wanted;
              lut_dirty = true;
            }

            size_t checked_count () const {
              size_t n = 0;
              for (size_t i = 1; 4*i+3 < palette.size(); ++i)
                if (palette[4*i+3] > 0.5f) ++n;
              return n;
            }

            size_t focus_index;   // ROI under the crosshair: stays active
            size_t hover_index;    // ROI under the mouse: active while hovered
            float dim_factor;
            std::map<uint32_t, std::string> names;
            std::map<uint32_t, Eigen::Vector3f> centroids;
            std::string label_path, lut_path;   // retained for session save

            //! Hand this atlas to a 3D renderer: index volume, palette, and the
            //! numbers that decide a region's opacity.
            /*! Public because the volume renderer composites it directly rather than
             *  calling render_atlas(); it cannot use the slice shader, whose palette
             *  lookup has no counterpart in a ray-cast. */
            Mode::Base::Atlas3D as_3D ()
            {
              update_texture3D();
              update_lut_texture();
              return { this, GLuint (texture()), GLuint (lut_texture),
                       int (focus_index), int (hover_index), alpha, dim_factor };
            }

          private:
            void update_lut_texture ()
            {
              if (lut_texture && !lut_dirty)
                return;
              if (!lut_texture)
                lut_texture.gen (gl::TEXTURE_2D, gl::NEAREST);
              lut_texture.bind();
              gl::PixelStorei (gl::UNPACK_ALIGNMENT, 1);
              gl::TexImage2D (gl::TEXTURE_2D, 0, gl::RGBA32F,
                  GLsizei (palette.size()/4), 1, 0, gl::RGBA, gl::FLOAT, palette.data());
              lut_dirty = false;
            }

            float index_at_voxel (ssize_t x, ssize_t y, ssize_t z) const
            {
              if (x < 0 || y < 0 || z < 0 ||
                  x >= header().size(0) || y >= header().size(1) || z >= header().size(2))
                return 0.0f;
              return indices[x + header().size(0)*(y + header().size(1)*z)];
            }

            Atlas::Shader shader;
            GL::Texture lut_texture;
            vector<float> indices;              // compact region index per voxel, x fastest
            vector<float> palette;              // RGBA per compact index, row-major
            vector<uint32_t> index_to_label;
            std::map<uint32_t, size_t> label_to_index;
            bool texture_dirty;
            bool lut_dirty = true;
        };



        // Offers every region of every loaded atlas as a tracking ROI.
        class Atlas::Regions : public RegionProvider
        { NOMEMALIGN
          public:
            Regions (Atlas& tool) : tool (tool) { }

            std::string provider_name () const override { return "Atlas"; }

            void list_regions (vector<RegionRef>& out) const override;
            MR::Image<bool> get_region_mask (const RegionRef&) const override;
            bool resolve (const std::string& key, RegionRef&) const override;

          private:
            Atlas& tool;
            // key is "atlas:<label volume path>#<label value>", so it survives a
            // session reload as long as the same atlas is loaded again.
            static std::string make_key (const std::string& path, uint32_t label) {
              return "atlas:" + path + "#" + str(label);
            }
            Item* item_for (const RegionRef&, uint32_t& label) const;
        };



        class Atlas::Model : public ListModelBase
        { MEMALIGN(Atlas::Model)
          public:
            Model (QObject* parent) : ListModelBase (parent) { }

            void add_item (const std::string& label_path, const std::string& lut_path) {
              Item* item = nullptr;
              try {
                MR::Connectome::LUT lut (lut_path);
                MR::Header grid_hdr = MR::Header::open (label_path);
                grid_hdr.ndim() = 3;

                item = new Item (std::move (grid_hdr), label_path, lut);
                item->label_path = label_path;
                item->lut_path = lut_path;
                // Colours come from the palette texture, not from a colourmap, and
                // opacity is decided per region in the shader; none of mrview's
                // intensity windowing / thresholding applies here.
                item->set_allowed_features (false, false, false);
                item->show = true;

                beginInsertRows (QModelIndex(), items.size(), items.size());
                items.push_back (std::unique_ptr<Displayable> (item));
                endInsertRows();
              } catch (Exception& e) {
                delete item;
                e.display();
              }
            }

            Item* get (QModelIndex& index) {
              return dynamic_cast<Item*> (items[index.row()].get());
            }
        };



        Atlas::Atlas (Dock* parent) :
            Base (parent),
            syncing_region_list (false)
        {
          VBoxLayout* main_box = new VBoxLayout (this);

          HBoxLayout* hlayout = new HBoxLayout;
          hlayout->setContentsMargins (0, 0, 0, 0);
          hlayout->setSpacing (0);

          QPushButton* open_button = new QPushButton (this);
          open_button->setToolTip (tr ("Open atlas (label volume + lookup table)"));
          open_button->setIcon (QIcon (":/open.svg"));
          connect (open_button, SIGNAL (clicked()), this, SLOT (atlas_open_slot ()));
          hlayout->addWidget (open_button, 1);

          QPushButton* close_button = new QPushButton (this);
          close_button->setToolTip (tr ("Close atlas"));
          close_button->setIcon (QIcon (":/close.svg"));
          connect (close_button, SIGNAL (clicked()), this, SLOT (atlas_close_slot ()));
          hlayout->addWidget (close_button, 1);

          hide_all_button = new QPushButton (this);
          hide_all_button->setToolTip (tr ("Hide all atlases"));
          hide_all_button->setIcon (QIcon (":/hide.svg"));
          hide_all_button->setCheckable (true);
          connect (hide_all_button, SIGNAL (clicked()), this, SLOT (hide_all_slot ()));
          hlayout->addWidget (hide_all_button, 1);

          main_box->addLayout (hlayout, 0);

          // Directly under the open/close row, so the batch show/hide controls sit
          // with the other list-wide actions rather than at the foot of the panel.
          HBoxLayout* checkall_layout = new HBoxLayout;
          QPushButton* check_all_button = new QPushButton (tr ("Check all"), this);
          check_all_button->setObjectName ("batchbtn");
          check_all_button->setToolTip (tr ("Show every atlas by checking its box"));
          connect (check_all_button, &QPushButton::clicked, this, [this]{ atlas_list_model->check_all(); window().updateGL(); });
          checkall_layout->addWidget (check_all_button, 1);
          QPushButton* uncheck_all_button = new QPushButton (tr ("Uncheck all"), this);
          uncheck_all_button->setObjectName ("batchbtn");
          uncheck_all_button->setToolTip (tr ("Hide every atlas by unchecking its box"));
          connect (uncheck_all_button, &QPushButton::clicked, this, [this]{ atlas_list_model->uncheck_all(); window().updateGL(); });
          checkall_layout->addWidget (uncheck_all_button, 1);
          main_box->addLayout (checkall_layout, 0);

          atlas_list_view = new QListView (this);
          atlas_list_view->setSelectionMode (QAbstractItemView::SingleSelection);
          atlas_list_view->setTextElideMode (Qt::ElideLeft);
          atlas_list_model = new Model (this);
          atlas_list_view->setModel (atlas_list_model);
          main_box->addWidget (atlas_list_view, 1);

          connect (atlas_list_model, SIGNAL (dataChanged (const QModelIndex&, const QModelIndex&)),
                   this, SLOT (toggle_shown_slot (const QModelIndex&, const QModelIndex&)));
          connect (atlas_list_view->selectionModel(),
                   SIGNAL (selectionChanged (const QItemSelection&, const QItemSelection&)),
                   this, SLOT (selection_changed_slot (const QItemSelection&, const QItemSelection&)));

          QGroupBox* display_box = new QGroupBox (tr ("Display"));
          main_box->addWidget (display_box);
          VBoxLayout* display_layout = new VBoxLayout;
          display_box->setLayout (display_layout);
          display_layout->addWidget (new QLabel (tr ("selected region opacity")));
          opacity_slider = new QSlider (Qt::Horizontal);
          opacity_slider->setRange (0, 1000);
          opacity_slider->setSliderPosition (1000);
          connect (opacity_slider, SIGNAL (valueChanged (int)), this, SLOT (opacity_slot (int)));
          display_layout->addWidget (opacity_slider);
          display_layout->addWidget (new QLabel (tr ("other regions opacity")));
          dim_slider = new QSlider (Qt::Horizontal);
          dim_slider->setRange (0, 1000);
          dim_slider->setSliderPosition (400);
          connect (dim_slider, SIGNAL (valueChanged (int)), this, SLOT (dim_slot (int)));
          display_layout->addWidget (dim_slider);

          QGroupBox* region_box = new QGroupBox (tr ("Active regions"));
          main_box->addWidget (region_box);
          VBoxLayout* region_box_layout = new VBoxLayout;
          region_box->setLayout (region_box_layout);
          region_box_layout->addWidget (new QLabel (tr ("crosshair:")));
          focus_region_label = new QLabel ("—");
          focus_region_label->setWordWrap (true);
          region_box_layout->addWidget (focus_region_label);
          region_box_layout->addWidget (new QLabel (tr ("hover:")));
          hover_region_label = new QLabel ("—");
          hover_region_label->setWordWrap (true);
          region_box_layout->addWidget (hover_region_label);

          QGroupBox* list_box = new QGroupBox (tr ("Regions (tick to show, double-click to jump)"));
          main_box->addWidget (list_box, 1);
          VBoxLayout* list_box_layout = new VBoxLayout;
          list_box->setLayout (list_box_layout);
          region_filter = new QLineEdit (this);
          make_search_box (region_filter, tr ("Search regions by name"));
          connect (region_filter, SIGNAL (textChanged (const QString&)),
                   this, SLOT (region_filter_slot (const QString&)));
          list_box_layout->addWidget (region_filter);
          region_list = new QListWidget (this);
          connect (region_list, SIGNAL (itemDoubleClicked (QListWidgetItem*)),
                   this, SLOT (region_activated_slot (QListWidgetItem*)));
          connect (region_list, SIGNAL (itemChanged (QListWidgetItem*)),
                   this, SLOT (region_check_slot (QListWidgetItem*)));
          list_box_layout->addWidget (region_list);
          QPushButton* clear_ticks = new QPushButton (tr ("untick all"), this);
          connect (clear_ticks, SIGNAL (clicked()), this, SLOT (clear_region_ticks_slot ()));
          list_box_layout->addWidget (clear_ticks);

          connect (&window(), SIGNAL (focusChanged()), this, SLOT (focus_changed_slot ()));
          connect (&window(), SIGNAL (hoverChanged()), this, SLOT (hover_changed_slot ()));
        }



        Atlas::Item* Atlas::current_item ()
        {
          QModelIndexList indices = atlas_list_view->selectionModel()->selectedIndexes();
          if (!indices.empty())
            return atlas_list_model->get (indices[0]);
          if (atlas_list_model->items.size())
            return dynamic_cast<Item*> (atlas_list_model->items[0].get());
          return nullptr;
        }



        void Atlas::draw (const Projection& projection, bool is_3D, int, int)
        {
          // In 3D the atlas is not drawn here: it is handed to whichever mode is
          // painting, which composites it into its ray-cast the way it does overlays.
          // The colours cannot come from an intensity colourmap - these voxels are
          // region indices - so the palette texture goes with it.
          if (is_3D) {
            for (int i = 0; i < atlas_list_model->rowCount(); ++i) {
              if (!atlas_list_model->items[i]->show || hide_all_button->isChecked())
                continue;
              Item* atlas = dynamic_cast<Item*> (atlas_list_model->items[i].get());
              if (!atlas)
                continue;
              Mode::Base::painter()->atlases_for_3D.push_back (atlas->as_3D());
            }
            return;
          }

          GL::assert_context_is_current();
          gl::Enable (gl::BLEND);
          gl::Disable (gl::DEPTH_TEST);
          gl::DepthMask (gl::FALSE_);
          gl::ColorMask (gl::TRUE_, gl::TRUE_, gl::TRUE_, gl::TRUE_);
          gl::BlendFunc (gl::SRC_ALPHA, gl::ONE_MINUS_SRC_ALPHA);
          gl::BlendEquation (gl::FUNC_ADD);

          for (int i = 0; i < atlas_list_model->rowCount(); ++i) {
            if (atlas_list_model->items[i]->show && !hide_all_button->isChecked()) {
              Item* atlas = dynamic_cast<Item*> (atlas_list_model->items[i].get());
              if (atlas)
                atlas->render_atlas (projection, projection.depth_of (window().focus()));
            }
          }

          gl::Disable (gl::BLEND);
          gl::Enable (gl::DEPTH_TEST);
          gl::DepthMask (gl::TRUE_);
          GL::assert_context_is_current();
        }



        void Atlas::atlas_open_slot ()
        {
          // One dialog naming both files, rather than two bare file pickers in
          // sequence: an atlas needs a label volume *and* a lookup table, and being
          // shown a file browser with no explanation of which is being asked for -
          // twice - is not a fair way to find that out.
          QDialog dialog (this);
          dialog.setWindowTitle (tr ("Open atlas"));
          QVBoxLayout* layout = new QVBoxLayout (&dialog);

          QLabel* explanation = new QLabel (tr (
              "An atlas needs two files:\n\n"
              "  \u2022  a label volume - an image whose voxel values are region indices\n"
              "  \u2022  a lookup table - text mapping those indices to names and colours\n\n"
              "FreeSurfer, ITK-SNAP, AAL, MRtrix and basic lookup tables are all read."));
          explanation->setWordWrap (true);
          layout->addWidget (explanation);

          QGridLayout* grid = new QGridLayout;
          layout->addLayout (grid);
          QLineEdit* label_edit = new QLineEdit (&dialog);
          label_edit->setPlaceholderText (tr ("path to the label volume"));
          QLineEdit* lut_edit = new QLineEdit (&dialog);
          lut_edit->setPlaceholderText (tr ("path to the lookup table"));
          QPushButton* label_browse = new QPushButton (tr ("Browse..."), &dialog);
          QPushButton* lut_browse = new QPushButton (tr ("Browse..."), &dialog);
          grid->addWidget (new QLabel (tr ("Label volume")), 0, 0);
          grid->addWidget (label_edit, 0, 1);
          grid->addWidget (label_browse, 0, 2);
          grid->addWidget (new QLabel (tr ("Lookup table")), 1, 0);
          grid->addWidget (lut_edit, 1, 1);
          grid->addWidget (lut_browse, 1, 2);

          QDialogButtonBox* buttons = new QDialogButtonBox (
              QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
          layout->addWidget (buttons);
          QPushButton* open_ok = buttons->button (QDialogButtonBox::Open);
          open_ok->setEnabled (false);

          auto refresh_ok = [&] () {
            open_ok->setEnabled (!label_edit->text().trimmed().isEmpty() &&
                                 !lut_edit->text().trimmed().isEmpty());
          };
          connect (label_edit, &QLineEdit::textChanged, refresh_ok);
          connect (lut_edit, &QLineEdit::textChanged, refresh_ok);
          connect (label_browse, &QPushButton::clicked, [&] () {
            const std::string path = Dialog::File::get_image (&dialog, "Select atlas label volume");
            if (path.size())
              label_edit->setText (qstr (path));
          });
          connect (lut_browse, &QPushButton::clicked, [&] () {
            const std::string path = Dialog::File::get_file (&dialog,
                "Select lookup table (FreeSurfer / ITK-SNAP / AAL / MRtrix / basic)");
            if (path.size())
              lut_edit->setText (qstr (path));
          });
          connect (buttons, SIGNAL (accepted()), &dialog, SLOT (accept()));
          connect (buttons, SIGNAL (rejected()), &dialog, SLOT (reject()));

          if (dialog.exec() != QDialog::Accepted)
            return;
          const std::string label_path = label_edit->text().trimmed().toStdString();
          const std::string lut_path = lut_edit->text().trimmed().toStdString();
          if (label_path.empty() || lut_path.empty())
            return;

          atlas_list_model->add_item (label_path, lut_path);
          atlas_list_view->selectionModel()->select (
              atlas_list_model->index (atlas_list_model->rowCount()-1, 0),
              QItemSelectionModel::ClearAndSelect);
          populate_region_list();
          focus_changed_slot();
          window().updateGL();
        }



        void Atlas::atlas_close_slot ()
        {
          QModelIndexList indexes = atlas_list_view->selectionModel()->selectedIndexes();
          while (!indexes.empty()) {
            atlas_list_model->remove_item (indexes.first());
            indexes = atlas_list_view->selectionModel()->selectedIndexes();
          }
          populate_region_list();
          update_region_labels();
          window().updateGL();
        }



        void Atlas::hide_all_slot ()
        {
          window().updateGL();
        }



        void Atlas::toggle_shown_slot (const QModelIndex& index, const QModelIndex&)
        {
          atlas_list_view->setCurrentIndex (index);
          window().updateGL();
        }



        void Atlas::selection_changed_slot (const QItemSelection&, const QItemSelection&)
        {
          populate_region_list();
          update_region_labels();
        }



        void Atlas::opacity_slot (int value)
        {
          const float alpha = value / 1000.0f;
          for (size_t i = 0; i < atlas_list_model->items.size(); ++i) {
            Item* atlas = dynamic_cast<Item*> (atlas_list_model->items[i].get());
            if (atlas)
              atlas->alpha = alpha;
          }
          window().updateGL();
        }



        void Atlas::dim_slot (int value)
        {
          const float dim = value / 1000.0f;
          for (size_t i = 0; i < atlas_list_model->items.size(); ++i) {
            Item* atlas = dynamic_cast<Item*> (atlas_list_model->items[i].get());
            if (atlas)
              atlas->dim_factor = dim;
          }
          window().updateGL();
        }



        void Atlas::focus_changed_slot ()
        {
          Item* atlas = current_item();
          if (!atlas) {
            update_region_labels();
            return;
          }
          set_focus_region (atlas->region_index_at (window().focus()));
        }



        void Atlas::hover_changed_slot ()
        {
          Item* atlas = current_item();
          if (!atlas)
            return;
          Mode::Base* mode = window().get_current_mode();
          if (!mode)
            return;
          const Projection* proj = mode->get_current_projection();
          if (!proj) {
            set_hover_region (0);
            return;
          }
          set_hover_region (atlas->region_index_at (
                proj->screen_to_model (window().mouse_position(), window().focus())));
        }



        // The crosshair region stays active until the focus moves; hovering another
        // region lights that one up as well rather than replacing it.
        void Atlas::set_focus_region (size_t index)
        {
          Item* atlas = current_item();
          if (!atlas || atlas->focus_index == index)
            return;
          atlas->focus_index = index;
          update_region_labels();
          select_in_region_list (index);
          window().updateGL();
        }



        void Atlas::set_hover_region (size_t index)
        {
          Item* atlas = current_item();
          if (!atlas || atlas->hover_index == index)
            return;
          atlas->hover_index = index;
          update_region_labels();
          window().updateGL();
        }



        void Atlas::select_in_region_list (size_t index)
        {
          Item* atlas = current_item();
          if (!atlas)
            return;
          // mirror the selection in the region list without re-triggering it
          syncing_region_list = true;
          if (!index) {
            region_list->setCurrentItem (nullptr);
          } else {
            const uint32_t label = atlas->label_of (index);
            for (int i = 0; i != region_list->count(); ++i) {
              if (region_list->item(i)->data (Qt::UserRole).toUInt() == label) {
                region_list->setCurrentRow (i);
                region_list->scrollToItem (region_list->item(i));
                break;
              }
            }
          }
          syncing_region_list = false;
        }



        QString Atlas::describe_region (size_t index)
        {
          Item* atlas = current_item();
          if (!atlas || !index)
            return QString ("—");
          const uint32_t label = atlas->label_of (index);
          const auto rgb = atlas->colour_of_index (index);
          const QColor colour (int (rgb[0]*255.0f), int (rgb[1]*255.0f), int (rgb[2]*255.0f));
          return QString ("<span style=\"color:%1\">&#9632;</span> <b>%2</b> [%3]")
              .arg (colour.name())
              .arg (qstr (atlas->name_of_label (label)).toHtmlEscaped())
              .arg (label);
        }



        void Atlas::update_region_labels ()
        {
          Item* atlas = current_item();
          focus_region_label->setText (describe_region (atlas ? atlas->focus_index : 0));
          hover_region_label->setText (describe_region (atlas ? atlas->hover_index : 0));
        }



        void Atlas::region_activated_slot (QListWidgetItem* item)
        {
          if (!item)
            return;
          Item* atlas = current_item();
          if (!atlas)
            return;
          const uint32_t label = uint32_t (item->data (Qt::UserRole).toUInt());
          const auto it = atlas->centroids.find (label);
          if (it == atlas->centroids.end())
            return;
          window().set_focus (it->second);
          window().updateGL();
        }



        void Atlas::region_check_slot (QListWidgetItem* item)
        {
          // Ticks are set on the item while the list is being rebuilt, which emits
          // this; without the guard the atlas would be rewritten from a half-built list.
          if (syncing_region_list || !item)
            return;
          Item* atlas = current_item();
          if (!atlas)
            return;
          const uint32_t label = uint32_t (item->data (Qt::UserRole).toUInt());
          const bool on = item->checkState() == Qt::Checked;
          atlas->set_checked (atlas->index_of (label), on);
          // A tick that no longer matches the search has to stay visible - it is
          // still on screen, and hiding it hides what the picture is made of - so
          // the list is rebuilt to pin it above the matches.
          if (!region_filter->text().isEmpty())
            populate_region_list();
          window().updateGL();
        }



        void Atlas::region_filter_slot (const QString&)
        {
          populate_region_list();
        }



        void Atlas::clear_region_ticks_slot ()
        {
          Item* atlas = current_item();
          if (!atlas)
            return;
          for (const auto& kv : atlas->centroids)
            atlas->set_checked (atlas->index_of (kv.first), false);
          populate_region_list();
          window().updateGL();
        }



        void Atlas::populate_region_list ()
        {
          syncing_region_list = true;
          region_list->clear();
          Item* atlas = current_item();
          if (atlas) {
            const QString needle = region_filter->text().trimmed().toLower();

            auto add_divider = [&] (const QString& text) {
              QListWidgetItem* qitem = new QListWidgetItem (text, region_list);
              qitem->setFlags (Qt::NoItemFlags);          // not checkable, not selectable
              QFont font = qitem->font();
              font.setItalic (true);
              qitem->setFont (font);
            };
            auto add_region = [&] (uint32_t label) {
              const size_t index = atlas->index_of (label);
              QListWidgetItem* qitem = new QListWidgetItem (qstr (atlas->name_of_label (label)), region_list);
              qitem->setData (Qt::UserRole, QVariant (uint (label)));
              qitem->setFlags (qitem->flags() | Qt::ItemIsUserCheckable);
              qitem->setCheckState (atlas->is_checked (index) ? Qt::Checked : Qt::Unchecked);
              const auto rgb = atlas->colour_of_index (index);
              QPixmap swatch (12, 12);
              swatch.fill (QColor (int (rgb[0]*255.0f), int (rgb[1]*255.0f), int (rgb[2]*255.0f)));
              qitem->setIcon (QIcon (swatch));
            };

            // Only list regions actually present in the volume (those with a centroid).
            // Ticked ones first, whatever the search says: they are what is drawn.
            size_t ticked = 0;
            for (const auto& kv : atlas->centroids)
              if (atlas->is_checked (atlas->index_of (kv.first))) ++ticked;
            if (ticked && !needle.isEmpty()) {
              add_divider (tr ("\u2014 ticked (%1) \u2014").arg (uint64_t (ticked)));
              for (const auto& kv : atlas->centroids)
                if (atlas->is_checked (atlas->index_of (kv.first)))
                  add_region (kv.first);
              add_divider (tr ("\u2014 matches \u2014"));
            }
            for (const auto& kv : atlas->centroids) {
              const uint32_t label = kv.first;
              if (!needle.isEmpty()) {
                if (atlas->is_checked (atlas->index_of (label)))
                  continue;                                // already pinned above
                if (!qstr (atlas->name_of_label (label)).toLower().contains (needle))
                  continue;
              }
              add_region (label);
            }
          }
          syncing_region_list = false;
        }



        void Atlas::get_session (nlohmann::json& node) const
        {
          node = nlohmann::json::array();
          for (size_t i = 0; i < atlas_list_model->items.size(); ++i) {
            const Item* atlas = dynamic_cast<const Item*> (atlas_list_model->items[i].get());
            // Both halves must still exist to be worth restoring; an atlas is
            // useless without its lookup table.
            if (atlas && Path::is_file (atlas->label_path) && Path::is_file (atlas->lut_path))
              node.push_back ({ { "labels", atlas->label_path }, { "lut", atlas->lut_path } });
          }
        }



        void Atlas::set_session (const nlohmann::json& node)
        {
          if (!node.is_array())
            return;
          vector<std::string> skipped;
          for (const auto& entry : node) {
            if (entry.find ("labels") == entry.end() || entry.find ("lut") == entry.end())
              continue;
            const std::string labels = entry["labels"].get<std::string>();
            const std::string lut = entry["lut"].get<std::string>();
            // Per entry, so one missing atlas does not cost the others theirs.
            try {
              atlas_list_model->add_item (labels, lut);
            } catch (Exception&) {
              skipped.push_back (labels);
            }
          }
          if (skipped.size())
            WARN ("session: " + str(skipped.size()) + " atlas(es) could not be restored: "
                  + join (skipped, ", "));
          if (atlas_list_model->rowCount())
            atlas_list_view->selectionModel()->select (
                atlas_list_model->index (atlas_list_model->rowCount()-1, 0),
                QItemSelectionModel::ClearAndSelect);
          populate_region_list();
          update_region_labels();
        }



        void Atlas::add_commandline_options (MR::App::OptionList& options)
        {
          using namespace MR::App;
          options
            + OptionGroup ("Atlas tool options")

            + Option ("atlas.load", "Load an atlas: a label volume and its lookup table.").allow_multiple()
            +   Argument ("labels").type_image_in()
            +   Argument ("lut").type_file_in()

            + Option ("atlas.opacity", "Set the opacity of the active (crosshair / hovered) regions [0-1].").allow_multiple()
            +   Argument ("value").type_float (0.0, 1.0)

            + Option ("atlas.dim", "Set the opacity of all other regions, relative to atlas.opacity [0-1].").allow_multiple()
            +   Argument ("value").type_float (0.0, 1.0)

            + Option ("atlas.select", "Tick one atlas region, so it is drawn at full opacity "
                                      "while the rest stay dimmed. Names are matched case-insensitively.").allow_multiple()
            +   Argument ("name").type_text()

            + Option ("atlas.export_region", "Write one atlas region out as a binary mask image.").allow_multiple()
            +   Argument ("name").type_text()
            +   Argument ("image").type_image_out();
        }



        bool Atlas::process_commandline_option (const MR::App::ParsedOption& opt)
        {
          if (opt.opt->is ("atlas.load")) {
            atlas_list_model->add_item (std::string (opt[0]), std::string (opt[1]));
            atlas_list_view->selectionModel()->select (
                atlas_list_model->index (atlas_list_model->rowCount()-1, 0),
                QItemSelectionModel::ClearAndSelect);
            populate_region_list();
            focus_changed_slot();
            window().updateGL();
            return true;
          }

          if (opt.opt->is ("atlas.opacity")) {
            opacity_slider->setSliderPosition (int (1.0e3f * float (opt[0])));
            return true;
          }

          if (opt.opt->is ("atlas.dim")) {
            dim_slider->setSliderPosition (int (1.0e3f * float (opt[0])));
            return true;
          }

          if (opt.opt->is ("atlas.select")) {
            Item* atlas = current_item();
            if (!atlas) {
              WARN ("no atlas is loaded: -atlas.select has nothing to tick");
              return true;
            }
            const std::string wanted = lowercase (std::string (opt[0]));
            bool found = false;
            for (const auto& kv : atlas->centroids) {
              if (lowercase (atlas->name_of_label (kv.first)) != wanted)
                continue;
              atlas->set_checked (atlas->index_of (kv.first), true);
              found = true;
            }
            if (!found)
              WARN ("no atlas region named \"" + std::string (opt[0]) + "\" is loaded");
            populate_region_list();
            window().updateGL();
            return true;
          }

          if (opt.opt->is ("atlas.export_region")) {
            const std::string name (opt[0]);
            // Go through the RegionProvider rather than straight to the Item, so
            // this exercises exactly the path the tracking tool will use.
            vector<RegionRef> all;
            region_provider()->list_regions (all);
            for (const auto& ref : all) {
              if (ref.name != name)
                continue;
              auto mask = region_provider()->get_region_mask (ref);
              MR::Header H (mask);
              H.datatype() = MR::DataType::Bit;
              auto out = MR::Image<bool>::create (opt[1], H);
              MR::copy (mask, out);
              return true;
            }
            WARN ("no atlas region named \"" + name + "\" is loaded");
            return true;
          }

          return false;
        }



        Atlas::~Atlas () { }


        RegionProvider* Atlas::region_provider ()
        {
          if (!regions)
            regions.reset (new Regions (*this));
          return regions.get();
        }



        void Atlas::Regions::list_regions (vector<RegionRef>& out) const
        {
          for (size_t i = 0; i != tool.atlas_list_model->items.size(); ++i) {
            const Item* atlas = dynamic_cast<const Item*> (tool.atlas_list_model->items[i].get());
            if (!atlas)
              continue;
            // Only regions actually present in the volume (those with a centroid).
            for (const auto& kv : atlas->centroids) {
              const uint32_t label = kv.first;
              RegionRef ref;
              ref.provider = provider_name();
              ref.name = atlas->name_of_label (label);
              ref.key = make_key (atlas->label_path, label);
              const auto rgb = atlas->colour_of_index (atlas->index_of (label));
              ref.colour = QColor (int (rgb[0]*255.0f), int (rgb[1]*255.0f), int (rgb[2]*255.0f));
              ref.index = label;
              out.push_back (ref);
            }
          }
        }



        Atlas::Item* Atlas::Regions::item_for (const RegionRef& ref, uint32_t& label) const
        {
          label = uint32_t (ref.index);
          // The key carries the owning atlas, so the right one is picked even
          // when several atlases share a label value.
          for (size_t i = 0; i != tool.atlas_list_model->items.size(); ++i) {
            Item* atlas = dynamic_cast<Item*> (tool.atlas_list_model->items[i].get());
            if (atlas && make_key (atlas->label_path, label) == ref.key)
              return atlas;
          }
          return nullptr;
        }



        MR::Image<bool> Atlas::Regions::get_region_mask (const RegionRef& ref) const
        {
          uint32_t label = 0;
          Item* atlas = item_for (ref, label);
          if (!atlas)
            throw Exception ("atlas region \"" + ref.name + "\" is no longer loaded");
          return atlas->region_mask (label);
        }



        bool Atlas::Regions::resolve (const std::string& key, RegionRef& ref) const
        {
          vector<RegionRef> all;
          list_regions (all);
          for (const auto& candidate : all) {
            if (candidate.key == key) {
              ref = candidate;
              return true;
            }
          }
          return false;
        }



        void Atlas::dropEvent (QDropEvent*)
        {
          // Atlas loading needs a label volume + a LUT, so drag-and-drop (which
          // supplies only file paths) is intentionally not wired up here.
        }


      }
    }
  }
}
