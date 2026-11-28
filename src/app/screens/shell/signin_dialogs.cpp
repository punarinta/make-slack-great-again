#include "screens/shell/signin_dialogs.h"

#include "app/slack/browser_login.h"
#include "app/slack/local_import.h"
#include "app/slack/session.h"
#include "base/i18n.h"
#include "base/str.h"
#include "net/net.h"
#include "screens/shell/shell_text.h"

#include <memory>

namespace shell {

using namespace ui;
using i18n::tr;
using V = Button::Kind;

namespace {

std::string friendlyImportError(const std::string &reason) {
    if (reason == "not_installed")
        return tr("The Slack desktop app wasn't found on this computer.");
    if (reason == "locked")
        return tr("Couldn't read Slack's data \xE2\x80\x94 try quitting the Slack app first.");
    if (reason == "decrypt_failed" || reason == "no_cookie")
        return tr("Couldn't read Slack's saved session automatically.");
    if (reason == "unsupported_platform")
        return tr("Automatic import isn't available in this build.");
    return tr("Automatic import didn't work.");
}

std::string friendlyBrowserError(const std::string &reason) {
    if (reason == "no_browser")
        return tr(
            "No supported browser was found \xE2\x80\x94 browser sign-in needs Chrome, "
            "Chromium, Brave, Edge or Vivaldi."
        );
    if (reason == "launch_failed")
        return tr("Couldn't start your browser.");
    if (reason == "no_devtools" || reason == "cdp_failed")
        return tr("Couldn't read the session back from the browser window.");
    if (reason == "cancelled")
        return tr("Browser sign-in was cancelled.");
    if (reason == "timeout")
        return tr("Browser sign-in timed out.");
    return tr("Browser sign-in didn't work.");
}

// A link button: link-coloured text that acts on a click.
Label *linkButton(View *parent, std::string text, std::function<void()> fn) {
    text::Style st = pxFont(13, text::Weight::Regular, themed(C::FormLink));
    st.linkId      = 1;
    st.underline   = true;
    auto *l        = parent->add<Label>();
    setStyledText(l, text, st);
    l->style().alignSelf(Align::Start);
    l->onLink = [fn = std::move(fn)](uint32_t) { fn(); };
    return l;
}

class SessionImportDialog final : public Dialog {
public:
    SessionImportDialog(screens::Context &ctx, net::Client &client, SessionImportHooks hooks)
        : Dialog(tr("Add Slack workspace with a session token"), 760), _ctx(ctx), _client(client),
          _hooks(std::move(hooks)) {
        View *c = content();
        c->style().spacing(12); // sp.lg
        const std::string browser    = slack::browserLoginName();
        const bool        canBrowser = !browser.empty();
        const bool        canLocal   = slack::localImportSupported();
        text(
            c,
            canBrowser
                ? tr("Log in to Slack in a browser window and msga uses that session instead of "
                     "app "
                     "keys \xE2\x80\x94 so it runs on your account's own rate limits and avoids "
                     "the "
                     "shared-key timeouts. New messages arrive by polling (there's no live push "
                     "this way).")
                : tr("Sign in with your existing Slack session instead of app keys \xE2\x80\x94 it "
                     "uses your account's own rate limits, so it avoids the shared-key timeouts. "
                     "New messages arrive by polling (there's no live push this way).")
        );
        _browserBtn = c->add<Button>(
            i18n::arg(tr("Sign in with %1"), browser), V::Primary, Button::Form::Normal
        );
        _browserBtn->setVisible(canBrowser);
        _browserBtn->onClick = [this] { startBrowserLogin(); };

        _importBtn = c->add<Button>(
            tr("Import from local Slack"),
            canBrowser ? V::Secondary : V::Primary,
            Button::Form::Normal
        );
        _importBtn->setVisible(canLocal);
        _importBtn->onClick = [this] { tryLocalImport(); };

        _manualToggle = linkButton(c, tr("Paste a session cookie instead"), [this] {
            // Giving up on the automatic paths: close any browser window we
            // opened (or the import running) so it can't report back over the
            // manual fields.
            _browser.reset();
            _import.reset();
            revealManual({}, true);
        });

        _steps = text(
            c,
            tr("1. Sign in to the workspace in your browser.\n"
               "2. Open developer tools (F12) \xE2\x86\x92 Application \xE2\x86\x92 Cookies "
               "\xE2\x86\x92 "
               "https://app.slack.com, and copy the value of the cookie named \xE2\x80\x9C"
               "d\xE2\x80\x9D (it starts with xoxd-) into Cookie. Browsers hide this cookie from "
               "scripts, so it has to be copied by hand.\n"
               "3. Enter your workspace address.")
        );
        _steps->setVisible(false);
        _cookie = c->add<TextField>("xoxd-\xE2\x80\xA6", TextField::Size::Small);
        _cookie->setPrefix(tr("Cookie"));
        _cookie->setMasked(true, true);
        _cookie->setVisible(false);
        _workspace = c->add<TextField>("myteam.slack.com", TextField::Size::Small);
        _workspace->setPrefix(tr("Workspace"));
        _workspace->setVisible(false);
        _workspace->onReturn = [this] { submitManual(); };

        _status = c->add<Label>();
        _status->setVisible(false);

        linkButton(c, tr("Use app keys (OAuth) instead"), [this] {
            auto fn = _hooks.useAppKeys;
            reject();
            if (fn)
                fn();
        });

        _submit = makeButton(tr("Add workspace"), V::Primary);
        _submit->setVisible(false);
        _submit->onClick = [this] { submitManual(); };
        addButtonRow(_submit, makeButton(tr("Cancel"), V::Secondary));

        // No automatic path at all: straight to the guided manual flow.
        if (!canBrowser && !canLocal)
            revealManual({}, true);
    }

private:
    Label *text(View *parent, std::string s) {
        // The default label font (15 px), text.primary.
        return styledLabel(
            parent, std::move(s), pxFont(15, text::Weight::Regular, color(C::FormText))
        );
    }

    void startBrowserLogin() {
        setBusy(true);
        setStatus(tr("Opening a browser window\xE2\x80\xA6"), false);
        // A fresh one per attempt; owned here, so closing the dialog kills the
        // browser and wipes its throwaway profile.
        _browser = std::make_unique<slack::BrowserLogin>(_ctx.app.platform(), _client);
        _browser->start(
            [this](std::string msg) { setStatus(msg, false); },
            [this](std::string cookie, std::vector<slack::TeamSession> teams, std::string error) {
                if (!error.empty() || cookie.empty()) {
                    setBusy(false);
                    revealManual(friendlyBrowserError(error), error != "cancelled");
                    return;
                }
                // Signed in, but the web client never showed which workspaces
                // the account has: ask for the address, the cookie filled in.
                if (teams.empty()) {
                    _cookie->setText(cookie);
                    revealManual(tr("Signed in \xE2\x80\x94 enter your workspace address."), false);
                    return;
                }
                deriveAndFinish(std::move(cookie), std::move(teams));
            }
        );
    }

    void tryLocalImport() {
        setBusy(true);
        setStatus(tr("Importing from local Slack\xE2\x80\xA6"), false);
        // Off the UI thread; the dialog closing (or giving up on it) drops
        // the result.
        _import = std::make_shared<char>(0);
        slack::importLocalSessionAsync(
            _ctx.app.platform(),
            [this, alive = std::weak_ptr<char>(_import)](slack::LocalImport imp) {
                if (alive.expired())
                    return;
                _import.reset();
                localImported(std::move(imp));
            }
        );
    }

    void localImported(slack::LocalImport imp) {
        if (!imp.ok()) {
            setBusy(false);
            revealManual(friendlyImportError(imp.error), true);
            return;
        }
        if (imp.teams.empty()) {
            _cookie->setText(imp.cookie);
            revealManual(
                tr("Found your Slack session \xE2\x80\x94 enter your workspace address."), false
            );
            return;
        }
        deriveAndFinish(std::move(imp.cookie), std::move(imp.teams));
    }

    void submitManual() {
        std::string cookie = slack::normalizeCookie(_cookie->text());
        std::string url    = slack::normalizeWorkspaceUrl(_workspace->text());
        if (cookie.empty()) {
            setStatus(
                tr("Paste the \xE2\x80\x9C"
                   "d\xE2\x80\x9D cookie value."),
                true
            );
            return;
        }
        if (url.empty()) {
            setStatus(tr("Enter your workspace address (e.g. myteam.slack.com)."), true);
            return;
        }
        slack::TeamSession cand;
        cand.workspaceUrl = std::move(url);
        deriveAndFinish(std::move(cookie), {std::move(cand)});
    }

    void deriveAndFinish(std::string cookie, std::vector<slack::TeamSession> candidates) {
        setBusy(true);
        setStatus(tr("Verifying your session\xE2\x80\xA6"), false);
        _deriver = std::make_unique<slack::TokenDeriver>(_client);
        _deriver->run(
            std::move(cookie),
            std::move(candidates),
            [this](std::vector<slack::Credentials> valid, std::string error) {
                setBusy(false);
                if (valid.empty()) {
                    std::string why =
                        tr("Couldn't verify that session. Check the cookie and "
                           "workspace address and try again.");
                    if (error == "invalid_auth")
                        why =
                            tr("That session was rejected \xE2\x80\x94 the cookie may have "
                               "expired. Sign in to Slack again and copy a fresh cookie.");
                    else if (error == "token_not_found")
                        // The boot page carried no token: a stale cookie, or an
                        // account that isn't a member of that workspace.
                        why =
                            tr("That workspace loaded but Slack didn't hand out a session token "
                               "\xE2\x80\x94 the cookie has probably expired, or it belongs to an "
                               "account without access to that workspace. Sign in to Slack again "
                               "and copy a fresh cookie.");
                    revealManual(why, true);
                    return;
                }
                auto fn = _hooks.imported;
                accept(); // closes (and destroys) the dialog
                if (fn)
                    fn(std::move(valid));
            }
        );
    }

    void revealManual(const std::string &notice, bool error) {
        setBusy(false);
        _steps->setVisible(true);
        _cookie->setVisible(true);
        _workspace->setVisible(true);
        _submit->setVisible(true);
        _manualToggle->setVisible(false); // redundant now
        if (!notice.empty())
            setStatus(notice, error);
        _cookie->edit().focus();
    }

    void setBusy(bool busy) {
        _browserBtn->setEnabled(!busy);
        _importBtn->setEnabled(!busy);
        _submit->setEnabled(!busy);
        _cookie->setEnabled(!busy);
        _workspace->setEnabled(!busy);
    }

    void setStatus(const std::string &msg, bool error) {
        if (msg.empty()) {
            _status->setVisible(false);
            return;
        }
        setStyledText(
            _status,
            msg,
            pxFont(15, text::Weight::Regular, themed(error ? C::FormError : C::FormTextMuted))
        );
        _status->setVisible(true);
    }

    screens::Context                    &_ctx;
    net::Client                         &_client;
    SessionImportHooks                   _hooks;
    Button                              *_browserBtn = nullptr, *_importBtn = nullptr;
    Button                              *_submit       = nullptr;
    Label                               *_manualToggle = nullptr, *_steps = nullptr;
    Label                               *_status = nullptr;
    TextField                           *_cookie = nullptr, *_workspace = nullptr;
    std::unique_ptr<slack::BrowserLogin> _browser;
    std::unique_ptr<slack::TokenDeriver> _deriver;
    std::shared_ptr<char>                _import; // a local import running
};

} // namespace

Popup *showMessage(Window &w, std::string title, std::string text) {
    auto d = std::make_unique<Dialog>(std::move(title));
    styledLabel(
        d->content(), std::move(text), pxFont(15, text::Weight::Regular, color(C::FormText))
    );
    auto   *ok  = Dialog::makeButton(tr("OK"), V::Primary);
    Dialog *raw = d.get();
    ok->onClick = [raw] { raw->accept(); };
    d->addButtonRow(ok, nullptr);
    w.showPopup(std::move(d));
    return raw;
}

Popup *showSessionImportDialog(
    screens::Context &ctx, Window &w, net::Client &client, SessionImportHooks hooks
) {
    auto  d   = std::make_unique<SessionImportDialog>(ctx, client, std::move(hooks));
    auto *raw = d.get();
    w.showPopup(std::move(d));
    return raw;
}

} // namespace shell
