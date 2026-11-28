#include "screens/shell/desktop_entry.h"

#include "app/identity.h"
#include "app/model/jobs.h"
#include "base/file.h"
#include "base/log.h"
#include "base/process.h"
#include "base/str.h"
#include "plat/plat.h"

#include <string_view>

#if defined(MSGA_FREEDESKTOP) && defined(MSGA_INSTALL_LAUNCHER)
// gfx/icon_256.png, embedded by CMake.
extern const unsigned char kMsgaIconPng[];
extern const unsigned      kMsgaIconPngSize;
#endif

namespace shell {

std::string desktopEntry(const std::string &exePath, const std::string &icon) {
    const std::string exec =
        exePath.find(' ') != std::string::npos ? str::concat({"\"", exePath, "\""}) : exePath;
    // The template is gfx/msga.desktop.
    return str::concat({
        "[Desktop Entry]\n"
        "Name=MSGA\n"
        "Comment=Fast native Slack client\n"
        "Exec=",
        exec,
        " %u\n"
        "Icon=",
        icon,
        "\n"
        "Type=Application\n"
        "Categories=Network;InstantMessaging;\n"
        "StartupWMClass=msga\n"
        "MimeType=x-scheme-handler/msga;\n",
    });
}

// Without MSGA_INSTALL_LAUNCHER the entry comes from a package.
#if defined(MSGA_FREEDESKTOP) && defined(MSGA_INSTALL_LAUNCHER)

namespace {

enum class Write { Error, Unchanged, Written };

Write writeIfChanged(const std::string &path, std::string_view bytes) {
    std::string have;
    if (file::readAll(path, &have) && have == bytes)
        return Write::Unchanged;
    return file::writeAtomic(path, bytes) ? Write::Written : Write::Error;
}

} // namespace

void installDesktopEntry(plat::App &app) {
    const std::string data = app.standardDir(plat::StandardDir::Data);
    const std::string exe  = base::executablePath();
    model::runInBackground(
        app,
        [data, exe] {
            if (data.empty() || exe.empty() || exe.find_first_of("\n\r") != std::string::npos)
                return;
            const std::string icon = file::join(data, "icons/hicolor/256x256/apps/msga.png");
            const bool        iconOk =
                writeIfChanged(
                    icon,
                    std::string_view(reinterpret_cast<const char *>(kMsgaIconPng), kMsgaIconPngSize)
                ) != Write::Error;
            const std::string path =
                file::join(data, str::concat({"applications/", identity::appId(), ".desktop"}));
            if (writeIfChanged(path, desktopEntry(exe, iconOk ? icon : "msga")) != Write::Written)
                return;
            // The launcher changed: refresh the MIME/desktop database (best
            // effort; a missing tool is fine).
            const std::string udb = base::findExecutable("update-desktop-database");
            if (!udb.empty()) {
                base::RunOptions o;
                o.timeoutMs = 10000;
                base::run(udb, {std::string(file::dirName(path))}, o);
            }
            LOG_INFO("shell", "installed %s", path.c_str());
        },
        [&app] { app.registerUrlScheme(identity::kUrlScheme); }
    );
}

#else

void installDesktopEntry(plat::App &app) {
    app.registerUrlScheme(identity::kUrlScheme);
}

#endif

} // namespace shell
