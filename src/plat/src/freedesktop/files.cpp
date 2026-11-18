#include "freedesktop/files.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace plat::linux_files {

std::optional<std::string> readFile(const std::string &path) {
    std::string out;
    if (!prim::file::readRange(path, 0, SIZE_MAX, &out))
        return std::nullopt;
    return out;
}

bool isFile(const std::string &path) {
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

int sharedMemoryFd(const char *name, size_t bytes) {
    int fd = -1;
#if defined(MFD_CLOEXEC)
    fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
#endif
    if (fd < 0) {
        const char *dir  = std::getenv("XDG_RUNTIME_DIR");
        std::string path = std::string(dir && *dir ? dir : "/tmp") + "/plat-shm-XXXXXX";
        fd               = mkostemp(path.data(), O_CLOEXEC);
        if (fd < 0)
            return -1;
        unlink(path.c_str());
    }
    int r;
    do
        r = ftruncate(fd, off_t(bytes));
    while (r < 0 && errno == EINTR);
    if (r < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

} // namespace plat::linux_files
