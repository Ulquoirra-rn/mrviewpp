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

#ifndef __gui_mrview_update_check_h__
#define __gui_mrview_update_check_h__

#ifndef MRTRIX_WASM

#include <QObject>
#include <QString>

#include "types.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QWidget;

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {

      // Checks github.com/Ulquoirra-rn/mrviewpp for a newer release and tells
      // the user about it. Notification only: nothing is ever downloaded or
      // installed, the user is just pointed at the release page.
      //
      // The version being compared is MRVIEWPP_VERSION from core/fork_version.h,
      // which CI guarantees matches the release tag.
      class UpdateCheck : public QObject
      { NOMEMALIGN
        Q_OBJECT

        public:
          explicit UpdateCheck (QWidget* parent);
          ~UpdateCheck ();

          // Silent unless a newer, non-skipped release exists. Does nothing if
          // the user disabled automatic checks or one already ran today.
          void check_in_background ();

          // Always reports an outcome, including "up to date" and errors.
          void check_now ();

          bool busy () const { return reply != nullptr; }

          static bool auto_check_enabled ();
          static void set_auto_check_enabled (bool);

          //! Compare two version strings; returns -1, 0 or +1 for a<b, a==b, a>b.
          /*! A leading "v" is ignored, the dot-separated components are compared
           *  numerically (so 3.0.10 > 3.0.9, which a string compare gets wrong),
           *  missing trailing components count as zero, and a pre-release suffix
           *  sorts before the corresponding release (3.0.7-rc1 < 3.0.7). */
          static int compare_versions (const std::string& a, const std::string& b);

        private slots:
          void reply_finished ();
          void request_timed_out ();

        private:
          QWidget* parent_widget;
          QNetworkAccessManager* manager;
          QNetworkReply* reply;
          QTimer* timeout;
          bool interactive;

          void start_request ();
          void fail (const std::string& reason);
          void present (const std::string& tag, const std::string& url, const std::string& notes);
      };

    }
  }
}

#endif

#endif
