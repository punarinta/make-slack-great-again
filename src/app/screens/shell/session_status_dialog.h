// "Session status" (an agent session's /status):
// label / value rows about the conversation (Backend::runLocalCommand) —
// version, model, account, folder — the labels in text.secondary, the
// values in text.primary with the value as their tooltip (a long path may
// be cut off), and Close. Wider than most dialogs (up to 760): ids and
// paths read best on one line.
#pragma once

#include "ui/ui.h"

#include <string>
#include <utility>
#include <vector>

namespace shell {

class SessionStatusDialog {
public:
    static ui::Dialog *
    show(ui::Window &w, const std::vector<std::pair<std::string, std::string>> &rows);
};

} // namespace shell
