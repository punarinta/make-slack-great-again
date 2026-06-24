// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/imap_add_account/imap_add_account_dialog.h"

#include "backend/imap/imap_client.h"
#include "backend/imap/imap_server_detect.h"
#include "llm/oauth_loopback.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"

#include <QDateTime>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QVBoxLayout>

ImapAddAccountDialog::ImapAddAccountDialog(QWidget *parent)
    : AppDialog(QObject::tr("Add email account"), parent) {
    const auto &sp  = Th::c().spacing;
    auto       *lay = contentLayout();

    _email = new StyledLineEdit(this);
    _email->setPlaceholderText(QStringLiteral("you@example.com"));
    lay->addWidget(_email);

    // Server row: [ IMAP server field | Detect ] — shown only for unknown domains.
    _serverRow      = new QWidget(this);
    auto *serverLay = new QHBoxLayout(_serverRow);
    serverLay->setContentsMargins(0, 0, 0, 0);
    serverLay->setSpacing(Th::c().spacing.sm);
    _imapHost = new StyledLineEdit(_serverRow);
    _imapHost->setPrefix(QObject::tr("IMAP server"));
    _detectBtn =
        new StyledButton(QObject::tr("Detect"), StyledButton::Variant::Secondary, _serverRow);
    serverLay->addWidget(_imapHost, 1);
    serverLay->addWidget(_detectBtn);
    _serverRow->setVisible(false);
    lay->addWidget(_serverRow);
    connect(_detectBtn, &StyledButton::clicked, this, &ImapAddAccountDialog::onDetect);

    // Passwordless sign-in for OAuth providers (shown only when a client id is
    // compiled in — otherwise the password field below is the fallback).
    _oauthBtn =
        new StyledButton(QObject::tr("Sign in with Google"), StyledButton::Variant::Primary, this);
    _oauthBtn->setVisible(false);
    lay->addWidget(_oauthBtn);
    connect(_oauthBtn, &StyledButton::clicked, this, &ImapAddAccountDialog::startOAuth);

    _password = new StyledLineEdit(this);
    _password->setPlaceholderText(QObject::tr("Password or app password"));
    _password->enablePasswordReveal();
    _password->setVisible(false);
    lay->addWidget(_password);

    _help = new QLabel(this);
    _help->setTextFormat(Qt::RichText);
    _help->setOpenExternalLinks(true);
    _help->setWordWrap(true);
    _help->setVisible(false);
    lay->addWidget(_help);

    _status = new QLabel(this);
    _status->setWordWrap(true);
    _status->setVisible(false);
    lay->addWidget(_status);

    _primary     = new StyledButton(QObject::tr("Continue"), StyledButton::Variant::Primary, this);
    auto *cancel = new StyledButton(QObject::tr("Cancel"), StyledButton::Variant::Secondary, this);
    addButtonRow(_primary, cancel);
    connect(_primary, &StyledButton::clicked, this, &ImapAddAccountDialog::onPrimary);
    connect(_email, &StyledLineEdit::returnPressed, this, &ImapAddAccountDialog::onPrimary);
    connect(_password, &StyledLineEdit::returnPressed, this, &ImapAddAccountDialog::onPrimary);

    Q_UNUSED(sp);
    updateCard();
}

ImapAddAccountDialog::~ImapAddAccountDialog() = default;

void ImapAddAccountDialog::onPrimary() {
    if (_step == 1)
        revealDetails();
    else
        validateAndAccept();
}

void ImapAddAccountDialog::revealDetails() {
    const QString email = _email->text().trimmed();
    if (!email.contains(QLatin1Char('@')) || email.endsWith(QLatin1Char('@'))) {
        setStatus(QObject::tr("Enter a valid email address."), true);
        return;
    }
    _step     = 2;
    _provider = imap::detectProvider(email);
    if (_provider.known) {
        configureProvider(); // known domain (e.g. gmail.com) — instant, no DNS
        return;
    }
    // Unknown domain: resolve via SRV / MX-provider / probe (e.g. a custom domain
    // hosted on Google → Gmail). Hide the fields until it resolves.
    _serverRow->setVisible(false);
    _password->setVisible(false);
    _help->setVisible(false);
    _primary->setText(QObject::tr("Add"));
    setDetecting(true);
    startResolve(email);
    updateCard();
}

void ImapAddAccountDialog::startResolve(const QString &email) {
    if (!_detector) {
        _detector = new imap::ServerDetector(this);
        connect(_detector, &imap::ServerDetector::resolved, this, [this](imap::ProviderInfo info) {
            setDetecting(false);
            _provider = info;
            configureProvider();
        });
    }
    _detector->resolve(email);
}

void ImapAddAccountDialog::configureProvider() {
    _primary->setText(QObject::tr("Add"));

    // Passwordless path: when the provider uses OAuth AND a client id is compiled
    // in, offer "Sign in with Google" and hide the password row + Add button. The
    // OAuth button drives accept directly. Otherwise fall back to password login.
    if (oauthAvailable()) {
        _oauthBtn->setText(QObject::tr("Sign in with %1").arg(_provider.name));
        _oauthBtn->setVisible(true);
        _password->setVisible(false);
        _serverRow->setVisible(false);
        _help->setVisible(false);
        _primary->setVisible(false);
        setStatus(QString(), false);
        updateCard();
        return;
    }
    _oauthBtn->setVisible(false);
    _primary->setVisible(true);
    _password->setVisible(true);

    // OAuth providers without a configured client id → app-password fallback note.
    QString helpHtml;
    if (_provider.auth == imap::AuthMethod::OAuthGoogle ||
        _provider.auth == imap::AuthMethod::OAuthMicrosoft)
        helpHtml = QObject::tr("%1 needs an app password to sign in").arg(_provider.name);
    else if (!_provider.appPasswordHelpUrl.isEmpty())
        helpHtml = QObject::tr("%1 needs an app password").arg(_provider.name);
    if (!helpHtml.isEmpty() && !_provider.appPasswordHelpUrl.isEmpty()) {
        _help->setText(
            QStringLiteral("%1: <a href=\"%2\">%3</a>")
                .arg(helpHtml, _provider.appPasswordHelpUrl, _provider.appPasswordHelpUrl)
        );
        _help->setVisible(true);
    } else {
        _help->setVisible(false);
    }

    if (_provider.known) {
        // Host known (table or detected) → no manual entry; confirm it inline.
        _serverRow->setVisible(false);
        setStatus(
            helpHtml.isEmpty() ? QObject::tr("Server: %1").arg(_provider.imapHost) : QString(),
            false
        );
    } else {
        // Couldn't detect → manual entry (empty, no guessed prefill).
        _imapHost->clear();
        _serverRow->setVisible(true);
        setStatus(QObject::tr("Couldn't detect the server — enter it manually."), true);
    }
    _password->lineEdit()->setFocus();
    updateCard();
}

void ImapAddAccountDialog::onDetect() {
    const QString email = _email->text().trimmed();
    if (!email.contains(QLatin1Char('@')))
        return;
    setDetecting(true);
    startResolve(email); // onResolved reconfigures (fills host / shows provider UI)
}

void ImapAddAccountDialog::setDetecting(bool on) {
    _primary->setEnabled(!on);
    if (_detectBtn)
        _detectBtn->setEnabled(!on);
    _imapHost->setEnabled(!on);
    if (on)
        setStatus(QObject::tr("Detecting…"), false);
}

void ImapAddAccountDialog::validateAndAccept() {
    const QString email = _email->text().trimmed();
    const QString pass  = _password->text();
    const QString host  = _provider.known ? _provider.imapHost : _imapHost->text().trimmed();
    if (!_provider.known && host.isEmpty()) {
        setStatus(QObject::tr("Enter the IMAP server or click Detect."), true);
        return;
    }
    if (pass.isEmpty()) {
        setStatus(QObject::tr("Enter your password."), true);
        return;
    }
    _creds.user     = email;
    _creds.password = pass;
    _creds.host     = host;
    _creds.port     = _provider.imapPort;
    _creds.smtpHost = _provider.smtpHost;
    _creds.smtpPort = _provider.smtpPort;

    setStatus(QObject::tr("Checking…"), false);
    _primary->setEnabled(false);

    _probe = new imap::ImapClient(this);
    connect(_probe, &imap::ImapClient::loggedIn, this, [this] {
        accept(); // success → accepted() fires; prompt() reads credentials()
    });
    connect(_probe, &imap::ImapClient::error, this, [this](const QString &err) {
        setStatus(QObject::tr("Sign-in failed: %1").arg(err), true);
        _primary->setEnabled(true);
        if (_probe) {
            _probe->deleteLater();
            _probe = nullptr;
        }
    });
    _probe->connectToServer(_creds.host, _creds.port);
    _probe->login(_creds.user, _creds.password);
}

bool ImapAddAccountDialog::oauthAvailable() const {
    const QString email = _email->text().trimmed();
    return imap::oauthConfigFor(_provider.auth, email).has_value();
}

void ImapAddAccountDialog::startOAuth() {
    const QString email = _email->text().trimmed();
    const auto    cfg   = imap::oauthConfigFor(_provider.auth, email);
    if (!cfg) { // shouldn't happen (button hidden otherwise) — fall back to password
        configureProvider();
        return;
    }
    _oauthBtn->setEnabled(false);
    setStatus(QObject::tr("Opening your browser to sign in…"), false);

    _oauthFlow = new OAuthLoopbackFlow(*cfg, this);
    connect(_oauthFlow, &OAuthLoopbackFlow::done, this, [this, email](QJsonObject tok) {
        const QString access  = tok.value(QStringLiteral("access_token")).toString();
        const QString refresh = tok.value(QStringLiteral("refresh_token")).toString();
        const qint64  ttl     = qint64(tok.value(QStringLiteral("expires_in")).toDouble(3600));
        if (access.isEmpty()) {
            setStatus(QObject::tr("Sign-in failed: no access token returned."), true);
            _oauthBtn->setEnabled(true);
            return;
        }
        _creds.user         = email;
        _creds.host         = _provider.imapHost;
        _creds.port         = _provider.imapPort;
        _creds.smtpHost     = _provider.smtpHost;
        _creds.smtpPort     = _provider.smtpPort;
        _creds.authMethod   = _provider.auth;
        _creds.accessToken  = access;
        _creds.refreshToken = refresh;
        _creds.expiresAt    = QDateTime::currentSecsSinceEpoch() + ttl;

        // Validate the token against IMAP before accepting.
        setStatus(QObject::tr("Checking…"), false);
        _probe = new imap::ImapClient(this);
        connect(_probe, &imap::ImapClient::loggedIn, this, [this] { accept(); });
        connect(_probe, &imap::ImapClient::error, this, [this](const QString &err) {
            setStatus(QObject::tr("Sign-in failed: %1").arg(err), true);
            _oauthBtn->setEnabled(true);
            if (_probe) {
                _probe->deleteLater();
                _probe = nullptr;
            }
        });
        _probe->connectToServer(_creds.host, _creds.port);
        _probe->loginXOAuth2(_creds.user, _creds.accessToken);
    });
    connect(_oauthFlow, &OAuthLoopbackFlow::failed, this, [this](const QString &e) {
        setStatus(QObject::tr("Sign-in failed: %1").arg(e), true);
        _oauthBtn->setEnabled(true);
    });
    _oauthFlow->start();
}

void ImapAddAccountDialog::setStatus(const QString &msg, bool error) {
    if (msg.isEmpty()) {
        _status->setVisible(false);
        return;
    }
    _status->setText(msg);
    _status->setStyleSheet(QStringLiteral("color:%1;")
                               .arg(Th::qss(error ? Th::c().danger.text : Th::c().text.secondary)));
    _status->setVisible(true);
    updateCard();
}

void ImapAddAccountDialog::prompt(
    QObject *parent, std::function<void(std::optional<imap::Credentials>)> done
) {
    auto *w   = qobject_cast<QWidget *>(parent);
    auto *dlg = new ImapAddAccountDialog(w);
    QObject::connect(dlg, &AppDialog::accepted, dlg, [dlg, done] { done(dlg->credentials()); });
    QObject::connect(dlg, &AppDialog::rejected, dlg, [done] { done(std::nullopt); });
    QObject::connect(dlg, &AppDialog::finished, dlg, [dlg](int) { dlg->deleteLater(); });
    dlg->open();
}
