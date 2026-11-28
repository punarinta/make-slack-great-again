// A program run in a pseudo-terminal of its own, for driving a terminal UI
// (`claude attach`, see attach.h): it sees a real terminal of the given size,
// what it draws arrives through onOutput, and write() is typing. POSIX
// pseudo-terminals on Linux and macOS, ConPTY on Windows (10 1809 and later).
//
// Callbacks run on the plat loop, never after the Pty is destroyed; either
// may destroy it.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plat {
class App;
}

namespace claude {

class Pty {
public:
    explicit Pty(plat::App &app);
    ~Pty(); // ends the program if it still runs
    Pty(const Pty &)            = delete;
    Pty &operator=(const Pty &) = delete;

    // Start `program` with `args` in `cwd`, the environment inherited plus
    // TERM=xterm-256color. False (errorString() says why) when it couldn't.
    bool start(
        const std::string              &program,
        const std::vector<std::string> &args,
        const std::string              &cwd,
        int                             rows,
        int                             cols
    );
    void               write(std::string_view bytes);
    // Ends the program (SIGTERM, SIGKILL 2 s later / TerminateProcess);
    // onFinished follows.
    void               terminate();
    bool               isRunning() const;
    const std::string &errorString() const { return _error; }

    std::function<void(std::string_view bytes)> onOutput;
    std::function<void()>                       onFinished; // it exited, or its terminal closed

private:
    struct Impl;
    plat::App            &_app;
    std::unique_ptr<Impl> d;
    std::string           _error;
    std::shared_ptr<bool> _alive; // false once destroyed: posted work then does nothing
};

} // namespace claude
