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

#include <fstream>
#include <utility>

#include "file/json.h"
#include "gui/mrview/window.h"
#include "gui/mrview/tool/session.h"
#include "gui/dialog/file.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {

        Session::Session (Dock* parent) :
            Base (parent)
        {
          VBoxLayout* main_box = new VBoxLayout (this);

          QGroupBox* box = new QGroupBox (tr ("Session file"));
          main_box->addWidget (box);
          VBoxLayout* box_layout = new VBoxLayout;
          box->setLayout (box_layout);

          QPushButton* save_button = new QPushButton (tr ("Save session..."), this);
          save_button->setToolTip (tr ("Save all loaded images, overlays, tracts, meshes and atlases to a session file"));
          connect (save_button, SIGNAL (clicked()), this, SLOT (save_slot ()));
          box_layout->addWidget (save_button);

          QPushButton* open_button = new QPushButton (tr ("Open session..."), this);
          open_button->setToolTip (tr ("Restore a previously saved session"));
          connect (open_button, SIGNAL (clicked()), this, SLOT (open_slot ()));
          box_layout->addWidget (open_button);

          QPushButton* restore_button = new QPushButton (tr ("Restore auto-saved session"), this);
          restore_button->setToolTip (tr ("Reload the session that is automatically saved to your home directory "
                                          "(also on the File menu, Ctrl+Shift+R)"));
          connect (restore_button, SIGNAL (clicked()), this, SLOT (restore_autosave_slot ()));
          box_layout->addWidget (restore_button);

          status_label = new QLabel (tr ("No session loaded."));
          status_label->setWordWrap (true);
          box_layout->addWidget (status_label);

          main_box->addStretch (1);
        }



        Dock* Session::ensure_tool_open (const std::string& tool_name)
        {
          QList<QAction*> actions = window().tools()->actions();
          for (int i = 0; i < actions.size(); ++i) {
            QAction* action = actions[i];
            if (action->text().toStdString() != tool_name)
              continue;
            __Action__* tool_action = dynamic_cast<__Action__*> (action);
            if (!tool_action)
              return nullptr;
            if (!tool_action->dock)
              action->trigger();   // opens the dock synchronously via select_tool_slot
            return tool_action->dock;
          }
          return nullptr;
        }



        void Session::save_slot ()
        {
          const std::string path = Dialog::File::get_save_name (this, "Save session", "session.json");
          if (path.empty())
            return;

          if (window().save_session (path))
            status_label->setText (qstr ("Saved session to " + path));
          else
            status_label->setText ("Failed to save session.");
        }



        void Session::open_slot ()
        {
          const std::string path = Dialog::File::get_file (this, "Open session", "JSON files (*.json)");
          if (path.empty())
            return;

          if (window().load_session (path))
            status_label->setText (qstr ("Loaded session from " + path));
          else
            status_label->setText ("Failed to load session.");
        }



        void Session::add_commandline_options (MR::App::OptionList& options)
        {
          using namespace MR::App;
          options
            + OptionGroup ("Session tool options")

            + Option ("session.load", "Restore a saved session: the images, the tools that were "
                                      "open, and what each of them held.").allow_multiple()
            +   Argument ("file").type_file_in()

            + Option ("session.save", "Save the current scene as a session file and carry on.").allow_multiple()
            +   Argument ("file").type_file_out();
        }



        bool Session::process_commandline_option (const MR::App::ParsedOption& opt)
        {
          if (opt.opt->is ("session.load")) {
            const std::string path (opt[0]);
            if (window().load_session (path))
              status_label->setText (qstr ("Loaded session from " + path));
            else
              throw Exception ("failed to load session \"" + path + "\"");
            return true;
          }

          if (opt.opt->is ("session.save")) {
            const std::string path (opt[0]);
            if (window().save_session (path))
              status_label->setText (qstr ("Saved session to " + path));
            else
              throw Exception ("failed to save session \"" + path + "\"");
            return true;
          }

          return false;
        }



        void Session::restore_autosave_slot ()
        {
          const std::string path = Window::autosave_session_path();
          if (path.empty() || !Path::is_file (path)) {
            status_label->setText ("No auto-saved session found.");
            return;
          }
          if (window().load_session (path))
            status_label->setText (qstr ("Restored auto-saved session from " + path));
          else
            status_label->setText ("Failed to restore auto-saved session.");
        }


      }
    }
  }
}
