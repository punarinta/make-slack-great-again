// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// "Email + auto-detect" add-account dialog (imap-backend-plan §5). Step 1: enter
// email. Step 2: provider auto-detected → password field (+ editable IMAP host
// for unknown domains) with a prefilled summary and app-password help link; on
// "Add" it validates by attempting an IMAP login, showing inline errors, and
// only accepts on success. Lives above the seam (QtWidgets); injected into
// imap::AuthStrategy via the prompt hook so the auth factory stays Widgets-free.
#pragma once

#include "backend/imap/imap_auth.h"
#include "backend/imap/imap_providers.h"
#include "ui/app_dialog/app_dialog.h"

#include <functional>
#include <optional>

class StyledLineEdit;
class StyledButton;
class QLabel;
class QWidget;
class QTimer;
class OAuthLoopbackFlow;

namespace imap {
class ImapClient;
class ServerDetector;
} // namespace imap

class ImapAddAccountDialog : public AppDialog {
    Q_OBJECT
public:
    explicit ImapAddAccountDialog(QWidget *parent = nullptr);
    ~ImapAddAccountDialog() override;

    // Valid once accepted() has fired (login succeeded).
    imap::Credentials credentials() const { return _creds; }

    // Prompt hook for imap::AuthStrategy::setPrompt — shows the dialog and calls
    // `done` with the validated credentials, or std::nullopt if cancelled.
    static void prompt(QObject *parent, std::function<void(std::optional<imap::Credentials>)> done);

private:
    void onPrimary();                        // Continue (step 1) → Add (step 2)
    void revealDetails();                    // detect provider, show password/server fields
    void startResolve(const QString &email); // async MX/SRV/probe provider resolution
    void configureProvider();                // lay out step 2 from the resolved _provider
    void validateAndAccept();                // probe IMAP login, accept on success
    void startOAuth(); // passwordless: run the provider's OAuth flow, then probe XOAUTH2
    [[nodiscard]] bool oauthAvailable() const; // provider is OAuth + a client id is compiled in
    void               onDetect();             // re-run detection for the entered address
    void               setDetecting(bool on);  // disable inputs + show a "Detecting…" status
    void               setStatus(const QString &msg, bool error);

    int                   _step = 1;
    imap::ProviderInfo    _provider;
    imap::Credentials     _creds;
    imap::ImapClient     *_probe    = nullptr;
    imap::ServerDetector *_detector = nullptr;

    OAuthLoopbackFlow *_oauthFlow = nullptr;

    StyledLineEdit *_email         = nullptr;
    StyledButton   *_oauthBtn      = nullptr; // "Sign in with Google" (OAuth providers)
    StyledLineEdit *_password      = nullptr;
    QWidget        *_serverRow     = nullptr; // [_imapHost | Detect]
    StyledLineEdit *_imapHost      = nullptr; // only shown for unknown providers
    StyledButton   *_detectBtn     = nullptr;
    QTimer         *_detectSpinner = nullptr;
    int             _spinFrame     = 0;
    QLabel         *_help          = nullptr;
    QLabel         *_status        = nullptr;
    StyledButton   *_primary       = nullptr;
};
