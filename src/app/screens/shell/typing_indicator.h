// The typing indicator, between the message list and the composer:
// "<b>Mira</b> is typing…", "<b>Mira</b>, <b>Jonas</b> are typing…", or
// "<b>You</b> are typing on another device…" — hidden (no height) while
// nobody types. Follows Store typing for one conversation (and thread).
// An agent at work (Typing::sinceMs) is "<b>Claude</b> is thinking (1m 5s)…"
// with the clock ticking every second ("… are thinking…" for several).
#pragma once

#include "screens/common/context.h"
#include "ui/ui.h"

namespace shell {

// Who's typing in `conv`'s `thread` right now, and whether every one of
// them is an agent at work rather than a person mid-sentence (Typing::
// sinceMs > 0 for all of them) -- "thinking" only if there's anyone to
// think; `since` is whichever row's, for the single-agent "(1m 5s)" clock.
struct TypingState {
    std::vector<model::UserRef> who;
    int64_t                     since    = 0;
    bool                        thinking = false;
};
TypingState typingStateOf(const model::Store &store, model::ConvRef conv, model::Ts thread);

class TypingIndicator : public ui::View {
public:
    explicit TypingIndicator(screens::Context &ctx);
    ~TypingIndicator() override;

    void               setTarget(model::ConvRef conv, model::Ts thread);
    const std::string &text() const { return _label->text(); }

private:
    void refresh();
    // Elapsed time: "42s", "1m 5s", "2h 3m".
public:
    static std::string formatElapsed(int64_t ms);

private:
    screens::Context        &_ctx;
    ui::Label               *_label;
    model::ConvRef           _conv     = model::kNoConv;
    model::Ts                _thread   = 0;
    model::Store::ObserverId _observer = 0;
    plat::TimerId            _tick     = 0; // while someone is thinking
    text::Style              _shownStyle;   // the plain style of the text shown
};

} // namespace shell
