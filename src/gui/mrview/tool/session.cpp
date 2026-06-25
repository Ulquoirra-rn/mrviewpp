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

        // Maps a session JSON key to the menu name of the tool that owns it.
        // The tool name must match the Action text declared in tool/list.h.
        static const std::vector<std::pair<std::string, std::string>> session_key_to_tool = {
          { "overlays", "Overlay" },
          { "tracts",   "Tractography" },
          { "meshes",   "Mesh display" },
          { "atlases",  "Atlas" }
        };



        Session::Session (Dock* parent) :
            Base (parent)
        {
          VBoxLayout* main_box = new VBoxLayout (this);

          QPushButton* save_button = new QPushButton ("Save session...", this);
          save_button->setToolTip (tr ("Save all loaded images, overlays, tracts, meshes and atlases to a session file"));
          connect (save_button, SIGNAL (clicked()), this, SLOT (save_slot ()));
          main_box->addWidget (save_button);

          QPushButton* open_button = new QPushButton ("Open session...", this);
          open_button->setToolTip (tr ("Restore a previously saved session"));
          connect (open_button, SIGNAL (clicked()), this, SLOT (open_slot ()));
          main_box->addWidget (open_button);

          status_label = new QLabel ("No session loaded.");
          status_label->setWordWrap (true);
          main_box->addWidget (status_label);

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

          try {
            nlohmann::json j;
            j["main"] = main_image_filenames();

            QList<QAction*> actions = window().tools()->actions();
            for (int i = 0; i < actions.size(); ++i) {
              __Action__* tool_action = dynamic_cast<__Action__*> (actions[i]);
              if (!tool_action || !tool_action->dock || !tool_action->dock->tool)
                continue;
              const std::string key = tool_action->dock->tool->session_key();
              if (key.empty())
                continue;
              nlohmann::json node;
              tool_action->dock->tool->get_session (node);
              j[key] = node;
            }

            std::ofstream out (path);
            if (!out)
              throw Exception ("unable to open session file \"" + path + "\" for writing");
            out << j.dump (2) << "\n";
            status_label->setText (qstr ("Saved session to " + path));
          } catch (Exception& e) {
            e.display();
            status_label->setText ("Failed to save session.");
          }
        }



        void Session::open_slot ()
        {
          const std::string path = Dialog::File::get_file (this, "Open session", "JSON files (*.json)");
          if (path.empty())
            return;

          try {
            std::ifstream in (path);
            if (!in)
              throw Exception ("unable to open session file \"" + path + "\"");
            nlohmann::json j;
            in >> j;

            if (j.find ("main") != j.end() && j["main"].is_array())
              load_main_images (j["main"].get<vector<std::string>>());

            for (const auto& kv : session_key_to_tool) {
              if (j.find (kv.first) == j.end())
                continue;
              Dock* dock = ensure_tool_open (kv.second);
              if (dock && dock->tool)
                dock->tool->set_session (j[kv.first]);
            }

            window().updateGL();
            status_label->setText (qstr ("Loaded session from " + path));
          } catch (Exception& e) {
            e.display();
            status_label->setText ("Failed to load session.");
          }
        }


      }
    }
  }
}
