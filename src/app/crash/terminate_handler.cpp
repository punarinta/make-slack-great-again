// Replaces libstdc++'s default std::terminate handler (Linux only,
// src/app/screens/shell/CMakeLists.txt).
//
// The stock __gnu_cxx::__verbose_terminate_handler prints the uncaught exception's
// demangled type name, and so drags libstdc++'s whole C++ demangler (62 KB) into the
// static release binary for one error line. Defining the handler here keeps the
// linker from pulling the library's copy, and with it the demangler. The message
// is the same, with the type name left mangled (`c++filt -t` reads it); the abort()
// then reaches the crash handler, which logs the backtrace as before.

#include <exception> // defines __GLIBCXX__ when the library is libstdc++

#if defined(__GLIBCXX__)
#include <cxxabi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <typeinfo>
#include <unistd.h>

void __gnu_cxx::__verbose_terminate_handler() {
    char        buf[320];
    const char *msg = "terminate called without an active exception\n";
    if (const std::type_info *type = abi::__cxa_current_exception_type()) {
        std::snprintf(
            buf,
            sizeof buf,
            "terminate called after throwing an instance of '%s' (mangled)\n",
            type->name()
        );
        msg = buf;
    }
    [[maybe_unused]] const ssize_t written = ::write(STDERR_FILENO, msg, std::strlen(msg));
    std::abort();
}

#endif // __GLIBCXX__
