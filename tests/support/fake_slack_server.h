// The fake Slack Web API (fake_slack.py) shared by the Slack test suites:
// started once per process, every SlackBackend's calls pointed at it
// (MSGA_SLACK_API_BASE), one plat::App to pump. POSIX only.
#pragma once

#include "net/net.h"
#include "plat/plat.h"

#include <functional>
#include <string>

namespace fakeslack {

plat::App         &app();
// Pumps the loop until done() or the time runs out; false on timeout.
bool               pumpUntil(const std::function<bool()> &done, int timeoutMs = 10000);
void               pumpFor(int ms);
// "http://127.0.0.1:port" ("" when python3 is missing). The first call
// starts the server and sets MSGA_SLACK_API_BASE to its /api/.
const std::string &server();
// A control request: /_ctl/reset, /_ctl/script, /_ctl/set, /_ctl/log.
net::Response      ctl(const char *method, const std::string &path, std::string body = {});

} // namespace fakeslack
