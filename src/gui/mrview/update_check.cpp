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

#include "gui/mrview/update_check.h"

#ifndef MRTRIX_WASM

#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QUrl>

#include "app.h"
#include "exception.h"
#include "fork_version.h"
#include "gui/gui.h"

namespace MR
{
  namespace GUI
  {
    namespace MRView
    {

      namespace
      {
        const char* const releases_api_url =
            "https://api.github.com/repos/Ulquoirra-rn/mrviewpp/releases/latest";
        const char* const releases_page_url =
            "https://github.com/Ulquoirra-rn/mrviewpp/releases";

        constexpr int request_timeout_ms = 10000;
        constexpr qint64 recheck_interval_s = 24 * 60 * 60;
        constexpr int max_notes_chars = 2000;

        QSettings settings () { return QSettings ("BrainSight", "mrview++"); }

        // "3.0.7-rc1" -> core {3,0,7}, prerelease "rc1"
        void split_version (const std::string& in, vector<int>& core, std::string& prerelease)
        {
          core.clear();
          prerelease.clear();

          std::string s = in;
          if (s.size() && (s[0] == 'v' || s[0] == 'V'))
            s = s.substr (1);

          const size_t dash = s.find_first_of ("-+");
          if (dash != std::string::npos) {
            prerelease = s.substr (dash+1);
            s = s.substr (0, dash);
          }

          size_t pos = 0;
          while (pos <= s.size()) {
            const size_t dot = s.find ('.', pos);
            const std::string field = s.substr (pos, dot == std::string::npos ? std::string::npos : dot-pos);
            int value = 0;
            try { value = field.size() ? std::stoi (field) : 0; }
            catch (...) { value = 0; }
            core.push_back (value);
            if (dot == std::string::npos)
              break;
            pos = dot + 1;
          }
        }
      }



      int UpdateCheck::compare_versions (const std::string& a, const std::string& b)
      {
        vector<int> ca, cb;
        std::string pa, pb;
        split_version (a, ca, pa);
        split_version (b, cb, pb);

        for (size_t i = 0; i != std::max (ca.size(), cb.size()); ++i) {
          const int va = i < ca.size() ? ca[i] : 0;
          const int vb = i < cb.size() ? cb[i] : 0;
          if (va != vb)
            return va < vb ? -1 : 1;
        }

        // Equal cores: a pre-release sorts before the plain release.
        if (pa.empty() && pb.empty()) return 0;
        if (pa.empty()) return 1;
        if (pb.empty()) return -1;
        if (pa == pb) return 0;
        return pa < pb ? -1 : 1;
      }



      bool UpdateCheck::auto_check_enabled ()
      {
        return settings().value ("updates/auto_check", true).toBool();
      }

      void UpdateCheck::set_auto_check_enabled (bool value)
      {
        QSettings s = settings();
        s.setValue ("updates/auto_check", value);
      }



      UpdateCheck::UpdateCheck (QWidget* parent) :
          QObject (parent),
          parent_widget (parent),
          manager (nullptr),
          reply (nullptr),
          timeout (nullptr),
          interactive (false) { }


      UpdateCheck::~UpdateCheck ()
      {
        if (reply) {
          reply->disconnect (this);
          reply->abort();
          reply->deleteLater();
          reply = nullptr;
        }
      }



      void UpdateCheck::check_in_background ()
      {
        if (!auto_check_enabled()) {
          DEBUG ("automatic update check disabled");
          return;
        }
        const qint64 last = settings().value ("updates/last_check_epoch", 0).toLongLong();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (last && now - last < recheck_interval_s) {
          DEBUG ("update check already ran within the last 24 hours");
          return;
        }
        interactive = false;
        start_request();
      }


      void UpdateCheck::check_now ()
      {
        interactive = true;
        start_request();
      }



      void UpdateCheck::start_request ()
      {
        if (reply)   // one request in flight at a time
          return;

        if (!manager) {
          QNetworkProxyFactory::setUseSystemConfiguration (true);
          manager = new QNetworkAccessManager (this);
        }

        QNetworkRequest request { QUrl (releases_api_url) };
        request.setRawHeader ("Accept", "application/vnd.github+json");
        // GitHub rejects requests that do not identify themselves.
        request.setRawHeader ("User-Agent", QByteArray ("mrviewpp/") + MRVIEWPP_VERSION);
        request.setAttribute (QNetworkRequest::RedirectPolicyAttribute,
                              QNetworkRequest::NoLessSafeRedirectPolicy);

        reply = manager->get (request);
        connect (reply, SIGNAL (finished()), this, SLOT (reply_finished()));

        if (!timeout) {
          timeout = new QTimer (this);
          timeout->setSingleShot (true);
          connect (timeout, SIGNAL (timeout()), this, SLOT (request_timed_out()));
        }
        timeout->start (request_timeout_ms);
      }



      void UpdateCheck::request_timed_out ()
      {
        if (reply)
          reply->abort();   // triggers finished() with OperationCanceledError
      }



      void UpdateCheck::fail (const std::string& reason)
      {
        if (interactive) {
          QMessageBox::warning (parent_widget, "Check for updates",
              qstr ("Could not check for updates.\n\n" + reason));
        } else {
          DEBUG ("background update check failed: " + reason);
        }
      }



      void UpdateCheck::reply_finished ()
      {
        if (timeout)
          timeout->stop();

        QNetworkReply* r = reply;
        reply = nullptr;
        if (!r)
          return;
        r->deleteLater();

        // Only a successful check counts as "checked today", so a transient
        // network failure does not suppress tomorrow's attempt... it just means
        // we try again on the next launch.
        if (r->error() != QNetworkReply::NoError) {
          fail (r->errorString().toStdString());
          return;
        }

        QJsonParseError parse_error;
        const QJsonDocument doc = QJsonDocument::fromJson (r->readAll(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
          fail ("unexpected response from the release server");
          return;
        }

        const QJsonObject obj = doc.object();
        if (obj.value ("draft").toBool (false) || obj.value ("prerelease").toBool (false)) {
          DEBUG ("latest release is a draft/pre-release; ignoring");
          if (interactive)
            QMessageBox::information (parent_widget, "Check for updates",
                qstr (std::string ("mrview++ ") + MRVIEWPP_VERSION + " is up to date."));
          return;
        }

        const std::string tag = obj.value ("tag_name").toString().toStdString();
        if (tag.empty()) {
          fail ("the release server did not report a version");
          return;
        }
        std::string url = obj.value ("html_url").toString().toStdString();
        if (url.empty())
          url = releases_page_url;
        std::string notes = obj.value ("body").toString().toStdString();
        if (notes.size() > max_notes_chars)
          notes = notes.substr (0, max_notes_chars) + "\n\n[...]";

        settings().setValue ("updates/last_check_epoch", QDateTime::currentSecsSinceEpoch());

        if (compare_versions (MRVIEWPP_VERSION, tag) >= 0) {
          INFO ("mrview++ " MRVIEWPP_VERSION " is up to date (latest release is " + tag + ")");
          if (interactive)
            QMessageBox::information (parent_widget, "Check for updates",
                qstr (std::string ("mrview++ ") + MRVIEWPP_VERSION + " is up to date."));
          return;
        }

        if (!interactive && settings().value ("updates/skipped_version").toString().toStdString() == tag) {
          DEBUG ("release " + tag + " was skipped by the user");
          return;
        }

        present (tag, url, notes);
      }



      void UpdateCheck::present (const std::string& tag, const std::string& url, const std::string& notes)
      {
        QMessageBox* box = new QMessageBox (parent_widget);
        box->setAttribute (Qt::WA_DeleteOnClose);
        box->setWindowTitle ("Update available");
        box->setIcon (QMessageBox::Information);
        box->setTextFormat (Qt::RichText);
        box->setText (qstr ("<b>mrview++ " + tag + " is available.</b>"));
        box->setInformativeText (qstr (std::string ("You are running ") + MRVIEWPP_VERSION + "."));
        if (notes.size())
          box->setDetailedText (qstr (notes));

        QPushButton* open = box->addButton ("Open release page", QMessageBox::AcceptRole);
        QPushButton* skip = box->addButton ("Skip this version", QMessageBox::DestructiveRole);
        QPushButton* later = box->addButton ("Later", QMessageBox::RejectRole);
        box->setDefaultButton (open);

        const QString tag_q = qstr (tag);
        const QString url_q = qstr (url);
        connect (box, &QMessageBox::finished, box,
            [box, open, skip, later, tag_q, url_q] (int) {
              QAbstractButton* clicked = box->clickedButton();
              if (clicked == open)
                QDesktopServices::openUrl (QUrl (url_q));
              else if (clicked == skip)
                settings().setValue ("updates/skipped_version", tag_q);
              else if (clicked == later)
                { /* nothing: it will be offered again on the next check */ }
            });

        box->setModal (false);
        box->show();
      }


    }
  }
}

#endif
