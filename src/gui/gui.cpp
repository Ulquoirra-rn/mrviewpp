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

      // Material-Design colour roles for a theme.
      struct ThemeColors { NOMEMALIGN
        QColor window;       // surface (panel background)
        QColor base;         // deepest background (canvas / list)
        QColor alt;          // alternate row
        QColor surface;      // raised surface / input (surface-variant)
        QColor hover;        // hover overlay
        QColor text;         // on-surface
        QColor text2;        // on-surface-variant (secondary)
        QColor muted;        // disabled
        QColor primary;      // accent
        QColor on_primary;   // text on accent
        QColor outline;      // borders
        QColor bright;       // bright/error text
      };

      ThemeColors theme_colors (const std::string& name)
      {
        ThemeColors c;
        if (name == "Light") {
          c.window="#f3f5f7"; c.base="#ffffff"; c.alt="#eef1f3"; c.surface="#e9edf1"; c.hover="#dde4ea";
          c.text="#1b1f23"; c.text2="#4a5560"; c.muted="#9aa3ab";
          c.primary="#2f6fdb"; c.on_primary="#ffffff"; c.outline="#cdd6dc"; c.bright="#c0392b";
        } else if (name == "Midnight") {
          c.window="#0a0a0a"; c.base="#000000"; c.alt="#141414"; c.surface="#1a1a1a"; c.hover="#262626";
          c.text="#e8e8e8"; c.text2="#9a9a9a"; c.muted="#6a6a6a";
          c.primary="#4f9cf9"; c.on_primary="#06121d"; c.outline="#2c2c2c"; c.bright="#ff6b6b";
        } else { // "Dark" (default) — teal-dark Material
          c.window="#0d1c22"; c.base="#0a1318"; c.alt="#10262e"; c.surface="#143038"; c.hover="#1d4450";
          c.text="#e6ecec"; c.text2="#8eb1ae"; c.muted="#5a7873";
          c.primary="#4f9cf9"; c.on_primary="#06121d"; c.outline="#244049"; c.bright="#ff6b6b";
        }
        return c;
      }

      QPalette build_theme_palette (const std::string& name)
      {
        const ThemeColors c = theme_colors (name);
        QPalette p = default_palette;
        auto set = [&] (QPalette::ColorRole role, const QColor& col) {
          p.setColor (QPalette::Active, role, col);
          p.setColor (QPalette::Inactive, role, col);
        };
        set (QPalette::Window, c.window);
        set (QPalette::WindowText, c.text);
        set (QPalette::Base, c.base);
        set (QPalette::AlternateBase, c.alt);
        set (QPalette::ToolTipBase, c.surface);
        set (QPalette::ToolTipText, c.text);
        set (QPalette::Text, c.text);
        set (QPalette::Button, c.surface);
        set (QPalette::ButtonText, c.text);
        set (QPalette::BrightText, c.bright);
        set (QPalette::Highlight, c.primary);
        set (QPalette::HighlightedText, c.on_primary);
        set (QPalette::Link, c.primary);
        set (QPalette::LinkVisited, c.primary);
        p.setColor (QPalette::Disabled, QPalette::WindowText, c.muted);
        p.setColor (QPalette::Disabled, QPalette::Text, c.muted);
        p.setColor (QPalette::Disabled, QPalette::ButtonText, c.muted);
        p.setColor (QPalette::PlaceholderText, c.muted);
        return p;
      }

      // A Material-Design widget stylesheet: rounded surfaces, tonal/accent
      // buttons, Material sliders/scrollbars, pill list selection, accented
      // group-box titles and tabs.
      QString build_stylesheet (const std::string& name)
      {
        const ThemeColors c = theme_colors (name);
        auto H = [] (const QColor& col) { return col.name(); };
        QString s;
        s += "* { outline: 0; }\n";
        s += QString ("QToolTip { background:%1; color:%2; border:1px solid %3; border-radius:6px; padding:4px 6px; }\n")
                .arg (H(c.surface), H(c.text), H(c.outline));
        s += QString ("QToolBar { background:%1; border:0; spacing:4px; padding:3px; }\n").arg (H(c.window));
        // Push buttons: rounded tonal buttons, accent on hover/checked.
        s += QString ("QPushButton { background:%1; color:%2; border:0; border-radius:14px; padding:6px 14px; }\n"
                      "QPushButton:hover { background:%3; }\n"
                      "QPushButton:pressed, QPushButton:checked { background:%4; color:%5; }\n"
                      "QPushButton:disabled { color:%6; background:%1; }\n")
                .arg (H(c.surface), H(c.text), H(c.hover), H(c.primary), H(c.on_primary), H(c.muted));
        // Tool buttons: flat, rounded hover.
        s += QString ("QToolButton { background:transparent; border:0; border-radius:8px; padding:5px; }\n"
                      "QToolButton:hover { background:%1; }\n"
                      "QToolButton:checked, QToolButton:pressed { background:%2; color:%3; }\n")
                .arg (H(c.hover), H(c.primary), H(c.on_primary));
        // Text inputs / combos.
        s += QString ("QLineEdit, QAbstractSpinBox, QComboBox { background:%1; color:%2; border:1px solid %3; "
                      "border-radius:8px; padding:4px 8px; selection-background-color:%4; selection-color:%5; }\n"
                      "QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus { border:1px solid %4; }\n"
                      "QComboBox QAbstractItemView { background:%6; border:1px solid %3; border-radius:8px; "
                      "selection-background-color:%4; selection-color:%5; }\n")
                .arg (H(c.surface), H(c.text), H(c.outline), H(c.primary), H(c.on_primary), H(c.window));
        // Group boxes: rounded card with accent title.
        s += QString ("QGroupBox { border:1px solid %1; border-radius:12px; margin-top:12px; padding:8px 6px 6px 6px; }\n"
                      "QGroupBox::title { subcontrol-origin:margin; subcontrol-position:top left; left:12px; "
                      "padding:0 4px; color:%2; }\n")
                .arg (H(c.outline), H(c.primary));
        // Lists / trees: card with pill selection.
        s += QString ("QListView, QListWidget, QTreeView, QTreeWidget, QTableView { background:%1; border:1px solid %2; "
                      "border-radius:10px; padding:2px; }\n"
                      "QListView::item, QListWidget::item, QTreeView::item { padding:4px; border-radius:6px; }\n"
                      "QListView::item:hover, QListWidget::item:hover, QTreeView::item:hover { background:%3; }\n"
                      "QListView::item:selected, QListWidget::item:selected, QTreeView::item:selected { background:%4; color:%5; }\n")
                .arg (H(c.base), H(c.outline), H(c.hover), H(c.primary), H(c.on_primary));
        // Sliders: Material track + circular thumb.
        s += QString ("QSlider::groove:horizontal { height:4px; background:%1; border-radius:2px; }\n"
                      "QSlider::sub-page:horizontal { background:%2; border-radius:2px; }\n"
                      "QSlider::handle:horizontal { background:%2; width:16px; height:16px; margin:-6px 0; border-radius:8px; }\n"
                      "QSlider::groove:vertical { width:4px; background:%1; border-radius:2px; }\n"
                      "QSlider::add-page:vertical { background:%2; border-radius:2px; }\n"
                      "QSlider::handle:vertical { background:%2; width:16px; height:16px; margin:0 -6px; border-radius:8px; }\n")
                .arg (H(c.outline), H(c.primary));
        // Scrollbars: thin rounded.
        s += QString ("QScrollBar:vertical { background:transparent; width:10px; margin:0; }\n"
                      "QScrollBar::handle:vertical { background:%1; border-radius:5px; min-height:24px; }\n"
                      "QScrollBar::handle:vertical:hover { background:%2; }\n"
                      "QScrollBar:horizontal { background:transparent; height:10px; margin:0; }\n"
                      "QScrollBar::handle:horizontal { background:%1; border-radius:5px; min-width:24px; }\n"
                      "QScrollBar::handle:horizontal:hover { background:%2; }\n"
                      "QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }\n"
                      "QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }\n")
                .arg (H(c.outline), H(c.text2));
        // Menus.
        s += QString ("QMenu { background:%1; border:1px solid %2; border-radius:8px; padding:4px; }\n"
                      "QMenu::item { padding:5px 22px; border-radius:6px; }\n"
                      "QMenu::item:selected { background:%3; color:%4; }\n"
                      "QMenuBar { background:%5; }\n"
                      "QMenuBar::item:selected { background:%6; border-radius:6px; }\n")
                .arg (H(c.window), H(c.outline), H(c.primary), H(c.on_primary), H(c.window), H(c.hover));
        // Tabs (tool docks) + dock titles.
        s += QString ("QTabBar::tab { background:%1; color:%2; padding:7px 10px; border-radius:8px; margin:2px; }\n"
                      "QTabBar::tab:selected { background:%3; color:%4; }\n"
                      "QDockWidget::title { background:%1; padding:6px; border-top-left-radius:8px; border-top-right-radius:8px; }\n")
                .arg (H(c.surface), H(c.text2), H(c.primary), H(c.on_primary));
        return s;
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
      if (name == "System" || name.empty()) {
        QApplication::setPalette (default_palette);
        if (App::application)
          App::application->setStyleSheet (QString());
        return;
      }
      QApplication::setPalette (build_theme_palette (name));
      if (App::application)
        App::application->setStyleSheet (build_stylesheet (name));
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




