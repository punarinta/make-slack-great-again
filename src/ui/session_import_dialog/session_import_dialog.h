// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// "Add Slack workspace with a session token" dialog. Offers, in order of how little
// the user has to do: browser sign-in (drives a throwaway-profile browser and reads
// the session out of it), one-click import from the locally-installed Slack desktop
// app, and — always available — a guided manual paste of the `d` cookie. All three
// paths derive the xoxc- token and validate it, then emit the ready-to-store
// workspace records. See docs/BROWSER_LOGIN_PLAN.md.
#pragma once

#include "auth/token_store.h"
#include "backend/slack/session_import/session_types.h"
#include "ui/app_dialog/app_dialog.h"

#include <QList>

class QLabel;
class QWidget;
class StyledButton;
class StyledLineEdit;

namespace slack::session {
class BrowserLogin;
class TokenDeriver;
} // namespace slack::session

class SessionImportDialog : public AppDialog {
    Q_OBJECT
public:
    explicit SessionImportDialog(QWidget *parent);

signals:
    // Emitted once with the validated workspace(s) the user chose to add. The
    // dialog accept()s right after; the host saves + activates them.
    void imported(const QList<TokenStore::WorkspaceRecord> &records);
    // Emitted when the user picks the secondary "use app keys (OAuth) instead"
    // escape. The dialog closes; the host starts the OAuth flow / switches mode.
    void useAppKeysRequested();

protected:
    // Wider/taller than the default AppDialog card — this dialog carries
    // step-by-step instructions plus two inputs and needs the breathing room.
    int cardWidth(int availOverlayWidth) const override;
    int minCardHeight() const override { return 440; }

private:
    void startBrowserLogin();
    void tryLocalImport();
    void submitManual();
    void revealManual(const QString &notice = {}, bool error = true);
    void setBusy(bool busy);
    void setStatus(const QString &msg, bool error);
    void
    deriveAndFinish(const QString &cookie, const QList<slack::session::TeamSession> &candidates);

    StyledButton   *_browserBtn   = nullptr;
    StyledButton   *_importBtn    = nullptr;
    QLabel         *_status       = nullptr;
    StyledButton   *_manualToggle = nullptr;
    QLabel         *_steps        = nullptr;
    StyledLineEdit *_cookieEdit   = nullptr;
    StyledLineEdit *_wsEdit       = nullptr;
    StyledButton   *_manualSubmit = nullptr;

    slack::session::BrowserLogin *_browser = nullptr;
    slack::session::TokenDeriver *_deriver = nullptr;
};
