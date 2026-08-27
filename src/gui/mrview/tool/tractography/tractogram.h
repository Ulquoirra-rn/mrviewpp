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

#ifndef __gui_mrview_tool_tractogram_h__
#define __gui_mrview_tool_tractogram_h__

#include "gui/mrview/displayable.h"
#include "gui/mrview/region_source.h"
#include "dwi/tractography/properties.h"
#include "dwi/tractography/streamline.h"
#include "dwi/tractography/recognition/refine.h"
#include "gui/mrview/tool/tractography/tractography.h"


namespace MR
{

  namespace GUI
  {
    class Projection;

    namespace MRView
    {
      class Window;

      namespace Tool
      {
        class Tractogram : public Displayable
        { MEMALIGN(Tractogram)
          Q_OBJECT

          public:
            // When loading a .trx group as its own tractogram, display_name
            // overrides the list label and track_filter restricts loading to the
            // given original streamline indices (empty = load all streamlines).
            Tractogram (Tractography& tool, const std::string& file_path,
                        const std::string& display_name = std::string(),
                        const vector<size_t>& track_filter = vector<size_t>());

            ~Tractogram ();

            Window& window () const { return *Window::main; }

            void render (const Projection& transform);

            void request_render_colourbar (DisplayableVisitor& visitor) override {
              if (color_type == TrackColourType::ScalarFile && show_colour_bar)
                visitor.render_tractogram_colourbar(*this);
            }

            void load_tracks();

            // Upload streamlines that were generated in-process rather than read
            // from a file (see the Track generation tool). The points are also
            // retained on the CPU, so the tractogram can be saved and edited
            // without a source file to re-read.
            void load_tracks_from_memory (const vector<MR::DWI::Tractography::Streamline<float>>&,
                                          const MR::DWI::Tractography::Properties&,
                                          uint64_t total_attempted = 0);
            bool is_in_memory () const { return memory_tracks.size(); }

            //! Replace the streamlines of an in-memory tractogram.
            /*! For the live preview while tracking runs: repeatedly uploading
             *  without releasing the old buffers would leak them. */
            void reload_from_memory (const vector<MR::DWI::Tractography::Streamline<float>>&,
                                     uint64_t total_attempted = 0);
            void release_track_buffers ();

            // --- selection / editing ---
            // Selection is rendered by reusing the per-streamline threshold path
            // rather than a new shader: each streamline gets a scalar of 1 or 0
            // and the threshold sits at 0.5, so deselected streamlines vanish.

            //! Number of streamlines currently held on the CPU (0 if not cached).
            size_t num_cpu_tracks () const { return cpu_cache ? cpu_cache->tracks.size() : 0; }
            //! Populate the CPU cache, re-reading from file if necessary.
            /*! Expensive for large tractograms; call only when editing starts. */
            void enable_editing ();
            void disable_editing ();
            bool editing_enabled () const { return bool (cpu_cache); }
            const vector<MR::DWI::Tractography::Streamline<float>>& cpu_tracks () const;

            //! Per-streamline selection flags; empty until editing is enabled.
            const vector<uint8_t>& selection () const { return selected_flags; }
            void set_selection (const vector<uint8_t>&);
            void clear_selection ();
            //! Discard the unselected (keep=true) or selected (keep=false) streamlines.
            void apply_selection (bool keep);
            //! The selected streamlines, for splitting into a new tractogram.
            vector<MR::DWI::Tractography::Streamline<float>> selected_tracks () const;
            //! The streamlines a "keep selection" would throw away.
            vector<MR::DWI::Tractography::Streamline<float>> unselected_tracks () const;

            //! Candidates an auto-track run discarded on its shape-distance metric.
            /*! Held with the tract they were rejected from, so "what did the threshold
             *  remove?" can still be answered after later runs - which is the only way
             *  to tell a threshold that is too strict from a bundle that is genuinely
             *  not there. Empty for anything not produced by auto-tracking. */
            const vector<MR::DWI::Tractography::Streamline<float>>& rejected_tracks () const
              { return rejected_tracks_; }
            void set_rejected_tracks (const vector<MR::DWI::Tractography::Streamline<float>>& tracks)
              { rejected_tracks_ = tracks; }

            //! What this tract was recognised as, so it can be re-recognised.
            /*! Refinement is non-destructive: what a run kept and what it rejected are
             *  both still here, so their union is the candidate set and any strictness
             *  can be re-derived from it at any time - no third copy, and no
             *  re-tracking. This records the other half of what that needs: which
             *  atlas bundle to measure against and with what settings.
             *
             *  Cleared by a destructive edit (Keep / Delete), because the candidate
             *  set it refers to no longer exists after one. */
            struct Refinement { NOMEMALIGN
              std::string bundle;    //!< atlas bundle name, as the registration knows it
              MR::DWI::Tractography::Recognition::BundleMatcher::Params match;
              MR::DWI::Tractography::Recognition::RefineOptions options;
              //! Competing bundles chosen by hand; empty means "whatever the atlas says".
              vector<std::string> neighbours;
              //! The settings the run itself used, for Revert.
              MR::DWI::Tractography::Recognition::RefineOptions original;
              //! Streamlines currently shown, and how many there were to choose from.
              /*! Recorded when the refinement is applied, so the panel can report
               *  "812 / 1204" without loading any streamlines to count them. */
              size_t kept = 0, candidates = 0;
              bool valid = false;
            };
            const Refinement& refinement () const { return refinement_; }
            Refinement& refinement () { return refinement_; }
            void clear_refinement () { refinement_ = Refinement(); }
            //! Kept plus rejected, in that order: everything the run had to choose from.
            vector<MR::DWI::Tractography::Streamline<float>> refine_candidates () const;

            //! A standing "passes through" / "avoids" criterion on a region.
            /*! Rules are kept rather than baked into the selection, so that they
             *  can be re-evaluated when the region itself is edited - drawing more
             *  of an avoid region should immediately deselect what it now covers. */
            struct SelectionRule { NOMEMALIGN
              RegionRef region;
              bool want_inside;    //!< true = passes through, false = avoids
            };
            const vector<SelectionRule>& selection_rules () const { return rules; }
            void add_selection_rule (const RegionRef&, bool want_inside);
            void clear_selection_rules ();
            //! Recompute the selection from the rules. GUI thread only.
            /*! Rules whose owning tool has gone away are skipped, and their count
             *  returned, so the caller can say so rather than silently ignoring them. */
            size_t apply_selection_rules ();

            const vector<MR::DWI::Tractography::Streamline<float>>& in_memory_tracks () const { return memory_tracks; }
            void save_to_file (const std::string& path) const;

            void load_end_colours();
            void load_intensity_track_scalars (const std::string&);
            void load_threshold_track_scalars (const std::string&);

            // Per-vertex (dpv) / per-streamline (dps) data arrays embedded in a
            // .trx source file, usable as threshold sources.
            struct TrxDataArray { NOMEMALIGN
              std::string entry;   // ZIP entry, e.g. "dpv/fa.float32"
              std::string name;    // display name, e.g. "fa"
              bool per_vertex;
            };
            vector<TrxDataArray> get_trx_threshold_arrays () const;
            void load_threshold_track_scalars_from_trx (const std::string& entry, bool per_vertex);
            void erase_colour_data();
            void erase_intensity_scalar_data ();
            void erase_threshold_scalar_data ();

            void set_color_type (const TrackColourType);
            void set_threshold_type (const TrackThresholdType);
            void set_geometry_type (const TrackGeometryType);
            TrackColourType get_color_type() const { return color_type; }
            TrackThresholdType get_threshold_type() const { return threshold_type; }
            TrackGeometryType get_geometry_type() const { return geometry_type; }

            float get_threshold_rate() const {
              switch (threshold_type) {
                case TrackThresholdType::None: return NaN;
                case TrackThresholdType::UseColourFile: return scaling_rate();
                case TrackThresholdType::SeparateFile: return (1e-3 * (threshold_max - threshold_min));
              }
              assert (0);
              return NaN;
            }
            float get_threshold_min()  const { return threshold_min; }
            float get_threshold_max()  const { return threshold_max; }
            //! The file this tractogram was read from; fixed for its lifetime.
            const std::string& get_filename() const { return filename; }
            //! The name shown in the list, which the user can rename.
            /*! Distinct from get_filename(): that one has to keep pointing at the
             *  source file, so anything user-facing - an export's suggested name,
             *  a derived tract's name - belongs here instead. */
            const std::string& display_name() const { return Displayable::get_filename(); }
            //! The tracking algorithm this came from, if it says so.
            /*! tckgen and the Track generation tool both record it as "method";
             *  empty for a tractogram whose header does not. */
            std::string tracking_method () const {
              const auto it = properties.find ("method");
              return it == properties.end() ? std::string() : it->second;
            }

            // Result of re-reading this tractogram from disk and dropping any
            // streamlines that fall entirely outside the active threshold.
            struct FilteredTracks { NOMEMALIGN
              vector<MR::DWI::Tractography::Streamline<float>> tracks;
              vector<vector<float>> dpv;   // per surviving track: one threshold value per vertex
              vector<float> dps;           // per surviving track: a single threshold value
              bool per_vertex = false, per_streamline = false;
              std::string source_name;     // basename of the source file
            };
            void get_filtered_streamlines (FilteredTracks&) const;

            static TrackGeometryType default_tract_geom;
            static constexpr float default_line_thickness = 2e-3f;
            static constexpr float default_point_size = 4e-3f;

            bool scalarfile_by_direction;
            bool show_colour_bar;
            bool should_update_stride;
            float original_fov;
            float line_thickness;
            std::string intensity_scalar_filename;
            std::string threshold_scalar_filename;

            class Shader : public Displayable::Shader { MEMALIGN(Shader)
              public:
                Shader () :
                    do_crop_to_slab (false),
                    use_lighting (false),
                    color_type (TrackColourType::Direction),
                    threshold_type (TrackThresholdType::None),
                    geometry_type (Tractogram::default_tract_geom) { }
                std::string vertex_shader_source (const Displayable&) override;
                std::string fragment_shader_source (const Displayable&) override;
                std::string geometry_shader_source (const Displayable&) override;
                virtual bool need_update (const Displayable&) const override;
                virtual void update (const Displayable&) override;
              protected:
                bool do_crop_to_slab, use_lighting;
                TrackColourType color_type;
                TrackThresholdType threshold_type;
                TrackGeometryType geometry_type;

            } track_shader;

          signals:
            void scalingChanged ();

          private:
            static const int track_padding = 6;
            Tractography& tractography_tool;

            const std::string filename;

            // Original streamline indices to load (empty = all); used to load a
            // single .trx group as its own tractogram.
            vector<size_t> track_filter;

            TrackColourType color_type;
            TrackThresholdType threshold_type;
            TrackGeometryType geometry_type;

            // Instead of tracking the file path, pre-calculate the
            //   streamline tangents and store them; then, if colour by
            //   endpoint is requested, generate the buffer based on these
            //   and the known track sizes
            vector<Eigen::Vector3f> endpoint_tangents;

            vector<GLuint> vertex_buffers;
            vector<GLuint> vertex_array_objects;
            vector<GLuint> colour_buffers;
            vector<GLuint> intensity_scalar_buffers;
            vector<GLuint> threshold_scalar_buffers;
            MR::DWI::Tractography::Properties properties;
            // Non-empty only for tractograms generated in-process; the file-backed
            // path keeps its points on the GPU only and re-reads on demand.
            vector<MR::DWI::Tractography::Streamline<float>> memory_tracks;
            // Set by the Track generation tool; see rejected_tracks().
            vector<MR::DWI::Tractography::Streamline<float>> rejected_tracks_;
            Refinement refinement_;
            // CPU-side copy used for editing and statistics. Opt-in, because a
            // large tractogram costs hundreds of MB.
            std::unique_ptr<FilteredTracks> cpu_cache;
            vector<uint8_t> selected_flags;
            vector<SelectionRule> rules;
            void upload_selection ();
            // Streamlines attempted (accepted + rejected), so a saved .tck carries
            // the same total_count a tckgen run would have written.
            uint64_t memory_total_count = 0;
            vector<vector<GLint> > track_starts;
            vector<vector<GLint> > track_sizes;
            vector<vector<GLint> > original_track_sizes;
            vector<vector<GLint> > original_track_starts;
            vector<size_t> num_tracks_per_buffer;
            // EBOs and indices for chunks of tracks
            vector<GLuint> element_buffers;
            vector<GLsizei> element_counts;
            GLint sample_stride;
            bool vao_dirty;

            // Extra members now required since different scalar files
            //   may be used for streamline colouring and thresholding
            float threshold_min, threshold_max;


            void load_tracks_onto_GPU (vector<Eigen::Vector3f>& buffer,
                                       vector<GLint>& starts,
                                       vector<GLint>& sizes,
                                       size_t& tck_count);

            void load_end_colours_onto_GPU (vector<Eigen::Vector3f>&);

            void load_intensity_scalars_onto_GPU (vector<float>& buffer, size_t& tck_count);
            void load_threshold_scalars_onto_GPU (vector<float>& buffer, size_t& tck_count);

            // Upload a flat (one value per vertex, ungrouped, unpadded) threshold
            // scalar array, splitting it by the known per-streamline vertex counts.
            void load_threshold_scalars_from_values (const vector<float>& flat_per_vertex, const std::string& label);

            void render_streamlines ();

            void update_stride ();

          private slots:
            void on_FOV_changed() {
              should_update_stride = true;
            }
        };
      }
    }
  }
}

#endif

