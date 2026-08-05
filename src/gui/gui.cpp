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

#include "types.h"

#ifndef MRTRIX_MACOSX
#include <QPalette>
#include <QStyleFactory>
#endif

#include "gui/gui.h"
#include "gui/opengl/gl.h"

namespace MR
{
  namespace GUI
  {



    QWidget* App::main_window = nullptr;
    App* App::application = nullptr;



#ifndef MRTRIX_MACOSX
    namespace
    {
      // A dark palette applied on top of the Fusion style.
      //
      // Deliberately NOT used on macOS: QMacStyle already follows the system
      // light/dark setting and looks native, and it is what the .app bundle goes
      // out of its way to preserve (see package_macos_app.sh). Overriding it there
      // made the interface look wrong.
      //
      // On Windows and Linux the situation is the reverse: the native Windows style
      // largely ignores palettes and both platforms' defaults look dated, so Fusion
      // is both the modern-looking option and the only one that takes a palette.
      void apply_dark_theme (QApplication& app)
      {
        app.setStyle (QStyleFactory::create ("Fusion"));

        const QColor window      (53, 53, 53);
        const QColor base        (35, 35, 35);
        const QColor alt_base    (45, 45, 45);
        const QColor text        (220, 220, 220);
        const QColor disabled    (127, 127, 127);
        const QColor highlight   (42, 130, 218);
        const QColor button      (60, 60, 60);

        QPalette p;
        p.setColor (QPalette::Window,          window);
        p.setColor (QPalette::WindowText,      text);
        p.setColor (QPalette::Base,            base);
        p.setColor (QPalette::AlternateBase,   alt_base);
        p.setColor (QPalette::ToolTipBase,     base);
        p.setColor (QPalette::ToolTipText,     text);
        p.setColor (QPalette::Text,            text);
        p.setColor (QPalette::Button,          button);
        p.setColor (QPalette::ButtonText,      text);
        p.setColor (QPalette::BrightText,      Qt::red);
        p.setColor (QPalette::Link,            highlight);
        p.setColor (QPalette::Highlight,       highlight);
        p.setColor (QPalette::HighlightedText, Qt::black);
        p.setColor (QPalette::PlaceholderText, disabled);

        // Without explicit disabled roles, greyed-out controls stay light and look
        // broken against the rest.
        p.setColor (QPalette::Disabled, QPalette::WindowText,      disabled);
        p.setColor (QPalette::Disabled, QPalette::Text,            disabled);
        p.setColor (QPalette::Disabled, QPalette::ButtonText,      disabled);
        p.setColor (QPalette::Disabled, QPalette::Highlight,       QColor (80, 80, 80));
        p.setColor (QPalette::Disabled, QPalette::HighlightedText, disabled);

        app.setPalette (p);
      }
    }
#endif



    App::App (int& cmdline_argc, char** cmdline_argv) :
      QApplication (cmdline_argc, cmdline_argv)
    {
      application = this;
      ::MR::File::Config::init ();
      ::MR::GUI::GL::set_default_context ();

#ifndef MRTRIX_MACOSX
      //CONF option: MRViewDarkMode
      //CONF default: true
      //CONF Render the interface with the Fusion style and a dark palette. Has no
      //CONF effect on macOS, which uses the native style and follows the system
      //CONF light/dark setting. Set to false on Windows or Linux to get the
      //CONF platform's own style instead.
      if (::MR::File::Config::get_bool ("MRViewDarkMode", true))
        apply_dark_theme (*this);
#endif

      QLocale::setDefault(QLocale::c());
      std::locale::global (std::locale::classic());
      std::setlocale (LC_ALL, "C");

      setAttribute (Qt::AA_DontCreateNativeWidgetSiblings);
    }



  }
}
