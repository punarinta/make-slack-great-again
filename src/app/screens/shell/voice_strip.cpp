#include "screens/shell/voice_strip.h"

#include "base/i18n.h"
#include "base/str.h"
#include "base/utf8.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "screens/common/message_rules.h"
#include "screens/common/message_text.h"
#include "screens/shell/nav_chrome.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace shell {

using i18n::tr;
using ui::C;

namespace {

constexpr float kH       = 30; // ui::kFormSmallH
constexpr float kDot     = 8;  // recording dot diameter
constexpr float kMeterH  = 16; // tallest meter bar
constexpr float kBarW    = 2;
constexpr float kBarGap  = 2;
constexpr float kSpinner = 14; // processing spinner diameter
constexpr float kCancel  = 20;
constexpr float kSpSm = 4, kSpMd = 8; // spacing.sm / .md

// A single message beyond this is cut: the context is there for vocabulary
// and names, and one pasted log must not crowd out the rest.
constexpr size_t kMaxMessageChars = 500;
// Members beyond this add little and cost prompt space on huge channels.
constexpr size_t kMaxMembers      = 60;

} // namespace

VoiceStrip::VoiceStrip(plat::App &app) : _app(app), _spinner([this] { update(); }) {
    style().row().height(kH).noShrink();
    _cancel = add<GlyphButton>(
        gfx::Icon::X, kCancel, 12, C::FormTextMuted, tr("Cancel voice input (Esc)")
    );
    _cancel->setLook({C::None, C::FormDivider, C::FormDivider, C::None, 3});
    _cancel->setFocusable(false);
    _cancel->onClick = [this] {
        if (onCancel)
            onCancel();
    };
    setVisible(false);
}

VoiceStrip::~VoiceStrip() {
    if (_errorHide)
        _app.cancelTimer(_errorHide);
    if (_pulse)
        _app.cancelTimer(_pulse);
}

void VoiceStrip::setMode(Mode mode, std::string errorText) {
    const Mode was = _mode;
    _mode          = mode;
    _error         = mode == Mode::Error ? std::move(errorText) : std::string();
    if (mode == Mode::Recording && was != Mode::Recording) {
        _startMs = base::monotonicMs();
        _levels.fill(0.f);
        _head = 0;
    }
    if (mode == Mode::Recording && !_pulse)
        _pulse = _app.addTimer(50, true, [this] { update(); });
    else if (mode != Mode::Recording && _pulse)
        _app.cancelTimer(std::exchange(_pulse, 0));

    if (mode == Mode::Transcribing || mode == Mode::Cleaning) {
        if (!_spinner.running())
            _spinner.start();
    } else if (_spinner.running()) {
        _spinner.stop();
    }

    if (_errorHide)
        _app.cancelTimer(std::exchange(_errorHide, 0));
    if (mode == Mode::Error)
        _errorHide = _app.addTimer(kErrorMs, false, [this] {
            _errorHide = 0;
            if (_mode == Mode::Error)
                setMode(Mode::Hidden);
        });

    _cancel->setTooltip(mode == Mode::Error ? tr("Dismiss") : tr("Cancel voice input (Esc)"));
    setVisible(mode != Mode::Hidden);
    update();
}

void VoiceStrip::pushLevel(float peak) {
    if (_mode != Mode::Recording)
        return;
    _levels[size_t(_head)] = std::clamp(peak, 0.f, 1.f);
    _head                  = (_head + 1) % kBars;
    // The pulse timer repaints at a steady rate.
}

std::string VoiceStrip::elapsedText() const {
    const int64_t secs = _startMs ? (base::monotonicMs() - _startMs) / 1000 : 0;
    char          buf[24];
    std::snprintf(buf, sizeof buf, "%lld:%02lld", (long long)(secs / 60), (long long)(secs % 60));
    return buf;
}

void VoiceStrip::layout() {
    _cancel->setFrame(
        {width() - kSpSm - kCancel, std::floor((height() - kCancel) / 2), kCancel, kCancel}
    );
}

void VoiceStrip::styleChanged() {
    for (ShapedText &t : _texts)
        t.layout.reset(); // a theme or text size change
    View::styleChanged();
}

void VoiceStrip::paint(gfx::Painter &p) {
    if (_mode == Mode::Hidden)
        return;
    const float k     = windowScale();
    const float cy    = height() / 2;
    const float right = _cancel->frame().x - kSpSm;
    float       x     = kSpMd;

    // Optional pieces simply drop out on a narrow box; the error elides.
    size_t     slot     = 0;
    const auto drawText = [&](const std::string &s, C color, bool elide = false) {
        if (x >= right)
            return;
        ShapedText      &st   = _texts[std::min<size_t>(slot++, 1)];
        const gfx::Color col  = ui::color(color);
        const float      maxW = elide ? right - x : 1e9f;
        if (!st.layout || st.text != s || st.color != col || st.maxWidth != maxW || st.scale != k) {
            text::AttributedText t;
            t.append(s, ui::pxFont(12, text::Weight::Regular, col));
            text::LayoutOptions o;
            o.maxLines   = 1;
            o.ellipsis   = elide;
            o.maxWidth   = maxW;
            o.lineHeight = 1.2f;
            st.layout    = text::Layout::build(std::move(t), o, k);
            st.text      = s;
            st.color     = col;
            st.maxWidth  = maxW;
            st.scale     = k;
        }
        const text::Layout &l = *st.layout;
        if (!elide && x + l.width() > right)
            return;
        l.paint(p, snapPx({x, std::floor(cy - l.height() / 2)}));
        x += std::ceil(l.width()) + kSpMd;
    };

    switch (_mode) {
    case Mode::Recording: {
        // Pulsing dot: a ~1.2 s breathing cycle.
        const double phase = double((base::monotonicMs() - _startMs) % 1200) / 1200.0;
        const float  a     = float(0.45 + 0.55 * (0.5 + 0.5 * std::cos(phase * 2 * 3.14159265)));
        p.fillCircle({x + kDot / 2, cy}, kDot / 2, gfx::withAlpha(ui::color(C::Danger), a));
        x += kDot + kSpSm;
        // Level meter: oldest bar on the left, newest on the right, in
        // neutral ink (the accent can sit close to the box colour).
        for (int i = 0; i < kBars; ++i) {
            const float level = _levels[size_t((_head + i) % kBars)];
            // sqrt lifts quiet speech into view; a minimum keeps a baseline.
            const float h     = std::max(2.f, kMeterH * std::sqrt(level));
            p.fillRoundRect({x, cy - h / 2, kBarW, h}, kBarW / 2, ui::color(C::FormTextMuted));
            x += kBarW + kBarGap;
        }
        x += kSpMd - kBarGap;
        drawText(elapsedText(), C::FormText);
        // TRANSLATORS: Hint in the composer's voice-recording strip
        drawText(tr("Esc to cancel"), C::FormTextFaint);
        break;
    }
    case Mode::Transcribing:
    case Mode::Cleaning: {
        _spinner.paint(p, {x, cy - kSpinner / 2, kSpinner, kSpinner}, kSpinner);
        x += kSpinner + kSpSm;
        drawText(
            _mode == Mode::Transcribing ? tr("Transcribing\xE2\x80\xA6")
                                        : tr("Cleaning up\xE2\x80\xA6"),
            C::FormTextMuted
        );
        break;
    }
    case Mode::Error:
        drawText(_error, C::FormError, true);
        break;
    case Mode::Hidden:
        break;
    }
}

// ── Voice context ───────────────────────────────────────────────────────────

namespace {

// Rows that are people talking (a bot's post, a broadcast reply) as opposed
// to "joined the channel" style system rows.
bool isConversational(const model::Message &m) {
    const std::string &s = m.subtype();
    return s.empty() || s == "bot_message" || s == "thread_broadcast" || s == "me_message" ||
           s == "file_share";
}

void addUnique(std::vector<std::string> &v, const std::string &s, size_t cap = SIZE_MAX) {
    if (!s.empty() && v.size() < cap && std::find(v.begin(), v.end(), s) == v.end())
        v.push_back(s);
}

} // namespace

llm::VoiceContext
buildVoiceContext(const model::Store &st, model::ConvRef conv, model::Ts thread, int maxMessages) {
    llm::VoiceContext out;
    if (conv >= st.conversationCount())
        return out;
    const model::Conversation &c     = st.conversation(conv);
    const auto                 label = [&](model::UserRef u) {
        return u == model::kNoUser ? std::string() : std::string(st.user(u).label());
    };

    // Recent messages, the newest maxMessages, kept oldest → newest.
    const std::vector<model::Message> *list = &c.messages;
    if (thread)
        list = st.replies(conv, thread);
    std::vector<std::string> lines, authors; // authors newest first: who talks now matters most
    if (list)
        for (auto it = list->rbegin(); it != list->rend() && int(lines.size()) < maxMessages;
             ++it) {
            const model::Message &m = *it;
            if (m.pending || !isConversational(m))
                continue;
            std::string text(str::trim(screens::plainText(st, m.text)));
            if (text.empty())
                continue;
            text = utf8::ellipsize(text, kMaxMessageChars);
            const std::string who(screens::authorName(st, m));
            lines.push_back(who.empty() ? text : who + ": " + text);
            addUnique(authors, who);
        }
    std::reverse(lines.begin(), lines.end()); // collected newest first
    out.recentMessages = std::move(lines);

    // The conversation's name and members.
    std::vector<std::string> members = authors;
    switch (c.kind) {
    case model::ConvKind::Dm:
        out.conversationName = label(c.dmUser);
        if (out.conversationName.empty())
            out.conversationName = c.name;
        addUnique(members, out.conversationName, kMaxMembers);
        break;
    case model::ConvKind::Group: {
        std::string names;
        for (model::UserRef u : c.members) {
            if (u == st.me)
                continue;
            const std::string n = label(u);
            if (!n.empty())
                names += (names.empty() ? "" : ", ") + n;
            addUnique(members, n, kMaxMembers);
        }
        out.conversationName = names.empty() ? c.name : names;
        break;
    }
    default:
        out.conversationName = c.name.empty() ? std::string() : "#" + c.name;
        // Only a list some earlier view already loaded; no members fetch.
        for (model::UserRef u : c.members)
            addUnique(members, label(u), kMaxMembers);
        break;
    }
    out.memberNames = std::move(members);
    return out;
}

} // namespace shell
