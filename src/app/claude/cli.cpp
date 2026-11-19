#include "app/claude/cli.h"

#include "app/claude/launcher.h"
#include "app/claude/roster.h"
#include "base/file.h"
#include "base/host.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/process.h"
#include "plat/plat.h"

#include <memory>

namespace claude {

namespace {

using base::isExecutable; // on the host in a Flatpak sandbox, as is the CLI

std::string nativeSeparators(std::string path) {
#ifdef _WIN32
    for (char &c : path)
        if (c == '/')
            c = '\\';
#endif
    return path;
}

} // namespace

auth::WorkspaceRecord toRecord(const Credentials &creds) {
    auth::WorkspaceRecord rec;
    rec.service     = kService;
    rec.id          = kWorkspaceId;
    rec.displayName = "Claude Code";
    json::Writer w;
    w.beginObject().key("claudePath").value(creds.claudePath);
    if (!creds.sshDestination.empty()) {
        w.key("ssh").value(creds.sshDestination);
        if (!creds.remoteMsga.empty())
            w.key("remoteMsga").value(creds.remoteMsga);
    }
    w.endObject();
    rec.auth = w.take();
    return rec;
}

Credentials fromRecord(const auth::WorkspaceRecord &rec) {
    Credentials    c;
    json::Document doc;
    if (doc.parse(std::string(rec.auth))) {
        c.claudePath     = std::string(doc.root()["claudePath"].str());
        c.sshDestination = std::string(doc.root()["ssh"].str());
        c.remoteMsga     = std::string(doc.root()["remoteMsga"].str());
    }
    // The CLI that matters then is the server's: the relay finds it there.
    if (!c.sshDestination.empty())
        return c;
    // The CLI may have moved (reinstalled, switched installer) since the
    // workspace was added: fall back to looking again.
    if (!isExecutable(c.claudePath))
        c.claudePath = findClaudeExecutable();
    return c;
}

std::string findClaudeExecutable() {
    // Seen through the host's files it might be found, but it could only be
    // run in the sandbox, where it doesn't work.
    if (base::host::hostAccessRevoked())
        return {};
#ifdef _WIN32
    const char *const names[] = {"claude.exe", "claude.cmd"};
#else
    const char *const names[] = {"claude"};
#endif
    for (const char *n : names)
        if (std::string p = base::findExecutable(n); !p.empty())
            return p;
    // A GUI app often starts with a thinner PATH than a login shell, so also
    // try where the installers put the binary ('/'-separated home, see below).
    const std::string        home = base::homeDir();
    std::vector<std::string> extra;
    if (!home.empty()) {
        extra.push_back(home + "/.local/bin");
        extra.push_back(home + "/.claude/local");
        extra.push_back(home + "/.npm-global/bin");
    }
    extra.emplace_back("/opt/homebrew/bin");
    extra.emplace_back("/usr/local/bin");
#ifdef _WIN32
    if (!home.empty())
        extra.push_back(home + "/AppData/Roaming/npm");
#endif
    for (const char *n : names)
        for (const std::string &dir : extra) {
            // Native separators: cmd.exe, which starts an npm install's
            // claude.cmd (Launcher), takes a '/' for a switch.
            const std::string p = nativeSeparators(file::join(dir, n));
            if (isExecutable(p))
                return p;
        }
    return {};
}

std::string notInstalledMessage() {
    if (base::host::hostAccessRevoked()) // the Flatpak, its host access taken away
        return i18n::tr(
            "Claude Code runs on your computer, outside msga's sandbox, and this Flatpak's "
            "access to it is turned off. Allow \"org.freedesktop.Flatpak\" and \"All system "
            "files\" for msga in Flatseal (or run `flatpak override --user "
            "--talk-name=org.freedesktop.Flatpak --filesystem=host com.nisdos.msga`), then "
            "restart msga."
        );
    return i18n::tr(
        "Claude Code isn't installed on this computer (msga can't find the claude command). "
        "Install it (https://code.claude.com/docs/en/setup), run `claude` once in a terminal "
        "to log in, then try again."
    );
}

std::string notLoggedInMessage() {
    return i18n::tr(
        "Claude Code isn't logged in on this computer. Run `claude` in a terminal and log in "
        "with /login, then try again."
    );
}

void checkSetup(plat::App &app, std::function<void(Credentials creds, std::string error)> done) {
    const std::string claude = findClaudeExecutable();
    if (claude.empty()) {
        app.post([done = std::move(done)] { done({}, notInstalledMessage()); });
        return;
    }
    const Paths paths = Paths::detect();
    if (!file::isDir(paths.home)) {
        std::string error = i18n::arg(
            i18n::tr(
                "Claude Code doesn't seem to be set up on this computer: %1 doesn't exist. Run "
                "`claude` once in a terminal, then add the workspace again."
            ),
            nativeSeparators(paths.home)
        );
        app.post([done = std::move(done), error = std::move(error)] { done({}, error); });
        return;
    }
    // A login that can't be read (an old CLI) lets the workspace in: a session
    // that then finds none says so (see the transcript's login error).
    auto launcher = std::make_shared<Launcher>(app, claude, paths);
    launcher->checkLogin([launcher, claude, done = std::move(done)](Login login) mutable {
        // The launcher goes with this callback (it is called once).
        if (login == Login::Out)
            done({}, notLoggedInMessage());
        else
            done(Credentials{claude}, {});
    });
}

} // namespace claude
