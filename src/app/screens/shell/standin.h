// A plain message list the shell shows until the messages screens'
// MessageList / ThreadPanel are linked in (app/screens/messages): avatar,
// name, time and the text of each message, rebuilt on every Store change of
// the conversation. Deliberately simple — it only keeps the shell usable and
// testable; delete it once the real list is embedded everywhere.
#pragma once

#include "screens/common/context.h"
#include "screens/shell/avatars.h"
#include "ui/ui.h"

namespace shell {

class MessageStandIn : public ui::ScrollView {
public:
    MessageStandIn(screens::Context &ctx, Avatars &avatars);
    ~MessageStandIn() override;
    // thread == 0: the conversation's top-level messages; else the root and
    // its replies.
    void setTarget(model::ConvRef conv, model::Ts thread);

private:
    void rebuild();

    screens::Context        &_ctx;
    Avatars                 &_avatars;
    model::ConvRef           _conv     = model::kNoConv;
    model::Ts                _thread   = 0;
    model::Store::ObserverId _observer = 0;
};

} // namespace shell
