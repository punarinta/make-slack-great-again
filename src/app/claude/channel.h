// A line channel to a relay (relay_protocol.h): a child process whose stdin
// and stdout carry the lines — `ssh <destination> msga --cc-relay`, or in
// tests `msga --cc-relay` itself — or anything else a test plugs in.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace claude {

class Channel {
public:
    using Line   = std::function<void(std::string_view line)>;
    using Closed = std::function<void(std::string why)>;
    virtual ~Channel();

    // Starts it. Lines arrive through `onLine` in order, without their
    // newline — on a thread of the channel's, not the loop's; `onClosed`
    // once after the last, with why (the program's last words, or "").
    // False when it couldn't start (`onClosed` then never runs, *error says why).
    virtual bool open(Line onLine, Closed onClosed, std::string *error) = 0;
    // Queues a line (its newline is added): never blocks. Any thread.
    virtual void send(std::string line)                                 = 0;
    // Ends it: no callback runs once this returns.
    virtual void close()                                                = 0;
};

// The program `exe` with `args`, in a session of its own (no terminal: ssh
// can't ask for a password), its stderr kept for the error.
std::unique_ptr<Channel> processChannel(std::string exe, std::vector<std::string> args);

} // namespace claude
