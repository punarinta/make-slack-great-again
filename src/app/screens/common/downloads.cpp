#include "app/screens/common/downloads.h"

#include "app/model/jobs.h"
#include "app/screens/common/remote_images.h"
#include "base/crypto.h"
#include "base/file.h"
#include "base/str.h"
#include "plat/plat.h"

#include <memory>

namespace screens {

void fetchFile(
    plat::App           &app,
    model::Backend      &backend,
    const std::string   &source,
    std::string          toPath,
    model::Backend::Done done
) {
    if (RemoteImages::isRemote(source)) {
        backend.downloadFile(source, std::move(toPath), std::move(done));
        return;
    }
    auto ok = std::make_shared<bool>(false);
    model::runInBackground(
        app,
        [ok, from = source, toPath = std::move(toPath)] {
            *ok = from == toPath || file::copy(from, toPath);
        },
        [ok, done = std::move(done)] {
            if (done)
                done(*ok, *ok ? std::string() : std::string("copy_failed"));
        }
    );
}

std::string tempDownloadPath(plat::App &app, std::string_view name) {
    const std::string tmp = app.standardDir(plat::StandardDir::Temp);
    if (tmp.empty())
        return {};
    const std::string rnd = crypto::randomHex(8);
    if (rnd.empty())
        return {};
    const std::string dir = file::join(file::join(tmp, "msga/downloads"), rnd);
    if (!file::makeDirs(dir))
        return {};
    // Only the last path component, and never "" / "." / "..".
    std::string base(file::baseName(name));
    if (base.empty() || base == "." || base == "..")
        base = "file";
    return file::join(dir, base);
}

void removeTempDownload(const std::string &path) {
    file::remove(path);
    file::remove(file::dirName(path));
}

} // namespace screens
