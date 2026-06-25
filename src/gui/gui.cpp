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

#include <locale>
#include <clocale>
#include <QStyleFactory>
#include <QStyle>
#include "gui/gui.h"
#include "gui/opengl/gl.h"

namespace MR
{
  namespace GUI
  {



    QWidget* App::main_window = nullptr;
    App* App::application = nullptr;



    App::App (int& cmdline_argc, char** cmdline_argv) :
      QApplication (cmdline_argc, cmdline_argv)
    {
      application = this;
      ::MR::File::Config::init ();
      ::MR::GUI::GL::set_default_context ();

      QLocale::setDefault(QLocale::c());
      std::locale::global (std::locale::classic());
      std::setlocale (LC_ALL, "C");

      setAttribute (Qt::AA_DontCreateNativeWidgetSiblings);

      // mrview's official macOS build runs as a packaged .app bundle, which gets
      // the native macOS widget style. When run as a raw binary, Qt falls back to
      // the Fusion style; explicitly request the native "macintosh" style so the
      // GUI matches the official build (sliders, checkboxes, spin boxes, etc.).
      //CONF option: GUIStyle
      //CONF default: macintosh on macOS (the native style); empty elsewhere
      //CONF The Qt widget style to use for the GUI. Empty keeps Qt's platform
      //CONF default. Set to a QStyleFactory key such as "Fusion" to override.
#ifdef MRTRIX_MACOSX
      const std::string default_style = "macintosh";
#else
      const std::string default_style;
#endif
      const std::string stylename = MR::File::Config::get ("GUIStyle", default_style);
      if (stylename.size()) {
        if (QStyle* s = QStyleFactory::create (qstr (stylename)))
          setStyle (s);
      }
    }



  }
}
