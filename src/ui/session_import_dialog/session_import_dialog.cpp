// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/session_import_dialog/session_import_dialog.h"

#include "backend/slack/session_import/local_importer.h"
#include "backend/slack/session_import/token_deriver.h"
#include "backend/slack/slack_auth.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString friendlyImportError(const QString &reason) {
    if (reason == QLatin1String("not_installed"))
        return QObject::tr("The Slack desktop app wasn't found on this computer.");
    if (reason == QLatin1String("locked"))
        return QObject::tr("Couldn't read Slack's data — try quitting the Slack app first.");
    if (reason == QLatin1String("decrypt_failed") || reason == QLatin1String("no_cookie"))
        return QObject::tr("Couldn't read Slack's saved session automatically.");
    if (reason == QLatin1String("unsupported_platform"))
        return QObject::tr("Automatic import isn't available in this build.");
    return QObject::tr("Automatic import didn't work.");
}

// Normalize a pasted cookie into a bare value (drops a leading "d=" and whitespace).
QString normalizeCookie(QString v) {
    v = v.trimmed();
    if (v.startsWith(QLatin1String("d=")))
        v = v.mid(2).trimmed();
    return v;
}

// Turn "myteam", "myteam.slack.com", or a full URL into "https://<host>".
QString normalizeWorkspaceUrl(QString v) {
    v = v.trimmed();
    if (v.isEmpty())
        return {};
    v.remove(QStringLiteral("https://"));
    v.remove(QStringLiteral("http://"));
    v = v.section(QLatin1Char('/'), 0, 0); // host only
    if (!v.contains(QLatin1Char('.')))
        v += QStringLiteral(".slack.com");
    return QStringLiteral("https://") + v;
}

} // namespace

SessionImportDialog::SessionImportDialog(QWidget *parent)
    : AppDialog(QObject::tr("Add Slack workspace with a session token"), parent) {
    const auto &sp  = Th::c().spacing;
    auto       *lay = contentLayout();
    lay->setSpacing(sp.lg);

    auto *intro = new QLabel(
        QObject::tr(
            "Sign in with your existing Slack session instead of app keys — it uses "
            "your account's own rate limits, so it avoids the shared-key timeouts. "
            "New messages arrive by polling (there's no live push this way)."
        ),
        this
    );
    intro->setWordWrap(true);
    lay->addWidget(intro);

    // ── Primary: one-click import from the local Slack desktop app ──────────
    _importBtn = new StyledButton(
        QObject::tr("Import from local Slack"), StyledButton::Variant::Primary, this
    );
    _importBtn->setVisible(slack::session::localImportSupported());
    lay->addWidget(_importBtn);
    connect(_importBtn, &StyledButton::clicked, this, &SessionImportDialog::tryLocalImport);

    _manualToggle = new StyledButton(
        QObject::tr("Paste a session cookie instead"), StyledButton::Variant::Link, this
    );
    lay->addWidget(_manualToggle);
    connect(_manualToggle, &StyledButton::clicked, this, [this] { revealManual(); });

    // ── Guided manual entry: paste the `d` cookie (the one value that can't be
    // scripted — it's HttpOnly) plus the workspace address; the app derives the
    // xoxc- token from them. Fields are added directly to contentLayout so their
    // wrapped-text heights count toward the card size. Hidden until revealed. ──
    _steps = new QLabel(
        QObject::tr(
            "1. Sign in to the workspace in your browser.\n"
            "2. Open developer tools (F12) → Application → Cookies → https://app.slack.com, "
            "and copy the value of the cookie named “d” (it starts with xoxd-) into Cookie. "
            "Browsers hide this cookie from scripts, so it has to be copied by hand.\n"
            "3. Enter your workspace address."
        ),
        this
    );
    _steps->setWordWrap(true);
    _steps->setVisible(false);
    lay->addWidget(_steps);

    _cookieEdit = new StyledLineEdit(this);
    _cookieEdit->setSize(StyledLineEdit::Size::Small);
    _cookieEdit->setPrefix(QObject::tr("Cookie"));
    _cookieEdit->setPlaceholderText(QStringLiteral("xoxd-…"));
    _cookieEdit->enablePasswordReveal();
    _cookieEdit->setVisible(false);
    lay->addWidget(_cookieEdit);

    _wsEdit = new StyledLineEdit(this);
    _wsEdit->setSize(StyledLineEdit::Size::Small);
    _wsEdit->setPrefix(QObject::tr("Workspace"));
    _wsEdit->setPlaceholderText(QStringLiteral("myteam.slack.com"));
    _wsEdit->setVisible(false);
    lay->addWidget(_wsEdit);
    connect(_wsEdit, &StyledLineEdit::returnPressed, this, &SessionImportDialog::submitManual);

    _status = new QLabel(this);
    _status->setWordWrap(true);
    _status->setVisible(false);
    lay->addWidget(_status);

    // Secondary escape: prefer session (this dialog is the default), but let the
    // user fall back to app-keys/OAuth sign-in. The host wires this up.
    auto *appKeysLink = new StyledButton(
        QObject::tr("Use app keys (OAuth) instead"), StyledButton::Variant::Link, this
    );
    lay->addWidget(appKeysLink);
    connect(appKeysLink, &StyledButton::clicked, this, [this] {
        emit useAppKeysRequested();
        reject();
    });

    // Footer: [Add workspace] is the manual submit; [Cancel] rejects.
    _manualSubmit =
        new StyledButton(QObject::tr("Add workspace"), StyledButton::Variant::Primary, this);
    _manualSubmit->setVisible(false);
    auto *cancel = new StyledButton(QObject::tr("Cancel"), StyledButton::Variant::Secondary, this);
    addButtonRow(_manualSubmit, cancel);
    connect(_manualSubmit, &StyledButton::clicked, this, &SessionImportDialog::submitManual);

    // No local import available → go straight to the guided manual flow.
    if (!slack::session::localImportSupported())
        revealManual();

    updateCard();
}

int SessionImportDialog::cardWidth(int availOverlayWidth) const {
    // Deliberately roomy: this dialog carries step-by-step instructions plus
    // inputs. Grow toward 760 but never exceed the available overlay.
    return std::min(760, std::max(600, availOverlayWidth));
}

void SessionImportDialog::tryLocalImport() {
    setBusy(true);
    setStatus(QObject::tr("Importing from local Slack…"), false);
    const slack::session::LocalImport imp = slack::session::importLocalSlackSession();
    if (!imp.ok()) {
        setBusy(false);
        revealManual(friendlyImportError(imp.error));
        return;
    }
    // Cookie recovered. If we also discovered workspace hosts, derive tokens for
    // them; otherwise fall to the guided flow with the cookie prefilled.
    if (imp.teams.isEmpty()) {
        _cookieEdit->setText(imp.cookie);
        revealManual(
            QObject::tr("Found your Slack session — enter your workspace address."), false
        );
        return;
    }
    deriveAndFinish(imp.cookie, imp.teams);
}

void SessionImportDialog::submitManual() {
    const QString cookie = normalizeCookie(_cookieEdit->text());
    const QString url    = normalizeWorkspaceUrl(_wsEdit->text());
    if (cookie.isEmpty()) {
        setStatus(QObject::tr("Paste the “d” cookie value."), true);
        return;
    }
    if (url.isEmpty()) {
        setStatus(QObject::tr("Enter your workspace address (e.g. myteam.slack.com)."), true);
        return;
    }
    slack::session::TeamSession cand;
    cand.workspaceUrl = url;
    deriveAndFinish(cookie, {cand});
}

void SessionImportDialog::deriveAndFinish(
    const QString &cookie, const QList<slack::session::TeamSession> &candidates
) {
    setBusy(true);
    setStatus(QObject::tr("Verifying your session…"), false);

    // Fresh deriver per attempt so a retry never sees a previous connection.
    if (_deriver)
        _deriver->deleteLater();
    _deriver = new slack::session::TokenDeriver(this);
    connect(
        _deriver,
        &slack::session::TokenDeriver::finished,
        this,
        [this](const QList<slack::Credentials> &valid, const QString &error) {
            setBusy(false);
            if (valid.isEmpty()) {
                const QString why = error == QLatin1String("invalid_auth")
                                        ? QObject::tr(
                                              "That session was rejected — the cookie may "
                                              "have expired. Sign in to Slack again and "
                                              "copy a fresh cookie."
                                          )
                                        : QObject::tr(
                                              "Couldn't verify that session. Check the "
                                              "cookie and workspace address and try again."
                                          );
                revealManual(why);
                return;
            }
            QList<TokenStore::WorkspaceRecord> records;
            records.reserve(valid.size());
            for (const auto &c : valid)
                records.append(slack::toRecord(c));
            emit imported(records);
            accept();
        }
    );
    _deriver->run(cookie, candidates);
}

void SessionImportDialog::revealManual(const QString &notice, bool error) {
    setBusy(false);
    _steps->setVisible(true);
    _cookieEdit->setVisible(true);
    _wsEdit->setVisible(true);
    _manualSubmit->setVisible(true);
    _manualToggle->setVisible(false); // now redundant
    if (!notice.isEmpty())
        setStatus(notice, error);
    _cookieEdit->lineEdit()->setFocus();
    updateCard();
}

void SessionImportDialog::setBusy(bool busy) {
    _importBtn->setEnabled(!busy);
    _manualSubmit->setEnabled(!busy);
    _cookieEdit->setEnabled(!busy);
    _wsEdit->setEnabled(!busy);
}

void SessionImportDialog::setStatus(const QString &msg, bool error) {
    if (msg.isEmpty()) {
        _status->setVisible(false);
        updateCard();
        return;
    }
    _status->setText(msg);
    _status->setStyleSheet(QStringLiteral("color:%1;")
                               .arg(Th::qss(error ? Th::c().danger.text : Th::c().text.secondary)));
    _status->setVisible(true);
    updateCard();
}
