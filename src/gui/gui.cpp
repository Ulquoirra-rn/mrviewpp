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
#include <QString>
#include "gui/gui.h"
#include "gui/opengl/gl.h"

namespace MR
{
  namespace GUI
  {

    namespace {

      QString hex (const QColor& c) { return c.name(); }

      QColor mix (const QColor& a, const QColor& b, double t) {
        return QColor (int (a.red()   + (b.red()   - a.red())   * t),
                       int (a.green() + (b.green() - a.green()) * t),
                       int (a.blue()  + (b.blue()  - a.blue())  * t));
      }

      // A Material-Design widget stylesheet derived from the application's own
      // palette, so it adopts whatever colour theme is active (the platform /
      // Fusion default) while giving everything the Material look: rounded tonal
      // buttons with accent states, card-style group boxes, pill list selection,
      // Material sliders, thin rounded scrollbars, accent tabs, rounded menus.
      QString material_stylesheet (const QPalette& pal)
      {
        const QColor window     = pal.color (QPalette::Window);
        const QColor base       = pal.color (QPalette::Base);
        const QColor surface    = pal.color (QPalette::Button);
        const QColor text       = pal.color (QPalette::WindowText);
        const QColor text2      = pal.color (QPalette::Disabled, QPalette::WindowText);
        const QColor muted      = pal.color (QPalette::Disabled, QPalette::Text);
        const QColor primary    = pal.color (QPalette::Highlight);
        const QColor on_primary = pal.color (QPalette::HighlightedText);
        const bool dark         = window.lightnessF() < 0.5;
        const QColor hover      = dark ? surface.lighter (135) : surface.darker (108);
        const QColor outline    = mix (window, text, 0.42);

        QString s;
        s += "* { outline: 0; }\n";
        s += QString ("QToolTip { background:%1; color:%2; border:1px solid %3; border-radius:6px; padding:4px 6px; }\n")
                .arg (hex(surface), hex(text), hex(outline));
        s += QString ("QToolBar { background:%1; border:0; spacing:4px; padding:3px; }\n").arg (hex(window));
        s += QString ("QPushButton { background:%1; color:%2; border:1px solid %7; border-radius:10px; padding:4px 10px; }\n"
                      "QPushButton:hover { background:%3; border-color:%4; }\n"
                      "QPushButton:pressed, QPushButton:checked { background:%4; color:%5; border-color:%4; }\n"
                      "QPushButton:disabled { color:%6; background:%1; border-color:%7; }\n"
                      "QPushButton#batchbtn { padding:2px 6px; font-size:11px; border-radius:8px; }\n")
                .arg (hex(surface), hex(text), hex(hover), hex(primary), hex(on_primary), hex(muted), hex(outline));
        s += QString ("QToolButton { background:transparent; border:0; border-radius:8px; padding:5px; }\n"
                      "QToolButton:hover { background:%1; }\n"
                      "QToolButton:checked, QToolButton:pressed { background:%2; color:%3; }\n")
                .arg (hex(hover), hex(primary), hex(on_primary));
        s += QString ("QLineEdit, QAbstractSpinBox, QComboBox { background:%1; color:%2; border:1px solid %3; "
                      "border-radius:8px; padding:4px 8px; selection-background-color:%4; selection-color:%5; }\n"
                      "QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus { border:1px solid %4; }\n"
                      "QComboBox QAbstractItemView { background:%6; border:1px solid %3; border-radius:8px; "
                      "selection-background-color:%4; selection-color:%5; }\n")
                .arg (hex(surface), hex(text), hex(outline), hex(primary), hex(on_primary), hex(window));
        s += QString ("QGroupBox { border:1px solid %1; border-radius:12px; margin-top:12px; padding:8px 6px 6px 6px; }\n"
                      "QGroupBox::title { subcontrol-origin:margin; subcontrol-position:top left; left:12px; "
                      "padding:0 4px; color:%2; }\n")
                .arg (hex(outline), hex(primary));
        s += QString ("QListView, QListWidget, QTreeView, QTreeWidget, QTableView { background:%1; border:1px solid %2; "
                      "border-radius:10px; padding:2px; }\n"
                      "QListView::item, QListWidget::item, QTreeView::item { padding:4px; border-radius:6px; }\n"
                      "QListView::item:hover, QListWidget::item:hover, QTreeView::item:hover { background:%3; }\n"
                      "QListView::item:selected, QListWidget::item:selected, QTreeView::item:selected { background:%4; color:%5; }\n")
                .arg (hex(base), hex(outline), hex(hover), hex(primary), hex(on_primary));
        s += QString ("QSlider::groove:horizontal { height:4px; background:%1; border-radius:2px; }\n"
                      "QSlider::sub-page:horizontal { background:%2; border-radius:2px; }\n"
                      "QSlider::handle:horizontal { background:%2; width:16px; height:16px; margin:-6px 0; border-radius:8px; }\n"
                      "QSlider::groove:vertical { width:4px; background:%1; border-radius:2px; }\n"
                      "QSlider::add-page:vertical { background:%2; border-radius:2px; }\n"
                      "QSlider::handle:vertical { background:%2; width:16px; height:16px; margin:0 -6px; border-radius:8px; }\n")
                .arg (hex(outline), hex(primary));
        s += QString ("QScrollBar:vertical { background:transparent; width:10px; margin:0; }\n"
                      "QScrollBar::handle:vertical { background:%1; border-radius:5px; min-height:24px; }\n"
                      "QScrollBar::handle:vertical:hover { background:%2; }\n"
                      "QScrollBar:horizontal { background:transparent; height:10px; margin:0; }\n"
                      "QScrollBar::handle:horizontal { background:%1; border-radius:5px; min-width:24px; }\n"
                      "QScrollBar::handle:horizontal:hover { background:%2; }\n"
                      "QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }\n"
                      "QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }\n")
                .arg (hex(outline), hex(text2));
        s += QString ("QMenu { background:%1; border:1px solid %2; border-radius:8px; padding:4px; }\n"
                      "QMenu::item { padding:5px 22px; border-radius:6px; }\n"
                      "QMenu::item:selected { background:%3; color:%4; }\n"
                      "QMenuBar::item:selected { background:%5; border-radius:6px; }\n")
                .arg (hex(window), hex(outline), hex(primary), hex(on_primary), hex(hover));
        s += QString ("QTabBar::tab { background:%1; color:%2; padding:7px 10px; border-radius:8px; margin:2px; }\n"
                      "QTabBar::tab:selected { background:%3; color:%4; }\n"
                      "QDockWidget::title { background:%1; padding:6px; border-top-left-radius:8px; border-top-right-radius:8px; }\n")
                .arg (hex(surface), hex(text2), hex(primary), hex(on_primary));
        return s;
      }

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

      // Apply the Material-Design widget stylesheet, derived from (and so keeping)
      // the active colour theme — i.e. the style's default palette.
      //CONF option: GUIMaterialStyle
      //CONF default: 1 (true)
      //CONF Whether to apply the Material-Design widget stylesheet in the GUI.
      if (MR::File::Config::get_bool ("GUIMaterialStyle", true))
        setStyleSheet (material_stylesheet (palette()));
    }



  }
}
