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

#ifndef __gui_mrview_tool_base_h__
#define __gui_mrview_tool_base_h__

#include <QBoxLayout>
#include <QDockWidget>
#include <QScrollArea>
#include <QEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QSlider>
#include <QStyleOptionSlider>

#include <cmath>

#include "file/config.h"
#include "file/json.h"

#include "gui/mrview/window.h"
#include "gui/mrview/region_source.h"
#include "gui/projection.h"

#define LAYOUT_SPACING 3

#define __STR__(x) #x
#define __STR(x) __STR__(x)

namespace MR
{
  namespace App {
    class OptionList;
    class Options;
  }

  namespace GUI
  {
    namespace MRView
    {
      namespace Tool
      {
        class Base;


        class CameraInteractor
        { NOMEMALIGN
          public:
            CameraInteractor () : _active (false) { }
            bool active () const { return _active; }
            virtual void deactivate ();
            virtual bool slice_move_event (const ModelViewProjection& projection, float inc);
            virtual bool pan_event (const ModelViewProjection& projection);
            virtual bool panthrough_event (const ModelViewProjection& projection);
            virtual bool tilt_event (const ModelViewProjection& projection);
            virtual bool rotate_event (const ModelViewProjection& projection);
          protected:
            bool _active;
            void set_active (bool onoff) { _active = onoff; }
        };



        class Dock : public QDockWidget
        { NOMEMALIGN
          public:
            Dock (const QString& name, bool floating,
                  Qt::DockWidgetArea area = Qt::RightDockWidgetArea) :
              QDockWidget (name, Window::main), tool (nullptr) {
                Window::main->addDockWidget (area, this);
                setFloating (floating);
                // A dock on the top or bottom edge is short and wide, which is the
                // opposite of what a panel of stacked group boxes wants. Tell the
                // tool where it has landed so it can lay itself out the other way.
                connect (this, &QDockWidget::dockLocationChanged,
                         [this] (Qt::DockWidgetArea where) { dock_area_changed (where); });
              }

            //! Pass a change of edge on to the tool, once it exists.
            /*! Defined out of line: Base is declared below this. */
            void dock_area_changed (Qt::DockWidgetArea);
            ~Dock ();

            void closeEvent (QCloseEvent*) override;

            Base* tool;
        };






        class Base : public QFrame { NOMEMALIGN
          public:
            Base (Dock* parent);
            Window& window () const { return *Window::main; }

            std::string current_folder;

            static void add_commandline_options (MR::App::OptionList& options);
            virtual bool process_commandline_option (const MR::App::ParsedOption& opt);

            //! Which edge this tool docks against when it is first opened.
            /*! Hidden, not overridden: create<T>() asks the concrete class, so a
             *  tool declares its own by redeclaring this. The default is the right
             *  edge, where every tool used to go.
             *
             *  Two edges rather than one because some tools are consulted *while*
             *  working in another - the tract list next to the ODF display it
             *  tracks from, the connectome next to the view controls - and tabbing
             *  them into one stack makes that a swap rather than a glance. The cost
             *  is image width, so this is for the tools that are worked in, not the
             *  ones that are visited. */
            static Qt::DockWidgetArea preferred_dock_area () { return Qt::RightDockWidgetArea; }

            //! Re-flow for the edge the tool has been docked against.
            /*! Docked left or right a tool is tall and narrow, and its group boxes
             *  stack downwards. Dragged to the top or bottom it becomes short and
             *  wide, and the same stack needs the whole window's height to show
             *  anything - so the boxes are laid out in a row instead.
             *
             *  The default handles any tool whose top-level layout is a QBoxLayout,
             *  which is all of them: QBoxLayout::setDirection re-flows the children
             *  already in it, so no widget is rebuilt or re-parented. Override to do
             *  something else, or nothing. */
            virtual void dock_area_changed (Qt::DockWidgetArea area) {
              QBoxLayout* box = dynamic_cast<QBoxLayout*> (layout());
              if (!box)
                return;
              const bool horizontal = (area == Qt::TopDockWidgetArea
                                    || area == Qt::BottomDockWidgetArea);
              box->setDirection (horizontal ? QBoxLayout::LeftToRight
                                            : QBoxLayout::TopToBottom);
            }

            //! Regions this tool can offer as tracking ROIs; nullptr if none.
            /*! Only tools that have actually been opened are ever asked. */
            virtual RegionProvider* region_provider () { return nullptr; }

            virtual QSize sizeHint () const override;

            void grab_focus () {
              window().tool_has_focus = this;
              window().set_cursor();
            }
            void release_focus () {
              if (window().tool_has_focus == this) {
                window().tool_has_focus = nullptr;
                window().set_cursor();
              }
            }

            class HBoxLayout : public QHBoxLayout { NOMEMALIGN
              public:
                HBoxLayout () : QHBoxLayout () { init(); }
                HBoxLayout (QWidget* parent) : QHBoxLayout (parent) { init(); }
              protected:
                void init () {
                  setSpacing (LAYOUT_SPACING);
                  setContentsMargins(LAYOUT_SPACING,LAYOUT_SPACING,LAYOUT_SPACING,LAYOUT_SPACING);
                }
            };

            class VBoxLayout : public QVBoxLayout { NOMEMALIGN
              public:
                VBoxLayout () : QVBoxLayout () { init(); }
                VBoxLayout (QWidget* parent) : QVBoxLayout (parent) { init(); }
              protected:
                void init () {
                  setSpacing (LAYOUT_SPACING);
                  setContentsMargins(LAYOUT_SPACING,LAYOUT_SPACING,LAYOUT_SPACING,LAYOUT_SPACING);
                }
            };

            class GridLayout : public QGridLayout { NOMEMALIGN
              public:
                GridLayout () : QGridLayout () { init(); }
                GridLayout (QWidget* parent) : QGridLayout (parent) { init(); }
              protected:
                void init () {
                  setSpacing (LAYOUT_SPACING);
                  setContentsMargins(LAYOUT_SPACING,LAYOUT_SPACING,LAYOUT_SPACING,LAYOUT_SPACING);
                }
            };


            class FormLayout : public QFormLayout { NOMEMALIGN
              public:
                FormLayout () : QFormLayout () { init(); }
                FormLayout (QWidget* parent) : QFormLayout (parent) { init(); }
              protected:
                void init () {
                  setSpacing (LAYOUT_SPACING);
                  setContentsMargins(LAYOUT_SPACING,LAYOUT_SPACING,LAYOUT_SPACING,LAYOUT_SPACING);
                }
            };

            //! A titled section whose contents fold away.
            /*! The tool panels have outgrown a flat stack of group boxes - the Tracts
             *  panel carries the generator, the tract list, per-tract refinement,
             *  editing and four option groups - and a dock inside a scroll area
             *  answers "does it fit" but not "can I see the part I am using". Folding
             *  is per section and remembered for the lifetime of the panel.
             *
             *  Qt has no such widget, and a checkable QGroupBox is the wrong one: its
             *  tick reads as "this section is enabled", not "this section is open". */
            class Section : public QWidget { NOMEMALIGN
              public:
                Section (const QString& title, QWidget* parent, bool open = true) :
                    QWidget (parent)
                {
                  VBoxLayout* outer = new VBoxLayout (this);
                  outer->setContentsMargins (0, 0, 0, 0);
                  header = new QToolButton (this);
                  header->setText (title);
                  header->setCheckable (true);
                  header->setChecked (open);
                  header->setToolButtonStyle (Qt::ToolButtonTextBesideIcon);
                  header->setArrowType (open ? Qt::DownArrow : Qt::RightArrow);
                  header->setAutoRaise (true);
                  // Full width, so the whole header line is the hit target rather
                  // than just the few pixels of the arrow and the text.
                  header->setSizePolicy (QSizePolicy::Expanding, QSizePolicy::Fixed);
                  outer->addWidget (header);
                  body = new QWidget (this);
                  body->setVisible (open);
                  outer->addWidget (body);
                  QObject::connect (header, &QToolButton::toggled, [this] (bool on) {
                    header->setArrowType (on ? Qt::DownArrow : Qt::RightArrow);
                    body->setVisible (on);
                  });
                }

                //! Where a caller puts the section's contents.
                QWidget* contents () { return body; }
                void set_title (const QString& title) { header->setText (title); }
                void set_open (bool on) { header->setChecked (on); }
                bool is_open () const { return header->isChecked(); }

              private:
                QToolButton* header;
                QWidget* body;
            };


            virtual void draw (const Projection& transform, bool is_3D, int axis, int slice);
            virtual void draw_colourbars ();
            virtual size_t visible_number_colourbars () { return 0; }
            virtual int draw_tool_labels (int, int, const Projection&) const { return 0; }
            virtual bool mouse_press_event ();
            virtual bool mouse_move_event ();
            virtual bool mouse_release_event ();
            // Wheel/scroll over the GL area while this tool has focus. Return true
            // to consume the event (otherwise it falls through to slice scrolling).
            virtual bool mouse_wheel_event (int /*delta_x*/, int /*delta_y*/) { return false; }
            virtual void close_event() { }
            virtual void reset_event () { }
            virtual QCursor* get_cursor ();
            void update_cursor() { window().set_cursor(); }

            // Session persistence hooks (used by the Session tool). A tool that
            // returns a non-empty session_key() has its state saved/restored;
            // the json node passed to get_session/set_session is that tool's own.
            virtual std::string session_key () const { return std::string(); }
            virtual void get_session (nlohmann::json&) const { }
            virtual void set_session (const nlohmann::json&) { }

            // Helpers for the main (base) image list; Tool::Base is a friend of
            // Window, so these reach its private image_group / add_images.
            vector<std::string> main_image_filenames () const;
            void load_main_images (const vector<std::string>&);

            void dragEnterEvent (QDragEnterEvent* event) override {
              event->acceptProposedAction();
            }
            void dragMoveEvent (QDragMoveEvent* event) override {
              event->acceptProposedAction();
            }
            void dragLeaveEvent (QDragLeaveEvent* event) override {
              event->accept();
            }
        };







        //! Make a line edit look like the search box it is.
        /*! A bare QLineEdit above a list reads as an empty text field, and grey
           placeholder text inside it is the first thing an eye skips - shown to
           several people, none of them noticed the filters existed. So: a magnifier
           on the left, drawn rather than themed so it follows the text colour on
           both light and dark; a clear button on the right once there is something
           to clear, which also says the field is live; a placeholder that names what
           will be searched; and a minimum height, since a field the height of a
           label is read as a label.
         *
         *  Drawn at 2x and scaled down, so it stays sharp on a retina display. */
        inline void make_search_box (QLineEdit* box, const QString& what)
        {
          if (!box)
            return;
          const int size = 32;      // 16 pt at 2x
          QPixmap glass (size, size);
          glass.fill (Qt::transparent);
          {
            QPainter painter (&glass);
            painter.setRenderHint (QPainter::Antialiasing, true);
            QColor ink = box->palette().color (QPalette::Active, QPalette::WindowText);
            ink.setAlpha (170);
            QPen pen (ink);
            pen.setWidth (3);
            pen.setCapStyle (Qt::RoundCap);
            painter.setPen (pen);
            painter.setBrush (Qt::NoBrush);
            painter.drawEllipse (QRectF (4, 4, 17, 17));
            painter.drawLine (QPointF (21.5, 21.5), QPointF (28, 28));
          }
          glass.setDevicePixelRatio (2.0);
          box->addAction (QIcon (glass), QLineEdit::LeadingPosition);
          box->setClearButtonEnabled (true);
          box->setPlaceholderText (what);
          box->setToolTip (what);
          box->setMinimumHeight (std::max (26, box->sizeHint().height()));
        }



        //! The scroll area a tool is docked inside, sized from the tool.
        /*! QScrollArea's own size hint is a fixed default that says nothing about its
         *  contents, so a dock built around one opens at that default and clips the
         *  panel until it is dragged wider. These pass the tool's hints through, plus
         *  room for the scroll bar, so a dock opens at the width its contents need and
         *  refuses to be squeezed narrower than they can be drawn - it is the height
         *  that scrolls, which is the direction a stack of group boxes grows in. */
        class ToolScrollArea : public QScrollArea { NOMEMALIGN
          public:
            ToolScrollArea (QWidget* parent) : QScrollArea (parent) { }

            QSize sizeHint () const override
            {
              if (!widget())
                return QScrollArea::sizeHint();
              QSize hint = widget()->sizeHint();
              hint.setWidth (hint.width() + gutter());
              return hint;
            }

            QSize minimumSizeHint () const override
            {
              if (!widget())
                return QScrollArea::minimumSizeHint();
              // Width only: the panel must be readable across, but it may be as short
              // as the user likes, because that is what the scroll bar is for.
              QSize hint = QScrollArea::minimumSizeHint();
              hint.setWidth (widget()->minimumSizeHint().width() + gutter());
              return hint;
            }

          private:
            int gutter () const {
              return verticalScrollBar()->sizeHint().width() + 2*frameWidth() + 2;
            }
        };



        //! Marks a slider's detents, drawn over it as a transparent child.
        /*! Qt's own tick marks (QSlider::setTickPosition) are not an option: on macOS
         *  they switch the slider to a different native appearance - a pointed handle
         *  and a groove that no longer fills behind it - which is what made the
         *  sliders look broken after round 14. Restyling has the same hazard, since
         *  the native style is entitled to draw a slider however it likes.
         *
         *  So the slider is left exactly as Qt draws it and the marks go on top, in a
         *  child widget that is transparent to the mouse and knows nothing about the
         *  slider beyond where its groove and handle are. Marks under the handle are
         *  skipped, and they thin out rather than crowd when the slider is short. */
        class SliderDetents : public QWidget { NOMEMALIGN
          public:
            SliderDetents (QSlider* slider, int steps) :
                QWidget (slider), slider (slider), steps (steps)
            {
              setAttribute (Qt::WA_TransparentForMouseEvents);
              setAttribute (Qt::WA_NoSystemBackground);
              setGeometry (slider->rect());
              slider->installEventFilter (this);
              raise();
            }

          protected:
            bool eventFilter (QObject* watched, QEvent* event) override
            {
              if (watched == slider && (event->type() == QEvent::Resize
                                     || event->type() == QEvent::Show)) {
                setGeometry (slider->rect());
                raise();
              }
              return QWidget::eventFilter (watched, event);
            }

            void paintEvent (QPaintEvent*) override
            {
              if (steps < 2 || slider->maximum() <= slider->minimum())
                return;
              QStyleOptionSlider option;
              option.initFrom (slider);
              option.orientation = slider->orientation();
              option.minimum = slider->minimum();
              option.maximum = slider->maximum();
              option.sliderPosition = slider->sliderPosition();
              option.sliderValue = slider->value();
              option.upsideDown = slider->invertedAppearance();
              option.subControls = QStyle::SC_All;

              QStyle* style = slider->style();
              const QRect groove = style->subControlRect (QStyle::CC_Slider, &option,
                                                          QStyle::SC_SliderGroove, slider);
              const QRect handle = style->subControlRect (QStyle::CC_Slider, &option,
                                                          QStyle::SC_SliderHandle, slider);
              const bool horizontal = slider->orientation() == Qt::Horizontal;
              const int span = horizontal ? groove.width() - handle.width()
                                          : groove.height() - handle.height();
              if (span <= 0)
                return;

              // Thin out rather than crowd: below about nine pixels apart a row of
              // marks reads as a smear. The slider still stops on all of them.
              const int stride = std::max (1, int (std::ceil (9.0 * steps / double (span))));

              QPainter painter (this);
              painter.setRenderHint (QPainter::Antialiasing, false);
              QColor ink = palette().color (QPalette::WindowText);
              // Faint: a scale to glance at, not a fence across the groove.
              ink.setAlpha (slider->isEnabled() ? 55 : 30);
              painter.setPen (ink);
              for (int i = 0; i <= steps; i += stride) {
                const int value = slider->minimum()
                    + int (std::lround (double (slider->maximum() - slider->minimum()) * i / steps));
                int at = QStyle::sliderPositionFromValue (slider->minimum(), slider->maximum(),
                                                          value, span, option.upsideDown);
                if (horizontal) {
                  at += groove.left() + handle.width()/2;
                  if (at >= handle.left()-1 && at <= handle.right()+1)
                    continue;                       // the handle is standing on it
                  // The lower half of the groove and a little below it: enough to
                  // read as a scale without hatching across the filled part.
                  painter.drawLine (at, groove.center().y() + 1, at, groove.bottom() + 2);
                } else {
                  at += groove.top() + handle.height()/2;
                  if (at >= handle.top()-1 && at <= handle.bottom()+1)
                    continue;
                  painter.drawLine (groove.center().x() + 1, at, groove.right() + 2, at);
                }
              }
            }

          private:
            QSlider* const slider;
            const int steps;
        };



        //! Give a slider detents, so a drag stops on values rather than between them.
        /*! Only for the sliders where a drag costs something: re-deriving a tract,
         *  rebuilding a tractogram's vertex buffers, re-running a volume ray-cast.
         *  A continuous groove asks for that work once per pixel of travel, at values
         *  a pixel apart that nobody chose and nobody can return to; \a steps detents
         *  give the same reach with values that can be hit deliberately, stepped
         *  through with the arrow keys, and seen on the groove.
         *
         *  Sliders whose handler only changes a uniform and redraws - opacity,
         *  thresholds, dimming, colour fades - are deliberately left smooth. They
         *  keep up with the drag, so detents would only take away the fine control
         *  they can afford to offer.
         *
         *  The snap is applied to the slider's *position* while it is being dragged,
         *  before the position becomes the value, so the handler downstream is called
         *  once per detent with the snapped number - not once per pixel and then again
         *  when it is corrected. */
        inline void set_slider_steps (QSlider* slider, int steps = 40)
        {
          if (!slider || steps < 1)
            return;
          const int span = slider->maximum() - slider->minimum();
          if (span <= 0)
            return;
          const int step = std::max (1, int (std::lround (double (span) / double (steps))));
          slider->setSingleStep (step);
          slider->setPageStep (step);
          slider->setTickPosition (QSlider::NoTicks);   // see SliderDetents
          new SliderDetents (slider, std::max (1, span / step));
          QObject::connect (slider, &QSlider::sliderMoved, slider, [slider, step] (int v) {
            const int base = slider->minimum();
            const int snapped = base + int (std::lround (double (v - base) / double (step))) * step;
            if (snapped != v)
              slider->setSliderPosition (snapped);   // bounded by the slider itself
          });
          // The marks sit next to the handle, so they have to be repainted with it.
          QObject::connect (slider, &QSlider::valueChanged, slider, [slider] { slider->update(); });
        }



        //! \cond skip

        inline Dock::~Dock () { delete tool; }

        inline void Dock::dock_area_changed (Qt::DockWidgetArea where) {
          // Docks are constructed before their tool, and Qt can emit this while the
          // dock is being placed - so there is a window in which there is nothing to
          // tell.
          if (tool)
            tool->dock_area_changed (where);
        }



        class __Action__ : public QAction
        { NOMEMALIGN
          public:
            __Action__ (QActionGroup* parent,
                        const char* const name,
                        const char* const description,
                        int index) :
              QAction (name, parent),
              dock (nullptr) {
              setCheckable (true);
              setShortcut (tr (std::string ("Ctrl+F" + str (index)).c_str()));
              setStatusTip (tr (description));
            }

            virtual ~__Action__ () { delete dock; }

            virtual Dock* create (bool floating) = 0;
            Dock* dock;
        };
        //! \endcond


        template <class T>
          Dock* create (const QString& text, bool floating)
          {
            Dock* dock = new Dock (text, floating, T::preferred_dock_area());
            dock->tool = new T (dock);
            // Tools report sizeHint() == minimumSizeHint(), so docking one straight
            // into the dock widget makes the whole main window grow to whatever the
            // panel demands - off-screen, for the taller panels. Putting it in a
            // scroll area lets the dock be smaller than its contents and scroll
            // instead. ~Dock deletes the tool before the scroll area, and QObject
            // unparents it on the way out, so there is no double delete.
            ToolScrollArea* scroll = new ToolScrollArea (dock);
            scroll->setFrameShape (QFrame::NoFrame);
            scroll->setWidgetResizable (true);
            scroll->setWidget (dock->tool);
            dock->setWidget (scroll);
            dock->show();
            // Open at the width the panel actually needs. Without this the dock takes
            // the scroll area's own default hint, which knows nothing about what is
            // inside it, and every panel opens too narrow to read - the first thing
            // to do with a newly opened tool should not be resizing it.
            Window::main->resizeDocks ({ dock }, { scroll->sizeHint().width() }, Qt::Horizontal);
            return dock;
          }


        template <class T>
          class Action : public __Action__
        { NOMEMALIGN
          public:
            Action (QActionGroup* parent,
                const char* const name,
                const char* const description,
                int index) :
              __Action__ (parent, name, description, index) { }

            virtual Dock* create (bool floating) {
              dock = Tool::create<T> (this->text(), floating);
              return dock;
            }
        };



        //! Find another tool by type, e.g. get_tool<Tool::ODF>().
        /*! Tools are registered as Action<T>, so the type is recoverable from the
         *  action alone - which means we can identify the right tool even before
         *  its dock exists, and open it on demand. Returns nullptr if the tool is
         *  not registered, or if it is closed and \a create_if_needed is false.
         *
         *  This is the typed alternative to matching on the menu name, which is
         *  what Session::ensure_tool_open has to do (its keys come from the
         *  session file). */
        template <class T>
          T* get_tool (bool create_if_needed = true)
          {
            if (!Window::main)
              return nullptr;
            QList<QAction*> actions = Window::main->tools()->actions();
            for (int i = 0; i != actions.size(); ++i) {
              Action<T>* action = dynamic_cast<Action<T>*> (actions[i]);
              if (!action)
                continue;
              if (!action->dock) {
                if (!create_if_needed)
                  return nullptr;
                actions[i]->trigger();   // opens the dock synchronously
              }
              return action->dock ? dynamic_cast<T*> (action->dock->tool) : nullptr;
            }
            return nullptr;
          }




      }
    }
  }
}

#endif


