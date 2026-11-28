// The fake AI provider (fake_llm.py) for test suites: started once per
// process on a free loopback port. POSIX only (server() is "" elsewhere,
// and when python3 is missing: callers skip).
#pragma once

#include "net/net.h"

#include <string>

namespace plat {
class App;
}

namespace fakellm {

// "http://127.0.0.1:port"; the first call starts the server.
const std::string &server();
// A control request (/_ctl/reset, /_ctl/script, /_ctl/log), pumping `app`
// until it is answered.
net::Response
ctl(plat::App &app, const char *method, const std::string &path, std::string body = {});
// Queues answers for a path: script(app, "/v1/messages", "[{…}, …]").
void script(plat::App &app, const char *path, const char *responsesJson);

} // namespace fakellm
