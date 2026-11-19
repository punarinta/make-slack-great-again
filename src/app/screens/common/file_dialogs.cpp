#include "screens/common/file_dialogs.h"

#include "base/file.h"
#include "base/i18n.h"

#include <cstdlib>
#include <cstring>

namespace screens {

namespace {

// The folder the last Open picked from, so the next attach starts there.
std::string &lastOpenDir() {
    static std::string dir;
    return dir;
}

#if defined(MSGA_FREEDESKTOP) || defined(MSGA_PORT) // the in-app chooser
bool forceBuiltin() {
    const char *v = std::getenv("MSGA_FILE_DIALOG");
    return v && std::strcmp(v, "builtin") == 0;
}

void builtin(Context &ctx, const plat::FileDialogDesc &d, PathsDone done) {
    if (!ctx.window) { // nothing to show it in
        ctx.app.platform().post([done = std::move(done)] { done({}); });
        return;
    }
    ui::FileBrowser::show(*ctx.window, d, std::move(done));
}
#endif

std::string startIn(Context &ctx, std::string dir) {
    return dir.empty() ? ctx.app.platform().standardDir(plat::StandardDir::Downloads) : dir;
}

PathsDone firstOnly(PathDone done) {
    return [done = std::move(done)](std::vector<std::string> p) {
        done(p.empty() ? std::string() : std::move(p[0]));
    };
}

} // namespace

void fileDialog(Context &ctx, plat::FileDialogDesc d, PathsDone done) {
    d.parent = ctx.window ? &ctx.window->native() : nullptr;
#if defined(MSGA_FREEDESKTOP) || defined(MSGA_PORT) // the in-app chooser
    if (forceBuiltin()) {
        builtin(ctx, d, std::move(done));
        return;
    }
    Context *c = &ctx; // outlives every window and dialog (owned by main)
    ctx.app.platform().showFileDialogEx(
        d, [c, d, done = std::move(done)](plat::FileDialogResult r) mutable {
            if (r.status == plat::FileDialogResult::Status::Unavailable) {
                builtin(*c, d, std::move(done));
                return;
            }
            done(std::move(r.paths));
        }
    );
#else
    ctx.app.platform().showFileDialogEx(d, [done = std::move(done)](plat::FileDialogResult r) {
        done(std::move(r.paths)); // Unavailable has no paths: a cancel
    });
#endif
}

void pickFiles(
    Context                      &ctx,
    PathsDone                     done,
    bool                          multiple,
    std::vector<plat::FileFilter> filters,
    std::string                   startDir
) {
    plat::FileDialogDesc d;
    d.mode = multiple ? plat::FileDialogDesc::Mode::OpenMultiple : plat::FileDialogDesc::Mode::Open;
    d.title      = multiple ? i18n::tr("Attach files") : i18n::tr("Attach a file");
    d.initialDir = !startDir.empty() && file::isDir(startDir) ? std::move(startDir) : lastOpenDir();
    d.filters    = std::move(filters);
    fileDialog(ctx, std::move(d), [done = std::move(done)](std::vector<std::string> p) {
        if (!p.empty())
            lastOpenDir() = std::string(file::dirName(p[0]));
        done(std::move(p));
    });
}

void saveFile(Context &ctx, std::string suggestedName, PathDone done, std::string startDir) {
    plat::FileDialogDesc d;
    d.mode          = plat::FileDialogDesc::Mode::Save;
    d.title         = i18n::tr("Save file");
    d.initialDir    = startIn(ctx, std::move(startDir));
    d.suggestedName = std::move(suggestedName);
    fileDialog(ctx, std::move(d), firstOnly(std::move(done)));
}

void pickFolder(Context &ctx, PathDone done, std::string startDir) {
    plat::FileDialogDesc d;
    d.mode       = plat::FileDialogDesc::Mode::PickFolder;
    d.title      = i18n::tr("Choose folder");
    d.initialDir = startIn(ctx, std::move(startDir));
    fileDialog(ctx, std::move(d), firstOnly(std::move(done)));
}

std::vector<std::string> droppedFiles(const plat::Event *raw) {
    return raw ? localPaths(raw->uris) : std::vector<std::string>();
}

std::vector<std::string> uriListPaths(std::string_view list) {
    std::vector<std::string> uris;
    size_t                   a = 0;
    while (a < list.size()) {
        size_t b = list.find('\n', a);
        if (b == std::string_view::npos)
            b = list.size();
        std::string_view line = list.substr(a, b - a);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (!line.empty() && line[0] != '#')
            uris.emplace_back(line);
        a = b + 1;
    }
    return localPaths(uris);
}

std::vector<std::string> localPaths(const std::vector<std::string> &uris) {
    std::vector<std::string> out;
    for (const std::string &u : uris)
        if (std::string p = file::fromFileUrl(u); !p.empty())
            out.push_back(std::move(p));
    return out;
}

bool dragOffersFiles(const plat::Event *raw) {
    if (!raw)
        return false;
    if (!raw->uris.empty())
        return true;
    for (const auto &it : raw->items)
        if (it.mime == "text/uri-list")
            return true;
    return false;
}

} // namespace screens
