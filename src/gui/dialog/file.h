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

#ifndef __gui_dialog_file_h__
#define __gui_dialog_file_h__

#include <functional>

#include "file/path.h"
#include "gui/opengl/gl.h"

namespace MR
{
  namespace GUI
  {
    namespace Dialog
    {
      namespace File
      {

        extern const std::string image_filter_string;
        void check_overwrite_files_func (const std::string& name);

        // For tools that can save several selected items at once: ask whether to
        // combine them into a single file or write one file each into a folder.
        enum class MultiSaveMode { Cancel, SingleFile, Folder };

        // Result of ask_multi_save_mode: the chosen mode plus, for the Folder
        // branch, the file extension picked from the format drop-down (e.g.
        // ".nii.gz"). The extension is empty for Cancel/SingleFile.
        struct MultiSaveChoice {
          MultiSaveMode mode;
          std::string extension;
        };

        // `folder_formats` lists the extensions to offer for the per-file
        // (Folder) case; the first entry is the default selection.
        MultiSaveChoice ask_multi_save_mode (QWidget* parent, const std::string& what,
                                             const vector<std::string>& folder_formats);

        std::string get_folder (QWidget* parent, const std::string& caption, std::string* folder = nullptr);
        std::string get_file (QWidget* parent, const std::string& caption, const std::string& filter = std::string(), std::string* folder = nullptr);
#ifdef MRTRIX_WASM
        // Browser: async file open (writes to MEMFS, then cb(path); empty if cancelled).
        void get_file_async (const std::string& filter, std::function<void(const std::string&)> cb);
        inline void get_image_async (std::function<void(const std::string&)> cb) { get_file_async (image_filter_string, cb); }
#endif
        vector<std::string> get_files (QWidget* parent, const std::string& caption, const std::string& filter = std::string(), std::string* folder = nullptr);
        std::string get_save_name (QWidget* parent, const std::string& caption, const std::string& suggested_name = std::string(), const std::string& filter = std::string(), std::string* folder = nullptr);

        inline std::string get_image (QWidget* parent, const std::string& caption, std::string* folder = nullptr) {
          return get_file (parent, caption, image_filter_string, folder);
        }

        inline vector<std::string> get_images (QWidget* parent, const std::string& caption, std::string* folder = nullptr) {
          return get_files (parent, caption, image_filter_string, folder);
        }

        inline std::string get_save_image_name (QWidget* parent, const std::string& caption, const std::string& suggested_name = std::string(), std::string* folder = nullptr) {
          return get_save_name (parent, caption, suggested_name, image_filter_string, folder);
        }

      }
    }
  }
}

#endif

