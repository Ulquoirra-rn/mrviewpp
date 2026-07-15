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

#include <QMessageBox>
#include <QDialog>
#include <QComboBox>
#include <QRadioButton>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFileDialog>
#ifdef MRTRIX_WASM
#include <emscripten.h>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <functional>
#endif

#include "app.h"
#include "gui/gui.h"
#include "gui/dialog/file.h"
#include "formats/list.h"

// Use the operating system's native file picker on all platforms.
#define FILE_DIALOG_OPTIONS QFileDialog::Options()

namespace MR
{
  namespace GUI
  {
    namespace Dialog
    {
      namespace File
      {

        const std::string image_filter_string = "Medical Images (*" + join (MR::Formats::known_extensions, " *") + ")";




        MultiSaveChoice ask_multi_save_mode (QWidget* parent, const std::string& what,
                                             const vector<std::string>& folder_formats)
        {
          QDialog dialog (parent);
          dialog.setWindowTitle (qstr ("Save " + what));

          QVBoxLayout* layout = new QVBoxLayout (&dialog);

          QLabel* prompt = new QLabel (qstr ("Saving " + what
                + ".\nSave as a single combined file, or as individual files in a folder?"), &dialog);
          prompt->setWordWrap (true);
          layout->addWidget (prompt);

          QRadioButton* single = new QRadioButton ("Single combined file", &dialog);
          QRadioButton* folder = new QRadioButton ("Individual files in a folder", &dialog);
          single->setChecked (true);
          layout->addWidget (single);
          layout->addWidget (folder);

          // Format drop-down applies to the per-file (folder) case; enabled only
          // when that mode is selected.
          QHBoxLayout* fmt_row = new QHBoxLayout;
          QLabel* fmt_label = new QLabel ("File format:", &dialog);
          QComboBox* fmt_combo = new QComboBox (&dialog);
          for (const auto& ext : folder_formats)
            fmt_combo->addItem (qstr (ext));
          fmt_label->setEnabled (false);
          fmt_combo->setEnabled (false);
          QObject::connect (folder, &QRadioButton::toggled, fmt_label, &QWidget::setEnabled);
          QObject::connect (folder, &QRadioButton::toggled, fmt_combo, &QWidget::setEnabled);
          fmt_row->addWidget (fmt_label);
          fmt_row->addWidget (fmt_combo, 1);
          layout->addLayout (fmt_row);

          QDialogButtonBox* buttons = new QDialogButtonBox (
              QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
          QObject::connect (buttons, SIGNAL (accepted()), &dialog, SLOT (accept()));
          QObject::connect (buttons, SIGNAL (rejected()), &dialog, SLOT (reject()));
          layout->addWidget (buttons);

          MultiSaveChoice choice { MultiSaveMode::Cancel, std::string() };
          if (dialog.exec() != QDialog::Accepted)
            return choice;

          if (folder->isChecked()) {
            choice.mode = MultiSaveMode::Folder;
            choice.extension = fmt_combo->currentText().toUtf8().data();
          } else {
            choice.mode = MultiSaveMode::SingleFile;
          }
          return choice;
        }



#ifdef MRTRIX_WASM

        // Async browser file open: Qt-wasm's event loop already uses ASYNCIFY, so
        // we must NOT block. getOpenFileContent runs the browser picker and fires
        // the callback later; we write the bytes into MEMFS and hand the caller a
        // path (empty if cancelled) so Header::open(path) works unchanged.
        void get_file_async (const std::string& filter, std::function<void(const std::string&)> cb)
        {
          QFileDialog::getOpenFileContent (qstr (filter),
              [cb] (const QString& name, const QByteArray& content) {
                std::string path;
                if (!name.isEmpty()) {
                  QDir().mkpath ("/uploads");
                  path = "/uploads/" + QFileInfo (name).fileName().toStdString();
                  QFile f (qstr (path));
                  if (f.open (QIODevice::WriteOnly)) { f.write (content); f.close(); }
                  else path.clear();
                }
                cb (path);
              });
        }

        // Synchronous open is not possible in the browser (would deadlock the
        // ASYNCIFY event loop); slots use get_file_async instead.
        std::string get_file (QWidget*, const std::string&, const std::string&, std::string*)
        { return std::string(); }

        vector<std::string> get_files (QWidget*, const std::string&, const std::string&, std::string*)
        { return vector<std::string>(); }

        std::string get_folder (QWidget*, const std::string&, std::string*)
        {
          QMessageBox::information (QApplication::activeWindow(), "Open folder",
              "Folder selection is not available in the web version yet.");
          return std::string();
        }

#else

        std::string get_folder (QWidget* parent, const std::string& caption, std::string* folder)
        {
          QString qstring = QFileDialog::getExistingDirectory (parent, qstr (caption),
              folder ? qstr (*folder) : QString(), QFileDialog::ShowDirsOnly | FILE_DIALOG_OPTIONS);

          std::string new_folder;
          if (qstring.size()) {
            new_folder = qstring.toUtf8().data();
            if (folder)
              *folder = new_folder;
          }
          return new_folder;
        }




        std::string get_file (QWidget* parent, const std::string& caption, const std::string& filter, std::string* folder)
        {
          QString qstring = QFileDialog::getOpenFileName (parent, qstr (caption),
              folder ? qstr(*folder) : QString(), qstr(filter), 0, FILE_DIALOG_OPTIONS);

          std::string filename;
          if (qstring.size()) {
            filename = qstring.toUtf8().data();
            std::string new_folder = Path::dirname (filename);
            if (folder)
              *folder = new_folder;
          }
          return filename;
        }





        vector<std::string> get_files (QWidget* parent, const std::string& caption, const std::string& filter, std::string* folder)
        {
          QStringList qlist = QFileDialog::getOpenFileNames (parent, qstr(caption),
              folder ? qstr(*folder) : QString(), qstr(filter), 0, FILE_DIALOG_OPTIONS);

          vector<std::string> list;
          if (qlist.size()) {
            for (int n = 0; n < qlist.size(); ++n)
              list.push_back (qlist[n].toUtf8().data());
            std::string new_folder = Path::dirname (list[0]);
            if (folder)
              *folder = new_folder;
          }
          return list;
        }

#endif // MRTRIX_WASM


        bool overwrite_files = false;

        void check_overwrite_files_func (const std::string& name)
        {
          if (overwrite_files)
            return;

          QMessageBox::StandardButton response = QMessageBox::warning (QApplication::activeWindow(),
              qstr ("confirm file overwrite"),
              qstr ("Action will overwrite file \"" + name + "\" - proceed?"),
              QMessageBox::Yes | QMessageBox::YesToAll | QMessageBox::Cancel, QMessageBox::Cancel);
          if (response == QMessageBox::Cancel)
            throw Exception ("File overwrite cancelled by user request");
          if (response == QMessageBox::YesToAll)
            overwrite_files = true;
        }



        std::string get_save_name (QWidget* parent, const std::string& caption, const std::string& suggested_name, const std::string& filter, std::string* folder)
        {
          overwrite_files = false;

          QString selection;
          if (folder) {
            if (suggested_name.size())
              selection = qstr (MR::Path::join (*folder, suggested_name));
            else
              selection = qstr (*folder);
          }
          else if (suggested_name.size())
            selection = qstr (suggested_name);

          QString qstring = QFileDialog::getSaveFileName (parent, qstr (caption), selection,
              qstr (filter), 0, FILE_DIALOG_OPTIONS | QFileDialog::DontConfirmOverwrite);

          std::string filename;
          if (qstring.size()) {
            filename = qstring.toUtf8().data();
            std::string new_folder = Path::dirname (filename);
            if (folder)
              *folder = new_folder;
          }
          return filename;
        }

      }
    }
  }
}


