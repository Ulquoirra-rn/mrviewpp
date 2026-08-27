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

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>

#include "gui/mrview/atlas_template.h"

#include <QCoreApplication>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>

#include "algo/loop.h"
#include "file/config.h"
#include "file/path.h"
#include "gui/mrview/data_path.h"
#include "math/SH.h"
#include "dwi/tractography/file_trx.h"
#include "file/json.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace AtlasTemplate
      {

        const char* const atlas_filename = "yeh2022.trx";
        // The HCP1065 QA template that accompanies the atlas, at 2 mm. QA is an
// anisotropy measure, so it pairs naturally with an FOD-derived anisotropy
// map and needs no structural image from the subject at all.
const char* const template_filename = "mni_qa.nii.gz";

        namespace
        {
          // A head alignment beyond these is not plausible, and every failure mode
          // seen while developing this landed well outside them: templates
          // collapsed to a plane (scale -> 0), blown up 50x, or rotated by exactly
          // 90 degrees. Reporting "unregistered" beats misplacing the atlas.
          constexpr float max_translation_mm = 80.0f;
          constexpr float max_rotation_deg = 45.0f;
          constexpr float min_allowed_scale = 0.75f;
          constexpr float max_allowed_scale = 1.35f;


          //! ITK/ANTs work in LPS, MRtrix and NIfTI in RAS.
          Eigen::Matrix3d lps_flip ()
          {
            Eigen::Matrix3d F = Eigen::Matrix3d::Identity();
            F(0,0) = -1.0; F(1,1) = -1.0;
            return F;
          }


          //! Read the 12 affine parameters and the centre from an ITK .mat file.
          /*! ANTs writes these as MATLAB level-4 files: a 20-byte header per
           *  variable, then the name, then the data. Two variables are present:
           *  AffineTransform_* (9 matrix entries then 3 translation), and fixed
           *  (the centre of rotation). */
          bool read_itk_affine (const std::string& path,
                                Eigen::Matrix3d& matrix,
                                Eigen::Vector3d& translation,
                                Eigen::Vector3d& centre)
          {
            std::ifstream in (path, std::ios::binary);
            if (!in)
              return false;
            const std::string bytes ((std::istreambuf_iterator<char> (in)),
                                     std::istreambuf_iterator<char>());
            bool have_affine = false, have_centre = false;
            size_t offset = 0;
            while (offset + 20 <= bytes.size()) {
              int32_t header[5];
              memcpy (header, bytes.data() + offset, 20);
              offset += 20;
              const int32_t rows = header[1], cols = header[2], name_length = header[4];
              if (rows <= 0 || cols <= 0 || name_length <= 0 ||
                  offset + size_t (name_length) > bytes.size())
                return false;
              const std::string name (bytes.data() + offset, size_t (name_length) - 1);
              offset += size_t (name_length);
              const bool is_double = ((header[0] % 100) / 10) == 0;
              const size_t count = size_t (rows) * size_t (cols);
              const size_t nbytes = count * (is_double ? 8 : 4);
              if (offset + nbytes > bytes.size())
                return false;
              vector<double> values (count);
              for (size_t n = 0; n != count; ++n) {
                if (is_double) {
                  double v; memcpy (&v, bytes.data() + offset + 8*n, 8); values[n] = v;
                } else {
                  float v; memcpy (&v, bytes.data() + offset + 4*n, 4); values[n] = double (v);
                }
              }
              offset += nbytes;

              if (name.compare (0, 15, "AffineTransform") == 0 && count >= 12) {
                for (size_t r = 0; r != 3; ++r)
                  for (size_t c = 0; c != 3; ++c)
                    matrix (r, c) = values[3*r + c];
                translation = Eigen::Vector3d (values[9], values[10], values[11]);
                have_affine = true;
              } else if (name == "fixed" && count >= 3) {
                centre = Eigen::Vector3d (values[0], values[1], values[2]);
                have_centre = true;
              }
            }
            if (!have_centre)
              centre.setZero();
            return have_affine;
          }


          //! Convert an ITK affine (LPS, about a centre) into a RAS point transform.
          /*! ITK maps q = A (p - c) + t + c in LPS. With F = diag(-1,-1,1) taking
           *  RAS to LPS and back, the equivalent RAS transform is
           *  q = (F A F) p + F (t + c - A c). Verified against the pipeline's own
           *  output: reproducing its stage-1 result this way matched at r = 1.00000. */
          transform_type itk_to_ras (const Eigen::Matrix3d& A,
                                     const Eigen::Vector3d& t,
                                     const Eigen::Vector3d& c)
          {
            const Eigen::Matrix3d F = lps_flip();
            transform_type out;
            out.linear() = F * A * F;
            out.translation() = F * (t + c - A * c);
            return out;
          }


          //! RMS over the l>0 SH coefficients: bright in white matter, dark elsewhere.
          /*! This is what the QA template is matched against. Written to \a out_path
           *  because antsRegistration reads its inputs from disk. */
          void write_anisotropy_map (const std::string& fod_path, const std::string& out_path)
          {
            auto fod = Image<default_type>::open (fod_path);
            Header header (fod);
            header.ndim() = 3;
            header.datatype() = DataType::Float32;
            auto out = Image<default_type>::create (out_path, header);
            const ssize_t num_volumes = fod.size(3);
            for (auto l = Loop (0, 3) (fod, out); l; ++l) {
              default_type sum_sq = 0.0;
              for (ssize_t v = 1; v != num_volumes; ++v) {
                fod.index(3) = v;
                const default_type value = fod.value();
                sum_sq += value * value;
              }
              out.value() = std::sqrt (sum_sq / default_type (num_volumes - 1));
            }
          }


          //! Rotation angle and scale range of an affine, via polar decomposition.
          /*! Not the trace formula: that is only the rotation angle for a pure
           *  rotation, and silently reports 0 degrees for an affine carrying a real
           *  rotation alongside scale and shear. */
          void decompose (const transform_type& transform,
                          float& rotation_deg, float& scale_min, float& scale_max)
          {
            Eigen::JacobiSVD<Eigen::Matrix3d> svd (transform.linear(),
                                                   Eigen::ComputeFullU | Eigen::ComputeFullV);
            Eigen::Matrix3d U = svd.matrixU();
            Eigen::Matrix3d R = U * svd.matrixV().transpose();
            if (R.determinant() < 0.0) {
              U.col(2) *= -1.0;
              R = U * svd.matrixV().transpose();
            }
            const default_type cosine = std::max (-1.0, std::min (1.0, 0.5 * (R.trace() - 1.0)));
            rotation_deg = float (std::acos (cosine) * 180.0 / Math::pi);
            scale_min = float (svd.singularValues().minCoeff());
            scale_max = float (svd.singularValues().maxCoeff());
          }
        }



        bool read_ants_affine (const std::string& path, transform_type& out)
        {
          Eigen::Matrix3d A;
          Eigen::Vector3d t, c;
          if (!read_itk_affine (path, A, t, c))
            return false;
          out = itk_to_ras (A, t, c);
          return true;
        }



        bool is_fod_image (const MR::Header& header)
        {
          if (header.ndim() < 4)
            return false;
          const ssize_t num_volumes = header.size(3);
          if (num_volumes < 6)
            return false;
          // LforN rounds down rather than reporting failure, so round-trip it: only
          // a complete antipodally-symmetric SH series survives.
          return ssize_t (Math::SH::NforL (Math::SH::LforN (num_volumes))) == num_volumes;
        }



        std::string ants_path ()
        {
#ifdef MRTRIX_WINDOWS
          const std::string exe ("antsRegistration.exe");
#else
          const std::string exe ("antsRegistration");
#endif
          // Packaged alongside mrview.
          if (QCoreApplication::instance()) {
            const std::string local = Path::join (QCoreApplication::applicationDirPath().toStdString(), exe);
            if (Path::is_file (local))
              return local;
          }
          //CONF option: MRViewAntsPath
          //CONF default: unset
          //CONF Full path of the antsRegistration executable used to align the
          //CONF built-in tract atlas to the subject. Set this if ANTs is installed
          //CONF somewhere the automatic search does not cover.
          const std::string configured = File::Config::get ("MRViewAntsPath");
          if (configured.size() && Path::is_file (configured))
            return configured;
          if (const char* ants_dir = getenv ("ANTSPATH")) {
            const std::string candidate = Path::join (ants_dir, exe);
            if (Path::is_file (candidate))
              return candidate;
          }
          // Leave the rest to the OS: QProcess searches PATH for a bare name. If it
          // is not there either, start() fails and that is reported.
          return exe;
        }



        Fit fit_to_image (const std::string& subject_path, Quality quality)
        {
          Fit fit;

          const std::string template_path = data_file (template_filename);

          QTemporaryDir workdir;
          if (!workdir.isValid()) {
            fit.note = "could not create a working directory";
            return fit;
          }
          //ENVVAR name: MRVIEWPP_KEEP_TEMP
          //ENVVAR Keep the working directory used for atlas registration, and log
          //ENVVAR its location, so the intermediate files can be inspected.
          if (getenv ("MRVIEWPP_KEEP_TEMP")) {
            workdir.setAutoRemove (false);
            INFO ("keeping registration working directory " + workdir.path().toStdString());
          }
          const QString prefix = workdir.filePath ("reg");

          // An FOD cannot be registered directly - it is 4D, and its volumes are SH
          // coefficients rather than an image. Reduce it to the anisotropy map that
          // the QA template is comparable with. A 3D input (an FA map, say) is used
          // as it stands.
          std::string fixed_path = subject_path;
          try {
            if (is_fod_image (Header::open (subject_path))) {
              fixed_path = workdir.filePath ("subject_anisotropy.nii.gz").toStdString();
              write_anisotropy_map (subject_path, fixed_path);
            }
          } catch (Exception& e) {
            fit.note = "could not read \"" + subject_path + "\": " + e[0];
            return fit;
          }

          // Fixed is the subject, moving is the template - matching how the
          // transforms this was validated against were produced. ITK therefore
          // writes the subject-to-template map, and the atlas needs its inverse.
          const QString fixed = QString::fromStdString (fixed_path);
          const QString moving = QString::fromStdString (template_path);
          const QString metric = "MI[" + fixed + "," + moving + ",1,32,Regular,0.25]";

          QStringList args;
          args << "--dimensionality" << "3" << "--float" << "1"
               << "--collapse-output-transforms" << "1"
               << "--output" << prefix
               << "--interpolation" << "Linear"
               << "--use-histogram-matching" << "1"
               << "--winsorize-image-intensities" << "[0.005,0.995]"
               << "--initial-moving-transform" << "[" + fixed + "," + moving + ",1]"
               << "--transform" << "Rigid[0.1]"
               << "--metric" << metric
               // The finest level is run rather than skipped: antsRegistrationSyN's
               // default ends in 0 iterations there, which is a speed choice we do
               // not need to make for a one-off background alignment.
               << "--convergence" << "[2000x1000x500x200,1e-7,10]"
               << "--shrink-factors" << "8x4x2x1"
               << "--smoothing-sigmas" << "3x2x1x0vox"
               << "--transform" << "Affine[0.1]"
               << "--metric" << metric
               << "--convergence" << "[2000x1000x500x200,1e-7,10]"
               << "--shrink-factors" << "8x4x2x1"
               << "--smoothing-sigmas" << "3x2x1x0vox";
          if (quality == Quality::Nonlinear) {
            args << "--transform" << "SyN[0.1,3,0]"
                 << "--metric" << metric
                 << "--convergence" << "[100x70x50x20,1e-6,10]"
                 << "--shrink-factors" << "8x4x2x1"
                 << "--smoothing-sigmas" << "3x2x1x0vox";
          }

          const std::string executable = ants_path();
          QProcess process;
          process.start (QString::fromStdString (executable), args);
          if (!process.waitForStarted (10000)) {
            fit.note = "could not run antsRegistration (\"" + executable + "\")";
            return fit;
          }
          // Rigid+affine takes tens of seconds, the nonlinear stage minutes. The cap
          // only exists so that a wedged process cannot hold the thread forever.
          if (!process.waitForFinished (1800000)) {
            process.kill();
            process.waitForFinished (5000);
            fit.note = "antsRegistration timed out";
            return fit;
          }
          if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            const std::string errors (process.readAllStandardError().constData());
            fit.note = "antsRegistration failed" + (errors.empty() ? std::string() : ": " + errors);
            return fit;
          }

          const std::string matrix_path = (prefix + "0GenericAffine.mat").toStdString();
          Eigen::Matrix3d A;
          Eigen::Vector3d t, c;
          if (!read_itk_affine (matrix_path, A, t, c)) {
            fit.note = "could not read the transform antsRegistration produced";
            return fit;
          }

          const transform_type subject_to_mni = itk_to_ras (A, t, c);
          if (!subject_to_mni.matrix().allFinite() ||
              std::abs (subject_to_mni.linear().determinant()) < 1e-6) {
            fit.note = "registration produced a degenerate transform";
            return fit;
          }

          transform_type candidate;
          candidate.matrix() = subject_to_mni.inverse().matrix();

          decompose (candidate, fit.rotation_deg, fit.min_scale, fit.max_scale);
          fit.translation_mm = float (candidate.translation().norm());

          if (fit.translation_mm > max_translation_mm || fit.rotation_deg > max_rotation_deg ||
              fit.min_scale < min_allowed_scale || fit.max_scale > max_allowed_scale) {
            fit.note = "fit rejected: " + str (fit.translation_mm, 3) + " mm, " +
                       str (fit.rotation_deg, 3) + " deg, scale " +
                       str (fit.min_scale, 3) + "-" + str (fit.max_scale, 3);
            return fit;
          }

          fit.mni_to_subject = candidate;
          fit.refined = true;
          fit.note = str (fit.translation_mm, 3) + " mm, " + str (fit.rotation_deg, 3) +
                     " deg, scale " + str (fit.min_scale, 3) + "-" + str (fit.max_scale, 3);
          return fit;
        }



        std::string atlas_directory ()
        {
          if (const char* env = getenv ("MRVIEWPP_TRACT_ATLAS")) {
            if (Path::is_dir (env))
              return env;
          }
          //CONF option: MRViewTractAtlasPath
          //CONF default: unset
          //CONF Directory holding a tract atlas as <category>/<bundle>.tck, for
          //CONF MRView++'s built-in atlas. When unset, the bundled .trx atlas is
          //CONF used instead.
          const std::string configured = File::Config::get ("MRViewTractAtlasPath");
          if (configured.size() && Path::is_dir (configured))
            return configured;
          const std::string beside_data = find_data_file (atlas_filename);
          if (beside_data.size()) {
            const std::string tracts = Path::join (Path::dirname (beside_data), "tracts");
            if (Path::is_dir (tracts))
              return tracts;
          }
          return std::string();
        }



        size_t default_max_streamlines ()
        {
          //CONF option: MRViewTractAtlasMaxStreamlines
          //CONF default: 2000
          //CONF Streamlines read per bundle from a directory tract atlas. Bundles of
          //CONF an averaged atlas can hold tens of thousands, far more than either
          //CONF the shape matcher or the tracking territory can use.
          return size_t (std::max (100, File::Config::get_int ("MRViewTractAtlasMaxStreamlines", 2000)));
        }



        vector<BundleRef> list_bundles ()
        {
          vector<BundleRef> out;
          const std::string root = atlas_directory();
          if (root.size()) {
            vector<std::string> categories;
            {
              Path::Dir dir (root);
              std::string entry;
              while ((entry = dir.read_name()).size()) {
                if (entry[0] == '.')
                  continue;
                if (Path::is_dir (Path::join (root, entry)))
                  categories.push_back (entry);
              }
            }
            std::sort (categories.begin(), categories.end());
            for (const auto& category : categories) {
              const std::string dir_path = Path::join (root, category);
              vector<std::string> files;
              Path::Dir dir (dir_path);
              std::string entry;
              while ((entry = dir.read_name()).size()) {
                if (entry[0] == '.' || !Path::has_suffix (entry, ".tck"))
                  continue;
                files.push_back (entry);
              }
              std::sort (files.begin(), files.end());
              for (const auto& file : files) {
                BundleRef ref;
                ref.category = category;
                ref.name = file.substr (0, file.size() - 4);
                ref.path = Path::join (dir_path, file);
                out.push_back (ref);
              }
            }
            // A directory with no .tck in it is a misconfiguration, not an empty
            // atlas; fall through to the bundled one rather than showing nothing.
            if (out.size())
              return out;
          }

          const std::string path = find_data_file (atlas_filename);
          if (path.empty())
            return out;
          for (const auto& group : MR::DWI::Tractography::TRX_Data::groups (path)) {
            BundleRef ref;
            // A group may be nested as "<category>/<bundle>", which is how
            // packaging/make_tract_atlas.py keeps a directory atlas's categories
            // when it packs one into a single archive.
            const size_t slash = group.first.find_last_of ('/');
            if (slash == std::string::npos) {
              ref.name = group.first;
            } else {
              ref.category = group.first.substr (0, slash);
              ref.name = group.first.substr (slash + 1);
            }
            out.push_back (ref);
          }
          std::sort (out.begin(), out.end(), [] (const BundleRef& a, const BundleRef& b) {
            return a.name < b.name;
          });
          return out;
        }



        vector<MR::DWI::Tractography::Streamline<float>> load_bundle (const BundleRef& ref,
                                                                      size_t max_streamlines)
        {
          if (!max_streamlines)
            max_streamlines = default_max_streamlines();
          vector<MR::DWI::Tractography::Streamline<float>> out;

          if (ref.path.empty()) {
            // .trx-backed: one archive, so the whole thing is read whatever we want
            // out of it. Parsed once and kept, because the caller asks bundle by
            // bundle and re-parsing per bundle is what made this slow.
            static std::map<std::string, vector<MR::DWI::Tractography::Streamline<float>>> all;
            static std::string cached_for;
            const std::string source = find_data_file (atlas_filename);
            if (all.empty() || cached_for != source) {
              all.clear();
              load_bundles (all);
              cached_for = source;
            }
            auto it = all.find (ref.category.size() ? ref.category + "/" + ref.name : ref.name);
            // NOLINTNEXTLINE - the map is static above, so the reference outlives us
            if (it == all.end())
              it = all.find (ref.name);
            if (it == all.end())
              return out;
            // A packed atlas is already capped and resampled, so it is taken as-is.
            return it->second;
          }

          MR::DWI::Tractography::Properties properties;
          MR::DWI::Tractography::Reader<float> reader (ref.path, properties);
          size_t total = 0;
          const auto count = properties.find ("count");
          if (count != properties.end())
            total = to<size_t> (count->second);

          // Stride so the kept streamlines are spread through the file rather than
          // taken from its head: a .tck is often written grouped by seed, so the
          // first N would sample one part of the bundle only.
          const size_t stride = (total > max_streamlines && max_streamlines)
                              ? (total + max_streamlines - 1) / max_streamlines : 1;
          MR::DWI::Tractography::Streamline<float> tck;
          size_t index = 0;
          while (reader (tck)) {
            if (index++ % stride)
              continue;
            out.push_back (tck);
            if (out.size() >= max_streamlines && stride == 1)
              break;
          }
          return out;
        }



        void load_bundles (std::map<std::string, vector<MR::DWI::Tractography::Streamline<float>>>& out)
        {
          out.clear();
          const std::string path = data_file (atlas_filename);

          // Every group index array in one pass: read_uint() would re-read the
          // whole archive per bundle.
          const auto groups = MR::DWI::Tractography::TRX_Data::read_groups (path);
          if (groups.empty())
            throw Exception ("\"" + path + "\" contains no named bundles");

          // Read the whole file once, then split by group membership: the archive is
          // a single blob, so per-group reads would parse it over and over.
          vector<MR::DWI::Tractography::Streamline<float>> all;
          {
            MR::DWI::Tractography::Properties properties;
            MR::DWI::Tractography::TRXReader<float> reader (path, properties);
            MR::DWI::Tractography::Streamline<float> tck;
            while (reader (tck))
              all.push_back (tck);
          }

          for (const auto& group : groups) {
            auto& bundle = out[group.first];
            bundle.reserve (group.second.size());
            for (const uint64_t index : group.second) {
              if (index < all.size())
                bundle.push_back (all[index]);
            }
          }
        }




        // --- the neighbour index ---------------------------------------------
        //
        // Competitive assignment needs to know which bundles run where the target
        // runs, and it needs to know cheaply: loading the whole atlas to find out
        // would cost more than the tracking it is meant to clean up. So each bundle
        // is reduced once to the set of 8 mm cells it passes through, and overlap
        // between those sets picks the competitors. Overlap is measured in atlas
        // space rather than the subject's, which is fine for *selecting* candidates:
        // the fit that places the atlas in a subject is smooth, so bundles that
        // share a territory in MNI share one in the subject too.

        namespace
        {
          std::mutex footprint_mutex;
          std::map<std::string, BundleFootprint> footprint_index;
          std::string footprint_stamp;

          constexpr float footprint_cell = 8.0f;

          //! 21 bits per axis, biased so negative coordinates stay in range.
          /*! The same packing VoxelHashGrid uses (bundle_matcher.h), so the two read
           *  the same way in a debugger. */
          uint64_t footprint_key (const Eigen::Vector3f& p)
          {
            const int32_t i = int32_t (std::floor (p[0] / footprint_cell));
            const int32_t j = int32_t (std::floor (p[1] / footprint_cell));
            const int32_t k = int32_t (std::floor (p[2] / footprint_cell));
            return (uint64_t ((i + (1<<20)) & 0x1FFFFF) << 42)
                 | (uint64_t ((j + (1<<20)) & 0x1FFFFF) << 21)
                 |  uint64_t ((k + (1<<20)) & 0x1FFFFF);
          }

          std::string footprint_cache_path ()
          {
            // Beside the autosaved session, which is the convention already set for
            // MRView++'s own per-user state (Window::autosave_session_path).
            return Path::join (Path::home(), ".mrview++-atlas-neighbours.json");
          }

          //! Identifies the atlas an index was built from, so a stale one is not used.
          /*! Size and modification time of every catalogued file, which changes
           *  whenever the atlas is repacked or repointed - cheaper and more reliable
           *  than hashing 1.3 GB. */
          std::string atlas_stamp (const vector<BundleRef>& catalogue)
          {
            std::string stamp = atlas_directory() + "|" + find_data_file (atlas_filename)
                              + "|" + str (catalogue.size()) + "|cell" + str (footprint_cell);
            for (const auto& ref : catalogue) {
              if (ref.path.empty())
                continue;
              struct stat info;
              if (!stat (ref.path.c_str(), &info))
                stamp += "|" + ref.name + ":" + str (int64_t (info.st_mtime))
                       + ":" + str (int64_t (info.st_size));
            }
            return stamp;
          }
        }



        bool footprints_ready ()
        {
          std::lock_guard<std::mutex> lock (footprint_mutex);
          return footprint_index.size();
        }



        bool load_footprint_cache ()
        {
          const vector<BundleRef> catalogue = list_bundles();
          if (catalogue.empty())
            return false;
          const std::string stamp = atlas_stamp (catalogue);
          {
            std::lock_guard<std::mutex> lock (footprint_mutex);
            if (footprint_index.size() && footprint_stamp == stamp)
              return true;
          }

          try {
            std::ifstream in (footprint_cache_path());
            if (!in)
              return false;
            nlohmann::json root;
            in >> root;
            if (!root.is_object() || root.value ("stamp", std::string()) != stamp)
              return false;
            std::map<std::string, BundleFootprint> loaded;
            const nlohmann::json& bundles = root.at ("bundles");
            // key()/value() rather than items(): the bundled nlohmann predates it.
            for (auto entry = bundles.begin(); entry != bundles.end(); ++entry) {
              BundleFootprint cells;
              for (const auto& key : entry.value())
                cells.insert (key.get<uint64_t>());
              if (cells.size())
                loaded[entry.key()] = std::move (cells);
            }
            if (loaded.empty())
              return false;
            std::lock_guard<std::mutex> lock (footprint_mutex);
            footprint_index = std::move (loaded);
            footprint_stamp = stamp;
            return true;
          } catch (...) {
            // A corrupt or older-format cache is simply not a cache; rebuilding is
            // the recovery, and it must not be an error the user has to see.
            return false;
          }
        }



        void build_footprints ()
        {
          const vector<BundleRef> catalogue = list_bundles();
          if (catalogue.empty())
            return;
          const std::string stamp = atlas_stamp (catalogue);

          std::map<std::string, BundleFootprint> built;
          for (const auto& ref : catalogue) {
            try {
              // A modest cap: a bundle's territory saturates long before its last
              // streamline, and this pass is the one that has to read every file.
              const auto bundle = load_bundle (ref, 200);
              BundleFootprint cells;
              for (const auto& tck : bundle)
                for (const auto& vertex : tck)
                  cells.insert (footprint_key (vertex));
              if (cells.size())
                built[ref.name] = std::move (cells);
            } catch (Exception& e) {
              // One unreadable bundle must not cost the index; it simply never
              // competes.
              INFO ("atlas footprint: skipping \"" + ref.name + "\": " + e[0]);
            }
          }
          if (built.empty())
            return;

          {
            std::lock_guard<std::mutex> lock (footprint_mutex);
            footprint_index = built;
            footprint_stamp = stamp;
          }

          try {
            nlohmann::json root;
            root["stamp"] = stamp;
            nlohmann::json bundles = nlohmann::json::object();
            for (const auto& entry : built) {
              nlohmann::json keys = nlohmann::json::array();
              for (const uint64_t key : entry.second)
                keys.push_back (key);
              bundles[entry.first] = keys;
            }
            root["bundles"] = bundles;
            std::ofstream out (footprint_cache_path());
            out << root.dump();
          } catch (...) {
            // Not being able to cache it only means paying for it again next time.
            WARN ("could not write the atlas neighbour index to \"" + footprint_cache_path() + "\"");
          }
        }



          // The maps spell the tract out; the atlas abbreviates. Only unambiguous
        // pairings are listed - the corpus callosum maps (Forceps_Major,
        // Forceps_Minor, Corpus_Callosum_Tapetum, Corpus_Callosum_full) have no
        // single .tck that clearly corresponds, and a wrong map is worse than none.
        const std::map<std::string, std::string> population_abbreviation {
          { "CST",   "Corticospinal_Tract" },
          { "COBT",  "Corticobulbar_Tract" },
          { "CPT_F", "Corticopontine_Tract_Frontal" },
          { "CPT_P", "Corticopontine_Tract_Parietal" },
          { "CPT_O", "Corticopontine_Tract_Occipital" },
          { "CS_A",  "Corticostriatal_Tract_Anterior" },
          { "CS_P",  "Corticostriatal_Tract_Posterior" },
          { "CS_S",  "Corticostriatal_Tract_Superior" },
          { "TR_A",  "Thalamic_Radiation_Anterior" },
          { "TR_P",  "Thalamic_Radiation_Posterior" },
          { "TR_S",  "Thalamic_Radiation_Superior" },
          { "OR",    "Optic_Radiation" },
          { "RST",   "Reticulospinal_Tract" },
          { "F",     "Fornix" },
          { "AF",    "Arcuate_Fasciculus" },
          { "SLF",   "Superior_Longitudinal_Fasciculus" },
          { "SLF1",  "Superior_Longitudinal_Fasciculus1" },
          { "SLF2",  "Superior_Longitudinal_Fasciculus2" },
          { "SLF3",  "Superior_Longitudinal_Fasciculus3" },
          { "ILF",   "Inferior_Longitudinal_Fasciculus" },
          { "IFOF",  "Inferior_Fronto_Occipital_Fasciculus" },
          { "MdLF",  "Middle_Longitudinal_Fasciculus" },
          { "UF",    "Uncinate_Fasciculus" },
          { "VOF",   "Vertical_Occipital_Fasciculus" },
          { "FAT",   "Frontal_Aslant_Tract" },
          { "PAT",   "Parietal_Aslant_Tract" },
          { "C_FPH", "Cingulum_Frontal_Parahippocampal" },
          { "C_FP",  "Cingulum_Frontal_Parietal" },
          { "C_PH",  "Cingulum_Parahippocampal" },
          { "C_PHP", "Cingulum_Parahippocampal_Parietal" },
          { "C_PO",  "Cingulum_Parolfactory" },
          { "CC",    "Corpus_Callosum" }
        };


        //! True if \a dir actually holds population maps, not merely something.
        /*! Checked by content, not by name. The HCP1065 layout has an unrelated
         *  "ROIs" directory beside the tract atlas holding seed regions in
         *  subfolders, and guessing by name alone picked it - which is worse than
         *  finding nothing, because every bundle would then be judged against the
         *  wrong image. */
        namespace {
          bool holds_population_maps (const std::string& dir)
          {
            if (dir.empty() || !Path::is_dir (dir))
              return false;
            try {
              Path::Dir listing (dir);
              std::string entry;
              while ((entry = listing.read_name()).size()) {
                std::string stem = entry;
                for (const char* extension : { ".nii.gz", ".nii" }) {
                  const size_t n = std::strlen (extension);
                  if (stem.size() > n && stem.compare (stem.size()-n, n, extension) == 0) {
                    stem = stem.substr (0, stem.size()-n);
                    break;
                  }
                }
                if (stem == entry)
                  continue;   // not a NIfTI
                if (stem.size() > 2 && stem[stem.size()-2] == '_'
                    && (stem.back() == 'L' || stem.back() == 'R'))
                  stem = stem.substr (0, stem.size()-2);
                for (const auto& known : population_abbreviation)
                  if (known.second == stem)
                    return true;
              }
            } catch (...) { }
            return false;
          }
        }



        std::string population_directory ()
        {
          static std::mutex resolve_mutex;
          static bool resolved = false;
          static std::string cached;
          std::lock_guard<std::mutex> lock (resolve_mutex);
          if (resolved)
            return cached;
          resolved = true;

          auto usable = [] (const std::string& dir) { return holds_population_maps (dir); };

          // A path given by hand that holds no maps is a mistake worth naming; a guess
          // that misses is not.
          auto try_explicit = [&usable] (const std::string& dir, const char* source) {
            if (dir.empty())
              return false;
            if (usable (dir))
              return true;
            WARN (std::string (source) + " is set to \"" + dir + "\", which holds no "
                  "population probability maps; ignoring it");
            return false;
          };

          if (const char* env = getenv ("MRVIEWPP_TRACT_PROB_ATLAS")) {
            if (try_explicit (env, "MRVIEWPP_TRACT_PROB_ATLAS")) { cached = env; return cached; }
          }
          //CONF option: MRViewTractProbAtlasPath
          //CONF Directory of per-tract population probability maps, as NIfTI volumes
          //CONF named after the tract ("Corticospinal_Tract_L.nii.gz"). Used to reject
          //CONF streamlines running where the tract is rare across the population,
          //CONF which unlike the shape distance says nothing about how the atlas was
          //CONF tracked. The HCP1065 release calls this folder "prob/ROIs".
          const std::string configured = File::Config::get ("MRViewTractProbAtlasPath");
          if (try_explicit (configured, "MRViewTractProbAtlasPath")) { cached = configured; return cached; }

          // Beside a directory atlas, in the layouts the HCP1065 release comes in.
          const std::string tracts = atlas_directory();
          if (tracts.size()) {
            const std::string parent = Path::dirname (tracts);
            const vector<std::string> guesses {
              Path::join (parent, "prob"),
              Path::join (Path::join (parent, "prob"), "ROIs"),
              Path::join (parent, "ROIs"),
              Path::join (Path::dirname (parent), "HCP1065_prob/ROIs")
            };
            for (const std::string& guess : guesses)
              if (usable (guess)) { cached = guess; return cached; }
          }
          return cached;
        }



        //! Says once, at startup, whether population maps were found and where.
        /*! Partial coverage is expected and silent, but *no* maps at all is usually a
         *  path that needs setting, and a silent absence would look like the test
         *  simply never helping. */
        void announce_population_directory ()
        {
          static bool announced = false;
          if (announced)
            return;
          announced = true;
          const std::string dir = population_directory();
          if (dir.size())
            INFO ("population probability maps: \"" + dir + "\"");
          else
            INFO ("no population probability maps found; set MRViewTractProbAtlasPath "
                  "to the folder holding them (\"prob/ROIs\" in the HCP1065 release) to "
                  "reject streamlines running where a tract is rare across the population");
        }



        std::string population_map_path (const std::string& bundle)
        {
          const std::string dir = population_directory();
          if (dir.empty() || bundle.empty())
            return std::string();


          std::string stem = bundle, side;
          if (stem.size() > 2 && stem[stem.size()-2] == '_'
              && (stem.back() == 'L' || stem.back() == 'R')) {
            side = stem.substr (stem.size()-2);
            stem = stem.substr (0, stem.size()-2);
          }
          const auto entry = population_abbreviation.find (stem);
          if (entry == population_abbreviation.end())
            return std::string();

          const std::string base = entry->second + side;
          for (const char* extension : { ".nii.gz", ".nii" }) {
            const std::string path = Path::join (dir, base + extension);
            if (Path::is_file (path))
              return path;
          }
          return std::string();
        }



        vector<std::string> category_siblings (const std::string& bundle)
        {
          vector<std::string> out;
          // The catalogue is a directory scan (or one .trx index read) and this is
          // asked once per bundle of a run, so it is worth holding on to. The atlas
          // does not change under a running mrview.
          static std::mutex catalogue_mutex;
          static vector<BundleRef> cached;
          std::lock_guard<std::mutex> lock (catalogue_mutex);
          if (cached.empty())
            cached = list_bundles();

          std::string category;
          for (const auto& ref : cached) {
            if (ref.name == bundle) { category = ref.category; break; }
          }
          // No category means either a flat atlas (the bundled .trx) or a bundle
          // that is not in the atlas at all. Neither can answer "same category",
          // and returning every flat-atlas bundle would be a competitor set of 88.
          if (category.empty())
            return out;
          for (const auto& ref : cached)
            if (ref.category == category && ref.name != bundle)
              out.push_back (ref.name);
          return out;
        }



        vector<std::string> competitors_for (const std::string& bundle,
                                             float min_overlap,
                                             size_t max_extra)
        {
          // The category first, whole and unranked: bundles of one class are the
          // ones that can be confused for each other, and taking all of them means
          // the set does not depend on a footprint index having been built, on a
          // ranking cut-off, or on which cell size the index happened to use. It is
          // also what a reader can predict - "the projection tracts compete with the
          // projection tracts" - which a top-N overlap list is not.
          vector<std::string> out = category_siblings (bundle);

          // Territory neighbours outside the category still matter: a candidate does
          // not know what class it was meant to be, and the corticospinal tract runs
          // alongside cerebellar peduncles as readily as alongside its own kind.
          // These are added, never substituted, so the set only ever grows.
          std::set<std::string> have (out.begin(), out.end());
          for (const std::string& other : neighbours_of (bundle, min_overlap, max_extra))
            if (!have.count (other))
              out.push_back (other);
          return out;
        }



        vector<std::string> neighbours_of (const std::string& bundle,
                                           float min_overlap,
                                           size_t max_count)
        {
          vector<std::string> out;
          std::lock_guard<std::mutex> lock (footprint_mutex);
          const auto target = footprint_index.find (bundle);
          if (target == footprint_index.end() || target->second.empty())
            return out;

          vector<std::pair<float, std::string>> ranked;
          for (const auto& entry : footprint_index) {
            if (entry.first == bundle)
              continue;
            size_t shared = 0;
            // Both are sorted sets, so this is a linear merge rather than a lookup
            // per cell.
            auto a = target->second.begin(), b = entry.second.begin();
            while (a != target->second.end() && b != entry.second.end()) {
              if (*a < *b)       ++a;
              else if (*b < *a)  ++b;
              else { ++shared; ++a; ++b; }
            }
            // The fraction of the *target's* course that this bundle also covers:
            // what decides whether it can steal streamlines, independent of how
            // much larger or smaller it is overall.
            const float overlap = float (shared) / float (target->second.size());
            if (overlap >= min_overlap)
              ranked.push_back ({ overlap, entry.first });
          }
          std::sort (ranked.begin(), ranked.end(),
                     [] (const std::pair<float, std::string>& x,
                         const std::pair<float, std::string>& y) { return x.first > y.first; });
          for (size_t i = 0; i != ranked.size() && i != max_count; ++i)
            out.push_back (ranked[i].second);
          return out;
        }



        void transform_bundles (std::map<std::string, vector<MR::DWI::Tractography::Streamline<float>>>& bundles,
                                const transform_type& transform)
        {
          if (transform.matrix().isApprox (transform_type::Identity().matrix()))
            return;
          for (auto& bundle : bundles) {
            for (auto& tck : bundle.second) {
              for (auto& vertex : tck)
                vertex = (transform.cast<float>() * vertex).eval();
            }
          }
        }


      }
    }
  }
}
