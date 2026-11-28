// detail::loopbackPort() over POSIX sockets: the Linux transport and the
// macOS one (NSURLSession has no API for it) both use this file.
#include "net/transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace net::detail {

int loopbackPort() {
#ifdef SOCK_CLOEXEC
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0); // macOS: closed again right below
#endif
    if (fd < 0)
        return 0;
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len     = sizeof a;
    int       port    = 0;
    if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0 &&
        getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len) == 0)
        port = ntohs(a.sin_port);
    ::close(fd);
    return port;
}

} // namespace net::detail
