#include "screens/shell/typing_indicator.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"

#include <algorithm>

using namespace ui;
using i18n::tr;

namespace shell {

namespace {

// Appends a translated template: "<b>…</b>" runs are bold, "%1" is names().
template <typename Names>
void appendTemplate(
    text::AttributedText &t,
    std::string_view      fmt,
    const text::Style    &plain,
    const text::Style    &bold,
    Names                 names
) {
    const text::Style *cur   = &plain;
    size_t             run   = 0;
    auto               flush = [&](size_t end) {
        if (end > run)
            t.append(fmt.substr(run, end - run), *cur);
    };
    for (size_t i = 0; i < fmt.size();) {
        if (fmt.compare(i, 3, "<b>") == 0 || fmt.compare(i, 4, "</b>") == 0) {
            flush(i);
            const bool open = fmt[i + 1] == 'b';
            cur             = open ? &bold : &plain;
            i += open ? 3 : 4;
            run = i;
        } else if (fmt.compare(i, 2, "%1") == 0) {
            flush(i);
            names();
            i += 2;
            run = i;
        } else {
            ++i;
        }
    }
    flush(fmt.size());
}

} // namespace

TypingIndicator::TypingIndicator(screens::Context &ctx) : _ctx(ctx) {
    style().padding(12, 1, 12, 1);
    _label = add<Label>("", Font::Caption, C::TextMuted);
    _label->setMaxLines(1);
    setVisible(false);
    _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Typing && ch.conv == _conv)
            refresh();
    });
}

TypingIndicator::~TypingIndicator() {
    _ctx.store.unobserve(_observer);
    _ctx.app.cancelTimer(_tick);
}

std::string TypingIndicator::formatElapsed(int64_t ms) {
    const int64_t s = std::max<int64_t>(0, ms / 1000);
    if (s < 60)
        return i18n::arg(tr("%1s"), str::number(s));
    if (s < 3600)
        return i18n::arg(tr("%1m %2s"), str::number(s / 60), str::number(s % 60));
    return i18n::arg(tr("%1h %2m"), str::number(s / 3600), str::number(s / 60 % 60));
}

void TypingIndicator::setTarget(model::ConvRef conv, model::Ts thread) {
    _conv   = conv;
    _thread = thread;
    refresh();
}

TypingState typingStateOf(const model::Store &store, model::ConvRef conv, model::Ts thread) {
    TypingState out;
    out.thinking = true;
    if (conv != model::kNoConv)
        for (const model::Typing &t : store.typing(conv))
            if (t.thread == thread) {
                out.who.push_back(t.user);
                out.since = t.sinceMs;
                out.thinking &= t.sinceMs > 0;
            }
    out.thinking = out.thinking && !out.who.empty();
    return out;
}

void TypingIndicator::refresh() {
    const TypingState state    = typingStateOf(_ctx.store(), _conv, _thread);
    const auto       &who      = state.who;
    const int64_t     since    = state.since;
    const bool        thinking = state.thinking;
    // A timer ticks the "thinking" clocks every second.
    if (thinking && !_tick)
        _tick = _ctx.app.addTimer(1000, true, [this] { refresh(); });
    else if (!thinking && _tick) {
        _ctx.app.cancelTimer(_tick);
        _tick = 0;
    }
    setVisible(!who.empty());
    if (who.empty()) {
        _label->setText({});
        return;
    }
    text::Style bold = font(Font::Caption), plain = bold;
    bold.weight = text::Weight::Bold;
    bold.color = plain.color = themed(C::TextMuted);
    text::AttributedText t;
    const auto           me = _ctx.store().me;
    // A lone self typer: only ever one of my other clients (we never echo ours).
    if (who.size() == 1 && who[0] == me) {
        appendTemplate(
            t, tr("<b>You</b> are typing on another device\xE2\x80\xA6"), plain, bold, [] {}
        );
    } else {
        appendTemplate(
            t,
            thinking && who.size() == 1
                // %1 kept for the names (a single pass: "%1" isn't re-read)
                ? i18n::arg(
                      tr("%1 is thinking (%2)\xE2\x80\xA6"),
                      "%1",
                      formatElapsed(base::nowMicros() / 1000 - since)
                  )
                : thinking          ? std::string(tr("%1 are thinking\xE2\x80\xA6"))
                  : who.size() == 1 ? std::string(tr("%1 is typing\xE2\x80\xA6"))
                                    : std::string(tr("%1 are typing\xE2\x80\xA6")),
            plain,
            bold,
            [&] {
                for (size_t i = 0; i < who.size(); ++i) {
                    if (i)
                        t.append(", ", plain);
                    t.append(
                        who[i] == me ? std::string_view(tr("You"))
                                     : _ctx.store().user(who[i]).label(),
                        bold
                    );
                }
            }
        );
    }
    // The tick with nothing new to say (several thinking, the clock not
    // shown) keeps the line as laid out.
    if (t.text == _label->text() && plain == _shownStyle)
        return;
    _shownStyle = plain;
    _label->setRichText(std::move(t));
}

} // namespace shell
