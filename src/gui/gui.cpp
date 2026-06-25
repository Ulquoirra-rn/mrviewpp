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
#include <QPalette>
#include <QColor>
#include "gui/gui.h"
#include "gui/opengl/gl.h"

namespace MR
{
  namespace GUI
  {

    namespace {
      QPalette default_palette;
      bool default_palette_captured = false;

      // Build a QPalette for a named theme. Colours mirror the perspective/NiiVue
      // viewer's token sets (dark teal default, plus light and near-black variants).
      QPalette build_theme_palette (const std::string& name)
      {
        QPalette p = default_palette;
        auto set = [&] (QPalette::ColorRole role, const QColor& c) {
          p.setColor (QPalette::Active, role, c);
          p.setColor (QPalette::Inactive, role, c);
        };

        QColor window, base, alt, surface, text, muted, accent, highlight_text, bright;

        if (name == "Light") {
          window         = QColor ("#eef1f3");
          base           = QColor ("#ffffff");
          alt            = QColor ("#e4e9ec");
          surface        = QColor ("#e6eaed");
          text           = QColor ("#1b1f23");
          muted          = QColor ("#9aa3ab");
          accent         = QColor ("#2f6fdb");
          highlight_text = QColor ("#ffffff");
          bright         = QColor ("#c0392b");
        } else if (name == "Midnight") {
          window         = QColor ("#000000");
          base           = QColor ("#0a0a0a");
          alt            = QColor ("#141414");
          surface        = QColor ("#1a1a1a");
          text           = QColor ("#e0e0e0");
          muted          = QColor ("#6a6a6a");
          accent         = QColor ("#3b82f6");
          highlight_text = QColor ("#ffffff");
          bright         = QColor ("#ff5555");
        } else { // "Dark" (default) — perspective teal-dark
          window         = QColor ("#0d1c22");
          base           = QColor ("#0a1318");
          alt            = QColor ("#10262e");
          surface        = QColor ("#143038");
          text           = QColor ("#dbe9e8");
          muted          = QColor ("#5a7873");
          accent         = QColor ("#3b82f6");
          highlight_text = QColor ("#ffffff");
          bright         = QColor ("#ff5555");
        }

        set (QPalette::Window, window);
        set (QPalette::WindowText, text);
        set (QPalette::Base, base);
        set (QPalette::AlternateBase, alt);
        set (QPalette::ToolTipBase, surface);
        set (QPalette::ToolTipText, text);
        set (QPalette::Text, text);
        set (QPalette::Button, surface);
        set (QPalette::ButtonText, text);
        set (QPalette::BrightText, bright);
        set (QPalette::Highlight, accent);
        set (QPalette::HighlightedText, highlight_text);
        set (QPalette::Link, accent);
        set (QPalette::LinkVisited, accent);
        p.setColor (QPalette::Disabled, QPalette::WindowText, muted);
        p.setColor (QPalette::Disabled, QPalette::Text, muted);
        p.setColor (QPalette::Disabled, QPalette::ButtonText, muted);
        p.setColor (QPalette::PlaceholderText, muted);
        return p;
      }
    }


    std::vector<std::string> gui_theme_names ()
    {
      return { "Dark", "Light", "Midnight", "System" };
    }


    void set_gui_theme (const std::string& name)
    {
      if (!default_palette_captured)
        return;
      if (name == "System" || name.empty())
        QApplication::setPalette (default_palette);
      else
        QApplication::setPalette (build_theme_palette (name));
    }




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

      // Force Qt's cross-platform Fusion style. The native macOS widget style
      // mis-sizes the compact icon buttons in the tool docks (they render at a
      // fixed small size with gaps, looking squished/overlapping); Fusion lays
      // them out consistently and fills the row as intended.
      //CONF option: GUIStyle
      //CONF default: Fusion
      //CONF The Qt widget style to use for the GUI ("Fusion", or a platform
      //CONF native style such as "macintosh"/"windowsvista"); empty keeps Qt's
      //CONF default for the platform.
      {
        const std::string style = MR::File::Config::get ("GUIStyle", "Fusion");
        if (style.size()) {
          if (QStyle* s = QStyleFactory::create (qstr (style)))
            setStyle (s);
        }
      }

      // Capture the style's default palette (used by the "System" theme) and
      // apply the configured colour theme.
      //CONF option: GUITheme
      //CONF default: Dark
      //CONF The GUI colour theme: "Dark", "Light", "Midnight", or "System".
      default_palette = palette();
      default_palette_captured = true;
      set_gui_theme (MR::File::Config::get ("GUITheme", "Dark"));
    }



  }
}




