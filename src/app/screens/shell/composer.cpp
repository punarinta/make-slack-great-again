#include "screens/shell/composer.h"

#include "app/llm/service.h"
#include "app/llm/voice_input.h"
#include "app/model/jobs.h"
#include "app/mrkdwn/emoji.h"
#include "app/spell/spell.h"
#include "app/mrkdwn/mrkdwn.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/mime.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "screens/common/file_dialogs.h"
#include "screens/shell/composer_popups.h"
#include "screens/shell/nav_chrome.h"
#include "screens/shell/shell_text.h"
#include "screens/shell/voice_strip.h"
#include "app/mrkdwn/markdown.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/emoji_picker.h"
#include "app/screens/messages/image_cache.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace ui;
using gfx::Icon;
using i18n::tr;

namespace shell {

// ── mrkdwn serialiser ───────────────────────────────────────────────────────

namespace {

constexpr uint8_t kMarkBits[] = {
    TextEdit::Bold, TextEdit::Italic, TextEdit::Strike, TextEdit::Code
};
constexpr char kMarkChars[] = {'*', '_', '~', '`'};

bool isBlank(char c) {
    return c == ' ' || c == '\t';
}

bool g_markdown = false; // markdownFormatting()

} // namespace

bool markdownFormatting() {
    return g_markdown;
}

void setMarkdownFormatting(bool on) {
    g_markdown = on;
}

std::string editorMrkdwn(const TextEdit &edit, bool markdown) {
    const std::vector<TextEdit::Run> runs = edit.runs();
    if (!markdown)
        return toMrkdwn(edit.text(), runs);
    // *x* → _x_ in what is typed; a link's label (a pill's name too) is no
    // markup and keeps its bytes. Same length, so the runs still fit.
    std::string text = mrkdwn::markdownItalics(edit.text());
    for (const TextEdit::Run &r : runs)
        if (!r.link.empty())
            for (uint32_t i = r.start; i < r.end && i < text.size(); ++i)
                text[i] = edit.text()[i];
    return toMrkdwn(text, runs);
}

std::string toMrkdwn(std::string_view text, const std::vector<TextEdit::Run> &runs) {
    // Per byte: mark bits and a link index (0 = none), then normalise so each
    // mark hugs its words and never spans a line break.
    const size_t                  n = text.size();
    std::vector<uint8_t>          marks(n, 0);
    std::vector<uint16_t>         link(n, 0);
    std::vector<std::string_view> urls;
    for (const auto &r : runs) {
        uint16_t li = 0;
        if (!r.link.empty()) {
            auto it = std::find(urls.begin(), urls.end(), r.link);
            if (it == urls.end()) {
                urls.push_back(r.link);
                it = urls.end() - 1;
            }
            li = uint16_t(it - urls.begin() + 1);
        }
        for (uint32_t i = r.start; i < r.end && i < n; ++i) {
            marks[i] =
                r.format & (TextEdit::Bold | TextEdit::Italic | TextEdit::Strike | TextEdit::Code);
            link[i] = li;
        }
    }
    for (size_t i = 0; i < n; ++i)
        if (text[i] == '\n')
            marks[i] = 0, link[i] = 0;
    for (uint8_t bit : kMarkBits) {
        size_t i = 0;
        while (i < n) {
            if (!(marks[i] & bit)) {
                ++i;
                continue;
            }
            size_t j = i;
            while (j < n && (marks[j] & bit))
                ++j;
            // Code keeps its spaces (they are content); the others drop the
            // mark from edge blanks so "*bold* " never becomes "*bold *".
            if (bit != TextEdit::Code) {
                size_t a = i, b = j;
                while (a < b && isBlank(text[a]))
                    marks[a++] &= uint8_t(~bit);
                while (b > a && isBlank(text[b - 1]))
                    marks[--b] &= uint8_t(~bit);
            }
            i = j;
        }
    }

    std::string out;
    out.reserve(n + n / 4);
    uint8_t stack[4];
    int     depth = 0;
    auto    open  = [&](uint8_t want) {
        // Close from the top down to the first open mark we no longer want…
        int keep = 0;
        while (keep < depth && (want & stack[keep]))
            ++keep;
        while (depth > keep) {
            const uint8_t bit = stack[--depth];
            for (int k = 0; k < 4; ++k)
                if (kMarkBits[k] == bit)
                    out += kMarkChars[k];
        }
        // …then open the missing ones in canonical order (code innermost).
        for (int k = 0; k < 4; ++k) {
            const uint8_t bit = kMarkBits[k];
            bool          on  = false;
            for (int d = 0; d < depth; ++d)
                on |= stack[d] == bit;
            if ((want & bit) && !on) {
                stack[depth++] = bit;
                out += kMarkChars[k];
            }
        }
    };

    size_t i = 0;
    while (i < n) {
        if (link[i]) {
            // A link is one token: Slack parses no marks inside a label, so
            // the marks of its first character wrap the whole <url|label>.
            size_t j = i;
            while (j < n && link[j] == link[i])
                ++j;
            open(marks[i] & uint8_t(~TextEdit::Code));
            const std::string_view url = urls[link[i] - 1];
            const std::string_view lbl = text.substr(i, j - i);
            if (url.size() > 1 && url[0] == '<') { // a pill: its raw token
                out += url;
                i = j;
                continue;
            }
            if (url.size() > 1 && (url[0] == '@' || url[0] == '#' || url[0] == '!')) {
                out += '<';
                out += url;
                out += '>';
                i = j;
                continue;
            }
            out += '<';
            out += url;
            if (lbl != url) {
                out += '|';
                // A '|' would end the label early (Slack has no escape for
                // it): show a broken bar instead; a '>' would end the token.
                for (char c : lbl)
                    if (c == '|')
                        out += "\xC2\xA6";
                    else if (c == '>')
                        out += "&gt;";
                    else
                        out += c;
            }
            out += '>';
            i = j;
            continue;
        }
        size_t j = i;
        while (j < n && marks[j] == marks[i] && !link[j])
            ++j;
        open(marks[i]);
        // & < > go out bare, as typed: Slack escapes them
        // itself, so an entity sent here would arrive as "&amp;gt;".
        out += text.substr(i, j - i);
        i = j;
    }
    open(0);
    return out;
}

// ── Drafts ──────────────────────────────────────────────────────────────────

void DraftStash::setStoreOf(std::function<model::Store *(const std::string &)> fn) {
    _storeOf = std::move(fn);
}

model::Store *DraftStash::storeOf(const std::string &scope) const {
    return _storeOf ? _storeOf(scope) : nullptr;
}

void DraftStash::stash(Key k, std::string html, std::vector<std::string> files) {
    if (k.conv == model::kNoConv)
        return;
    if (model::Store *s = storeOf(_scope))
        s->setDraft(k.conv, k.thread, std::move(html), std::move(files));
}

std::vector<EmojiCompletion> emojiCompletions(const model::Store &store, std::string_view q) {
    static const char *const kCommon[] = {"thumbsup",    "thumbsdown",  "clap",          "heart",
                                          "fire",        "rocket",      "eyes",          "smile",
                                          "laughing",    "wink",        "grin",          "joy",
                                          "sweat_smile", "sob",         "thinking_face", "wave",
                                          "ok_hand",     "point_right", "muscle",        "100"};
    auto                     rankOf    = [q](std::string_view n) {
        if (n.substr(0, q.size()) == q)
            return 0;
        for (size_t at = n.find(q); at != std::string_view::npos; at = n.find(q, at + 1))
            if (at > 0 && (n[at - 1] == '_' || n[at - 1] == '-' || n[at - 1] == '+'))
                return 1;
        return n.find(q) != std::string_view::npos ? 2 : -1;
    };
    // Per rank, in the order considered: common, the table, custom (sorted).
    // Eight prefix matches fill the list: nothing later can come before them.
    // A rank keeps its first eight only (no more of it can show), and a name
    // seen again is in its own rank's list already (the rank is the name's).
    constexpr size_t             kMax = 8;
    std::vector<EmojiCompletion> tiers[3];
    auto                         consider = [&](std::string_view n, bool custom) {
        const int r = rankOf(n);
        if (r >= 0 && tiers[r].size() < kMax &&
            std::none_of(tiers[r].begin(), tiers[r].end(), [n](const EmojiCompletion &e) {
                return e.name == n;
            }))
            tiers[r].push_back({std::string(n), custom});
        return tiers[0].size() < kMax; // false: the list is settled
    };
    for (const char *n : kCommon)
        if (!emoji::toUnicode(n).empty() && !consider(n, false))
            break;
    if (tiers[0].size() < kMax)
        emoji::forEachName([&](std::string_view n) { return consider(n, false); });
    for (std::string_view n : store.customEmojiNames())
        if (tiers[0].size() >= kMax || !consider(n, true))
            break;
    std::vector<EmojiCompletion> out;
    for (auto &t : tiers)
        for (EmojiCompletion &e : t) {
            if (out.size() >= kMax)
                return out;
            out.push_back(std::move(e));
        }
    return out;
}

std::string DraftStash::get(Key k) const {
    const model::Store        *s = storeOf(_scope);
    const model::Store::Draft *d = s ? s->draft(k.conv, k.thread) : nullptr;
    return d ? d->html : std::string();
}

std::vector<std::string> DraftStash::files(Key k) const {
    const model::Store        *s = storeOf(_scope);
    const model::Store::Draft *d = s ? s->draft(k.conv, k.thread) : nullptr;
    return d ? d->files : std::vector<std::string>();
}

bool DraftStash::has(Key k) const {
    const model::Store *s = storeOf(_scope);
    return s && s->draft(k.conv, k.thread) != nullptr;
}

void DraftStash::erase(Key k) {
    if (model::Store *s = storeOf(_scope))
        s->setDraft(k.conv, k.thread, {}, {});
}

void DraftStash::appendText(const std::string &scope, Key k, std::string_view add) {
    model::Store              *s   = storeOf(scope);
    const model::Store::Draft *old = s ? s->draft(k.conv, k.thread) : nullptr;
    if (!s || k.conv == model::kNoConv || add.empty())
        return; // its workspace is closed, or nothing to add
    std::vector<std::string> files = old ? old->files : std::vector<std::string>();
    std::string              text;
    std::vector<uint16_t>    fmt;
    std::vector<std::string> links;
    rich::fromHtml(old ? old->html : std::string(), &text, &fmt, &links);
    if (size_t last = utf8::prevBoundary(text, text.size());
        !text.empty() && !utf8::isSpace(utf8::decode(text, last)))
        text += ' ';
    text += add;
    fmt.resize(text.size(), 0); // the dictation is plain text
    s->setDraft(k.conv, k.thread, rich::toHtml(text, fmt.data(), links), std::move(files));
}

// ── Undo send ───────────────────────────────────────────────────────────────
// The undo-send pill: "Message sent · Undo
// Ctrl+Z" on the tooltip chip, right-aligned 6 px above the composer box, for
// 5 s. A click (or Ctrl+Z with an empty editor) takes the message back.

class UndoPill final : public Popup {
public:
    explicit UndoPill(std::function<void()> undo) : _undo(std::move(undo)) {
        setModal(false);
        setCard(false);
        setPaintOutset(8);
        setCursor(plat::Cursor::Hand);
    }
    SizeF measureContent(float, float) override {
        build();
        return {std::ceil(_w), std::ceil(_h)};
    }
    void styleChanged() override { _a.reset(); }
    void paint(gfx::Painter &p) override {
        build();
        const RectF b = bounds();
        p.dropShadow(b, 8, 6, {0, 1}, 0x30000000U);
        p.fillRoundRect(b, 8, ui::color(C::TooltipBg));
        float x = 12;
        for (const text::Layout *l : {_a.get(), _dot.get(), _u.get(), _k.get()}) {
            l->paint(p, snapPx({x, std::floor((b.h - l->height()) / 2)}));
            x += l->width() + (l == _a.get() || l == _dot.get() ? 8 : 0);
        }
    }
    bool onEvent(Event &e) override {
        switch (e.type) {
        case EventType::PointerEnter:
        case EventType::PointerLeave:
            _a.reset(); // "Undo" underlines while hovered
            update();
            return false;
        case EventType::PointerDown:
            return e.button == plat::Button::Left;
        case EventType::PointerUp:
            if (bounds().contains(e.pos) && _undo) {
                auto cb = _undo;
                cb();
            }
            return true;
        default:
            return false;
        }
    }

private:
    void build() {
        // Measured before the pill is attached to a window, the first build
        // runs at scale 1; rebuild once the real display scale is known, or
        // the text paints at a fraction of its size until a hover rebuilds it.
        const float k = windowScale();
        if (_a && _scale == k)
            return;
        _scale    = k;
        auto line = [k](std::string_view s, text::Style st) {
            text::AttributedText t;
            t.append(s, st);
            return text::Layout::build(std::move(t), {}, k);
        };
        const text::Style base = ui::pxFont(12, text::Weight::Regular, ui::color(C::TooltipText));
        text::Style       dim = base, bold = base;
        dim.color              = ui::color(C::OnDarkDim);
        bold.weight            = text::Weight::Bold;
        bold.underline         = hovered();
        // The native key hint for Shortcut::UndoSend ("Ctrl+Z", "⌘Z").
        const std::string hint = " " + shortcuts::nativeKeys(shortcuts::Id::UndoSend);
        _a                     = line(tr("Message sent"), base);
        _dot                   = line("\xC2\xB7", dim);
        _u                     = line(tr("Undo"), bold);
        _k                     = line(hint, dim);
        _w = 12 + _a->width() + 8 + _dot->width() + 8 + _u->width() + _k->width() + 12;
        _h = 7 + std::max(_a->height(), _u->height()) + 7;
    }
    std::function<void()>         _undo;
    std::unique_ptr<text::Layout> _a, _dot, _u, _k;
    float                         _w = 0, _h = 0, _scale = 0;
};

// ── Composer parts ──────────────────────────────────────────────────────────

namespace {

// A toolbar / bottom-bar button: 26 px, the icon in
// composer.toolbarIcon (…Active while the editor has focus), a
// surface.highlightStrong wash on hover, radius 3.
GlyphButton *toolButton(View *parent, Icon icon, float iconPx, std::string tip) {
    auto *b = parent->add<GlyphButton>(icon, 26, iconPx, C::ComposerIcon, std::move(tip));
    b->setLook({C::None, C::FormHighlightStrong, C::FormHighlightStrong, C::None, 3});
    b->setFocusable(false);
    return b;
}

View *toolSeparator(View *parent) {
    auto *s = parent->add<View>();
    s->style().size(1, 16).noShrink();
    s->setBackground(C::FieldBorder);
    return s;
}

// The send button and the schedule chevron: a pill that fills with the
// accent once there is something to send.
class SendPart final : public GlyphButton {
public:
    SendPart(Icon icon, float w, float iconPx, std::string tip, bool chevron)
        : GlyphButton(icon, 28, iconPx, C::DropArrow, std::move(tip)), _chevron(chevron) {
        style().size(w, 28);
        setFocusable(false);
        setActive(false);
    }
    void setActive(bool on) {
        _on = on;
        setTint(on ? C::AccentText : C::DropArrow);
        setLook(
            {C::None, on ? C::None : C::FormHighlight, on ? C::None : C::FormHighlight, C::None, 4}
        );
    }
    void paint(gfx::Painter &p) override {
        if (_on && (hovered() || pressed())) // rgba(255,255,255,40) / rgba(0,0,0,40)
            p.fillRect(bounds(), pressed() ? 0x28000000U : 0x28ffffffU);
        if (_on && _chevron) // the seam: border-left rgba(0,0,0,60)
            p.fillRect({0, 0, 1, height()}, 0x3c000000U);
        GlyphButton::paint(p);
    }

private:
    bool _on = false, _chevron;
};

// The send + schedule pair's shared pill.
class SendGroup final : public View {
public:
    void paint(gfx::Painter &p) override {
        if (on)
            p.fillRoundRect(bounds(), 4, color(C::Accent));
    }
    bool on = false;
};

constexpr float kChipW = 160, kChipH = 92, kChipGap = 8;

// A picture the chip shows as one (SVG is shown as a file).
bool looksLikeImage(std::string_view path) {
    const std::string_view m = mime::fromName(path);
    return str::startsWith(m, "image/") && m != "image/svg+xml";
}

bool looksLikeText(std::string_view path) {
    return mime::isTextName(path);
}

// A name longer than 18 characters: its first 15, "…", its extension.
std::string chipName(std::string_view path) {
    const std::string name(file::baseName(path));
    if (utf8::prefixBytes(name, 18) == name.size())
        return name;
    return str::concat(
        {name.substr(0, utf8::prefixBytes(name, 15)), "\xE2\x80\xA6", file::extension(path)}
    );
}

// The name and size, bottom-left on translucent plates.
View *plate(View *parent, std::string text, Font f) {
    auto *pl = parent->add<View>();
    pl->style().padding(4, 1).alignSelf(Align::Start);
    pl->setBackground(C::OverlayBg, 4);
    pl->add<Label>(std::move(text), f, C::OverlayText)->setMaxLines(1);
    return pl;
}

// A text file's first lines as the chip's background (10 px, tertiary).
std::string textPreview(const std::string &path) {
    std::string all; // the head only: the file may be large
    if (!file::readRange(path, 0, 2048, &all))
        return {};
    std::string out;
    int         lines = 0;
    for (char c : all) {
        if (c == '\r')
            continue;
        if (c == '\n' && ++lines >= 8)
            break;
        if (c == '\t')
            out += "    ";
        else
            out += c;
    }
    return out;
}

} // namespace

// ── Composer ────────────────────────────────────────────────────────────────

Composer::Composer(screens::Context &ctx, DraftStash &drafts) : _ctx(ctx), _drafts(drafts) {
    style().padding(12, 8, 12, 8);
    watchAncestorHide();
    auto *box = add<View>();
    box->setBackground(C::FormBg, 8);
    box->setBorder(C::FieldBorder);
    box->setClipChildren(true);
    box->style().padding(1); // the contents sit inside the 1-px border
    _box = box;

    // ── Formatting toolbar ──────────────────────────────────────────────────
    auto *toolbar = box->add<View>();
    toolbar->style().row().height(32).padding(8, 2, 8, 2).spacing(4).items(Align::Center);
    using shortcuts::Id;
    struct ToolDef {
        Icon        icon;
        const char *label;
        Id          id;
    };
    static constexpr ToolDef kTools[] = {
        {Icon::Bold, N_("Bold"), Id::Bold},
        {Icon::Italic, N_("Italic"), Id::Italic},
        {Icon::Underline, N_("Underline"), Id::Underline},
        {Icon::Strikethrough, N_("Strikethrough"), Id::Strikethrough},
        {Icon::Link, N_("Link"), Id::Link},
        {Icon::ListOrdered, N_("Ordered list"), Id::OrderedList},
        {Icon::List, N_("Bullet list"), Id::BulletList},
        {Icon::Quote, N_("Blockquote"), Id::Quote},
        {Icon::Code, N_("Inline code"), Id::InlineCode},
        {Icon::Braces, N_("Code block"), Id::CodeBlock},
    };
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7)
            _seps[i == 4 ? 0 : 1] = toolSeparator(toolbar);
        const ToolDef &d   = kTools[i];
        _tools[i]          = toolButton(toolbar, d.icon, 18, shortcuts::tip(tr(d.label), d.id));
        _tools[i]->onClick = [this, id = d.id] { formatAction(id); };
    }

    // ── "Editing message" banner ────────────────────────────────────────────
    // Unstyled: only its label and cross show, no fill, no accent bar.
    _editBar = box->add<View>();
    _editBar->style().row().height(30).padding(8, 0, 4, 0).spacing(4).items(Align::Center);
    _editBar->add<Label>(tr("Editing message"), Font::SmallSemibold, C::BannerText)
        ->style()
        .flex(1);
    auto *cancel = _editBar->add<GlyphButton>(Icon::X, 20, 12, C::BannerText, tr("Cancel"));
    cancel->setLook({C::None, C::BannerAccent, C::BannerAccent, C::None, 3});
    cancel->setFocusable(false);
    cancel->onClick = [this] { endEdit(); };
    _editBar->setVisible(false);

    // ── Attachments ─────────────────────────────────────────────────────────
    _chips = box->add<View>();
    _chips->style().height(kChipH + 14).noShrink();
    _chips->setClipChildren(true);
    _chipRow = _chips->add<View>();
    _chipRow->style().row().padding(8, 8, 8, 6).spacing(kChipGap);
    _chips->setVisible(false);

    // ── The editor ──────────────────────────────────────────────────────────
    _edit = box->add<TextEdit>();
    _edit->setMaxLines(18);
    _edit->style().padding(14, 10);
    _edit->setPlainPaste(true);                // pasted rich text arrives as plain
    _edit->setLinkBackground(C::AccentSubtle); // mention / channel / GIF pills
    _edit->onChange = [this] {
        typing();
        refreshLook();
        updatePickList();
        scheduleSpell();
    };
    _edit->onContextMenu     = [this](uint32_t offset, PointF at) { return spellMenu(offset, at); };
    _spellObserver           = spell::Checker::instance().observe([this] { respell(); });
    _edit->onSelectionChange = [this] { updatePickList(); };
    _edit->onPasteMedia      = [this](const std::vector<std::string> &m, plat::Selection sel) {
        return pasteMedia(m, sel);
    };
    _edit->onFocusChange = [this](bool) { refreshLook(); };
    // Enter is the table's (SendMessage/NewLine), not TextEdit's onSubmit.
    _edit->onKey         = [this](const Event &e) {
        // Push-to-talk: letting go of the voice shortcut's key after holding
        // it stops the recording; a quick tap leaves it running (toggle).
        // Auto-repeat releases are the key still being held.
        if (e.type == EventType::KeyUp && _pttArmed) {
            shortcuts::Keys keys[2];
            shortcuts::bindings(shortcuts::Id::VoiceInput, keys);
            if (e.key != keys[0].key)
                return false;
            if (e.repeat)
                return true;
            _pttArmed = false;
            if (base::monotonicMs() - _pttPressedMs >= kPushToTalkMs && voiceActiveHere() &&
                _ctx.ai->voice().stateOf(voiceJobHere()) == llm::VoiceInput::State::Recording)
                _ctx.ai->voice().stop();
            return true;
        }
        return e.type == EventType::KeyDown && keyDown(e);
    };

    // ── Voice input status (hidden until a dictation starts here) ───────────
    _voiceStrip           = box->add<VoiceStrip>(_ctx.app.platform());
    _voiceStrip->onCancel = [this] {
        if (_voiceStrip->mode() == VoiceStrip::Mode::Error)
            _voiceStrip->setMode(VoiceStrip::Mode::Hidden);
        else
            cancelVoiceHere();
    };

    // ── Bottom bar ──────────────────────────────────────────────────────────
    auto *bottom = box->add<View>();
    bottom->style().row().height(36).padding(8, 2, 4, 2).spacing(4).items(Align::Center);
    auto *attach =
        toolButton(bottom, Icon::Paperclip, 19, shortcuts::tip(tr("Attach file"), Id::AttachFile));
    attach->onClick = [this] { chooseAttachments(); };
    _emojiBtn = toolButton(bottom, Icon::Smile, 18, shortcuts::tip(tr("Emoji"), Id::EmojiPicker));
    _emojiBtn->onClick = [this] { openEmoji(); };
    _gifBtn            = toolButton(bottom, Icon::Gif, 18, tr("Search GIFs"));
    _gifBtn->onClick   = [this] { openGif(); };
    _mentionBtn        = toolButton(bottom, Icon::AtSign, 18, str::concat({tr("Mention"), " (@)"}));
    _mentionBtn->onClick = [this] {
        _edit->insertText("@");
        _edit->focus();
        updatePickList();
    };
    _bottom[0] = attach;
    _bottom[1] = _emojiBtn;
    _bottom[2] = _gifBtn;
    _bottom[3] = _mentionBtn;
    bottom->add<View>()->style().flex(1);
    // Voice input, beside the send group: shown while a provider can do
    // speech-to-text (updateMicVisibility); its icon, tooltip and look follow
    // the recording (updateVoiceUi).
    _micBtn = toolButton(bottom, Icon::Mic, 18, shortcuts::tip(tr("Voice input"), Id::VoiceInput));
    _micBtn->setVisible(false);
    _micBtn->onClick = [this] { toggleVoiceInput(); };
    _sendGroup       = bottom->add<SendGroup>();
    _sendGroup->style().row().noShrink();
    _sendBtn          = _sendGroup->add<SendPart>(Icon::Send, 38, 18, "", false);
    _sendBtn->onClick = [this] { send(); };
    _dropBtn = _sendGroup->add<SendPart>(Icon::ChevronDown, 18, 12, tr("Schedule send"), true);
    _dropBtn->onClick = [this] { openSchedule(); };
    refreshTips();
    refreshLook();

    if (llm::Service *ai = _ctx.ai) {
        llm::VoiceInput::Listener l;
        l.stateChanged = [this](llm::VoiceInput::State) { updateVoiceUi(); };
        l.level        = [this](float peak) {
            if (voiceActiveHere())
                _voiceStrip->pushLevel(peak);
            else if (_voiceStrip->mode() == VoiceStrip::Mode::Recording)
                updateVoiceUi(); // another composer took the microphone
        };
        l.finished = [this](const void *owner, const std::string &text) {
            voiceEnded(owner, &text, nullptr);
        };
        l.failed = [this](const void *owner, const std::string &error) {
            voiceEnded(owner, nullptr, &error);
        };
        _voiceObserver     = ai->voice().observe(std::move(l));
        _providersObserver = ai->observeProviders([this] { updateMicVisibility(); });
    }
    updateMicVisibility();
    updateVoiceUi();
}

Composer::~Composer() {
    // Its dictations would otherwise end for a composer that no longer exists.
    cancelAllVoice();
    if (_ctx.ai) {
        _ctx.ai->voice().unobserve(_voiceObserver);
        _ctx.ai->unobserveProviders(_providersObserver);
    }
    stashNow();
    withdrawUndo();
    spell::Checker::instance().unobserve(_spellObserver);
    _ctx.app.cancelTimer(_spellTimer);
}

GlyphButton *Composer::button(shortcuts::Id id) const {
    using shortcuts::Id;
    static constexpr Id kOrder[] = {
        Id::Bold,
        Id::Italic,
        Id::Underline,
        Id::Strikethrough,
        Id::Link,
        Id::OrderedList,
        Id::BulletList,
        Id::Quote,
        Id::InlineCode,
        Id::CodeBlock
    };
    for (int i = 0; i < 10; ++i)
        if (kOrder[i] == id)
            return _tools[i];
    if (id == Id::AttachFile)
        return _bottom[0];
    if (id == Id::EmojiPicker)
        return _bottom[1];
    if (id == Id::VoiceInput)
        return _micBtn;
    if (id == Id::SendMessage)
        return _sendBtn;
    return nullptr;
}

void Composer::refreshTips() {
    // The send key follows the Ctrl+Enter option; the GIF tip whether a
    // GIPHY key is set.
    _sendBtn->setTooltip(shortcuts::tip(tr("Send message"), shortcuts::Id::SendMessage));
    // Configured: a GIPHY key, or the service's own search (the demo's).
    const bool gif = _ctx.backend.gifSearchAvailable() || (gifKey && !gifKey().empty());
    _gifBtn->setTooltip(
        gif ? tr("Search GIFs") : tr("Search GIFs \xE2\x80\x94 needs a GIPHY API key")
    );
}

void Composer::setScheduleVisible(bool on) {
    _dropBtn->setVisible(on);
}

bool Composer::scheduleVisible() const {
    return _dropBtn->visible();
}

bool Composer::editBannerShown() const {
    return _editBar->visible();
}

// Focus tints the icons and the box border; text or files light Send.
void Composer::refreshLook() {
    const bool focused = _edit->focused() || _dropHover;
    if (focused != _focusedLook) {
        _focusedLook = focused;
        const C tint = focused ? C::ComposerIconActive : C::ComposerIcon;
        for (GlyphButton *b : _tools)
            b->setTint(tint);
        for (GlyphButton *b : _bottom)
            b->setTint(tint);
        // A recording mic button wears the stop icon (updateVoiceUi).
        if (_voiceStrip->mode() != VoiceStrip::Mode::Recording)
            _micBtn->setTint(tint);
        _box->setBorder(focused ? C::FieldBorderFocus : C::FieldBorder);
    }
    const bool active = !str::trim(_edit->text()).empty() || !_files.empty();
    if (active != _active) {
        _active = active;
        static_cast<SendPart *>(_sendBtn)->setActive(active);
        static_cast<SendPart *>(_dropBtn)->setActive(active);
        static_cast<SendGroup *>(_sendGroup)->on = active;
        _sendGroup->update();
    }
    // The placeholder: the suggestion (with its key hint) while there is one.
    std::string ph =
        _suggestion.empty() ? _placeholder : str::concat({_suggestion, "  \xE2\x86\x92"});
    if (ph != _shownPlaceholder) {
        _shownPlaceholder = ph;
        _edit->setPlaceholder(std::move(ph));
    }
}

// The composer's key handler, in this order. A feature that is off here
// (voice input without a provider, prompt history without a source) lets
// the key go on to the editor.
bool Composer::keyDown(const Event &e) {
    using shortcuts::Id;
    using shortcuts::matches;
    if (!_edit->preedit().empty())
        return false; // the IME owns the keyboard while composing (TextEdit's rule)
    const uint32_t mods = e.mods & ui::kModMask;
    // Voice input first: while a dictation of ours is in flight, Escape
    // cancels it ahead of every other Escape meaning (popups, edit mode).
    if (e.key == plat::Key::Escape && !mods && voiceActiveHere()) {
        cancelVoiceHere();
        return true;
    }
    if (matches(Id::VoiceInput, e) && _micBtn->visible()) {
        if (!e.repeat) {
            const bool wasActive = voiceActiveHere();
            toggleVoiceInput();
            // Push-to-talk only for a press that started a recording.
            _pttArmed = !wasActive && voiceActiveHere();
            if (_pttArmed)
                _pttPressedMs = base::monotonicMs();
        }
        return true;
    }
    // The pick list takes its navigation keys first.
    if (_pick && _pick->handleKey(e))
        return true;
    // A suggested reply is taken with → or Tab from the empty editor.
    if ((e.key == plat::Key::Right || e.key == plat::Key::Tab) && !mods && acceptSuggestion())
        return true;
    // Pills are atomic: deleting into one removes the whole of it (a partly
    // edited pill would lose its token).
    if ((e.key == plat::Key::Backspace || e.key == plat::Key::Delete) && !mods) {
        const uint32_t a  = std::min(_edit->anchor(), _edit->caret());
        const uint32_t b  = std::max(_edit->anchor(), _edit->caret());
        const uint32_t at = a == b ? (e.key == plat::Key::Backspace ? (a ? a - 1 : a) : a) : a;
        for (const TextEdit::Run &r : _edit->runs()) {
            if (r.link.empty() || r.link[0] != '<')
                continue;
            const bool hit = a == b ? (at >= r.start && at < r.end) : (r.start < b && r.end > a);
            if (!hit)
                continue;
            const uint32_t from = std::min(a, r.start), to = std::max(b, r.end);
            _edit->setSelection(from, to);
            _edit->insertText("");
            return true;
        }
    }
    if (matches(Id::SendMessage, e)) {
        send();
        return true;
    }
    if (matches(Id::UndoSend, e) && _edit->empty() && undoSend())
        return true;
    if (matches(Id::SearchPromptHistory, e) && openPromptSearch())
        return true;
    if (!mods && (e.key == plat::Key::Up || e.key == plat::Key::Down) &&
        stepPromptHistory(e.key == plat::Key::Up))
        return true;
    if (matches(Id::EditLastMessage, e) && _edit->empty()) {
        editLast();
        return true;
    }
    if (e.key == plat::Key::Escape && _editTs) { // CancelOrExitEdit
        endEdit();
        return true;
    }
    static constexpr Id kFormatKeys[] = {
        Id::Bold,
        Id::Italic,
        Id::Underline,
        Id::Strikethrough,
        Id::InlineCode,
        Id::CodeBlock,
        Id::OrderedList,
        Id::BulletList,
        Id::Quote,
        Id::AttachFile,
        Id::Link,
        Id::EmojiPicker,
    };
    for (Id id : kFormatKeys)
        if (matches(id, e)) {
            formatAction(id);
            return true;
        }
    return false;
}

// The toolbar: markers typed around the selection (wrapInline), line
// prefixes (prefixLines), fences (wrapCodeBlock).
void Composer::formatAction(shortcuts::Id id) {
    using shortcuts::Id;
    switch (id) {
    case Id::Bold:
        return wrapInline(markdownFormatting() ? "**" : "*");
    case Id::Italic:
        return wrapInline(markdownFormatting() ? "*" : "_");
    case Id::Underline:
        return wrapInline("__");
    case Id::Strikethrough:
        return wrapInline("~");
    case Id::InlineCode:
        return wrapInline("`");
    case Id::CodeBlock:
        return wrapCodeBlock();
    case Id::OrderedList:
        return prefixLines("", true);
    case Id::BulletList:
        return prefixLines("- ", false);
    case Id::Quote:
        return prefixLines("> ", false);
    case Id::AttachFile:
        return chooseAttachments();
    case Id::Link:
        return openLinkPopup();
    case Id::EmojiPicker:
        return openEmoji();
    default:
        return;
    }
}

// Up in an empty editor: edit my newest message here (the newest
// that is mine and not a system line). Agent sessions have no edit.
void Composer::editLast() {
    if (_key.conv == model::kNoConv || _ctx.backend.isAgentSession(_key.conv))
        return;
    const model::Store &st   = _ctx.store;
    const auto          mine = [&](const model::Message &m) {
        return m.user == st.me && st.me != model::kNoUser && m.subtype().empty() && !m.pending;
    };
    if (_key.thread) {
        if (const auto *r = st.replies(_key.conv, _key.thread))
            for (auto it = r->rbegin(); it != r->rend(); ++it)
                if (mine(*it))
                    return beginEdit(it->ts);
        const model::Message *root = st.findMessage(_key.conv, _key.thread);
        if (root && mine(*root))
            beginEdit(root->ts);
        return;
    }
    const auto &msgs = st.conversation(_key.conv).messages;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        if (mine(*it))
            return beginEdit(it->ts);
}

// ── Voice input ─────────────────────────────────────────────────────────────

void Composer::setVoiceInput(bool on) {
    _voiceOn = on;
    if (!on)
        cancelAllVoice();
    updateMicVisibility();
    updateVoiceUi();
}

Composer::VoiceJob *Composer::voiceJobHere() const {
    if (!_ctx.ai)
        return nullptr;
    for (const auto &j : _voiceJobs)
        if (j->key == _key && j->scope == _drafts.scope() &&
            _ctx.ai->voice().stateOf(j.get()) != llm::VoiceInput::State::Idle)
            return j.get();
    return nullptr;
}

bool Composer::voiceActiveHere() const {
    return voiceJobHere() != nullptr;
}

bool Composer::toggleVoiceInput() {
    if (!_voiceOn || !_ctx.ai)
        return false;
    llm::VoiceInput &vi = _ctx.ai->voice();
    if (VoiceJob *j = voiceJobHere()) {
        // Transcription / clean-up can only be cancelled (Esc, ×), not stopped.
        if (vi.stateOf(j) == llm::VoiceInput::State::Recording)
            vi.stop();
        return true;
    }
    // Dismissed here rather than left to time out: a new attempt is starting.
    if (_voiceStrip->mode() == VoiceStrip::Mode::Error)
        _voiceStrip->setMode(VoiceStrip::Mode::Hidden);
    // Each dictation is its own VoiceInput owner: one may still be finishing
    // for a conversation left a moment ago. (One another composer's recording
    // cancelled ended without a word: forgotten here.)
    std::erase_if(_voiceJobs, [&vi](const auto &p) {
        return vi.stateOf(p.get()) == llm::VoiceInput::State::Idle;
    });
    _voiceJobs.push_back(std::make_unique<VoiceJob>(VoiceJob{_drafts.scope(), _key}));
    vi.start(_voiceJobs.back().get(), buildVoiceContext(_ctx, _key.conv, _key.thread));
    // Keeps Escape (cancel) and the shortcut (stop) within reach.
    _edit->focus();
    return true;
}

void Composer::cancelVoiceHere() {
    _pttArmed = false;
    if (VoiceJob *j = voiceJobHere()) {
        _ctx.ai->voice().cancel(j);
        std::erase_if(_voiceJobs, [j](const auto &p) { return p.get() == j; });
    }
}

void Composer::cancelAllVoice() {
    _pttArmed = false;
    // Moved out first: cancel reports Idle, and the listeners look at the list.
    auto jobs = std::move(_voiceJobs);
    _voiceJobs.clear();
    if (_ctx.ai)
        for (const auto &j : jobs)
            _ctx.ai->voice().cancel(j.get());
}

// The user moved on: the dictation is theirs all the same. A recording is
// stopped and transcribed; the text then lands in its conversation's draft.
void Composer::finishVoiceInBackground() {
    _pttArmed = false;
    if (VoiceJob *j = voiceJobHere();
        j && _ctx.ai->voice().stateOf(j) == llm::VoiceInput::State::Recording)
        _ctx.ai->voice().stop();
}

void DraftStash::putBack(
    const std::string &scope, Key k, std::string_view html, std::vector<std::string> files
) {
    model::Store              *s   = storeOf(scope);
    const model::Store::Draft *old = s ? s->draft(k.conv, k.thread) : nullptr;
    if (!s || k.conv == model::kNoConv || (html.empty() && files.empty()))
        return;
    std::string merged = old ? old->html : std::string();
    if (merged.empty()) {
        merged = html;
    } else if (!html.empty()) {
        TextEdit edit;
        edit.insertHtml(html);
        edit.insertText("\n");
        edit.insertHtml(merged);
        merged = edit.html();
    }
    std::vector<std::string> all = old ? old->files : std::vector<std::string>();
    for (std::string &f : files)
        if (std::find(all.begin(), all.end(), f) == all.end())
            all.push_back(std::move(f));
    s->setDraft(k.conv, k.thread, std::move(merged), std::move(all));
}

void Composer::voiceEnded(const void *owner, const std::string *text, const std::string *error) {
    auto it = std::find_if(_voiceJobs.begin(), _voiceJobs.end(), [owner](const auto &p) {
        return p.get() == owner;
    });
    if (it == _voiceJobs.end())
        return; // another composer's
    const VoiceJob job = **it;
    _voiceJobs.erase(it);
    const bool here = job.key == _key && job.scope == _drafts.scope();
    if (text) {
        if (here)
            insertVoiceText(*text);
        else
            _drafts.appendText(job.scope, job.key, str::trim(*text));
    } else if (error) {
        if (here) {
            _pttArmed = false;
            _voiceStrip->setMode(VoiceStrip::Mode::Error, *error);
        } else {
            _voiceErrors.emplace_back(job, *error);
        }
    }
    updateVoiceUi();
}

void Composer::updateMicVisibility() {
    _micBtn->setVisible(_voiceOn && _ctx.ai && _ctx.ai->voice().available());
}

void Composer::updateVoiceUi() {
    using Mode       = VoiceStrip::Mode;
    using State      = llm::VoiceInput::State;
    Mode        mode = Mode::Hidden;
    const State st   = _ctx.ai ? _ctx.ai->voice().stateOf(voiceJobHere()) : State::Idle;
    switch (st) {
    case State::Recording:
        mode = Mode::Recording;
        break;
    case State::Transcribing:
        mode = Mode::Transcribing;
        break;
    case State::Cleaning:
        mode = Mode::Cleaning;
        break;
    case State::Idle:
        break;
    }
    // A failure shown in the strip stays until it times out, is dismissed or
    // a new recording starts (toggleVoiceInput clears it first), whatever
    // state changes arrive around it.
    if (_voiceStrip->mode() != Mode::Error && mode != _voiceStrip->mode())
        _voiceStrip->setMode(mode);
    if (st == State::Idle)
        _pttArmed = false;
    // The mic: a red round stop button while recording, otherwise one of the
    // bottom bar's icons (tinted with the rest by refreshLook).
    using shortcuts::Id;
    if (_voiceStrip->mode() == Mode::Recording) {
        _micBtn->setLook({C::DangerFill, C::DangerFillHover, C::DangerFillHover, C::None, 13});
        _micBtn->setIcon(Icon::MicStop);
        _micBtn->setTint(C::AccentText);
        _micBtn->setTooltip(shortcuts::tip(tr("Stop and transcribe"), Id::VoiceInput));
    } else {
        _micBtn->setLook({C::None, C::FormHighlightStrong, C::FormHighlightStrong, C::None, 3});
        _micBtn->setIcon(Icon::Mic);
        _micBtn->setTint(_focusedLook ? C::ComposerIconActive : C::ComposerIcon);
        _micBtn->setTooltip(shortcuts::tip(tr("Voice input"), Id::VoiceInput));
    }
}

void Composer::insertVoiceText(const std::string &text) {
    const std::string_view t = str::trim(text);
    if (t.empty())
        return;
    // Judged by the character left of what the text replaces: "hello" +
    // dictation reads "hello world", not "helloworld".
    const uint32_t     at   = std::min(_edit->anchor(), _edit->caret());
    const std::string &all  = _edit->text();
    bool               glue = false;
    if (at > 0 && at <= all.size()) {
        size_t prev = utf8::prevBoundary(all, at);
        glue        = !utf8::isSpace(utf8::decode(all, prev));
    }
    _edit->insertText(glue ? str::concat({" ", t}) : std::string(t));
    // Not into a composer that went out of sight while it was transcribing.
    bool shown = window() != nullptr;
    for (const View *v = this; v && shown; v = v->parent())
        shown = v->visible();
    if (shown)
        _edit->focus();
    refreshLook();
}

// ↑ from an empty editor shows the newest prompt,
// then older ones; ↓ goes back, past the newest to an empty editor again.
bool Composer::stepPromptHistory(bool older) {
    if (_editTs)
        return false;
    // Inside a multi-line prompt the arrows move between its lines first.
    if (!_edit->caretOnEdgeLine(older))
        return false;
    if (_historyIndex < 0) {
        // Only from an empty editor: a draft is never replaced by ↑.
        if (!older || !historySource || !_edit->empty())
            return false;
        _history = historySource();
        if (_history.empty())
            return false;
    }
    const int next = _historyIndex + (older ? 1 : -1);
    if (next >= int(_history.size()))
        return true; // the oldest is shown already
    if (next < 0) {
        resetPromptHistory();
        _edit->clear();
        return true;
    }
    _historyIndex = next;
    // Where the next press in the same direction goes on at once: going back
    // from the first line, forward from the last.
    setHistoryText(_history[size_t(next)], older);
    return true;
}

void Composer::resetPromptHistory() {
    _history.clear();
    _historyIndex = -1;
}

void Composer::setHistoryText(const std::string &text, bool caretAtStart) {
    loadMrkdwn(*_edit, _ctx.store, text); // the editor's mrkdwn
    if (caretAtStart)
        _edit->setSelection(0, 0);
    refreshLook();
}

// Ctrl+R: the prompt search over the composer box.
bool Composer::openPromptSearch() {
    Window *w = window();
    if (_editTs || !historySource || !w)
        return false;
    std::vector<std::string> history = historySource();
    if (history.empty())
        return false;
    if (_historySearch)
        _historySearch->dismiss();
    // What's typed so far is where the search starts; picking replaces it.
    const std::string typed = str::simplified(_edit->text());
    View             *area  = _popupArea ? _popupArea : parent();
    auto             *hs    = HistorySearch::open(
        *w, std::move(history), typed, _box->windowRect(), area ? area->windowRect() : RectF{}
    );
    _historySearch           = hs;
    std::weak_ptr<int> alive = _alive;
    hs->onPicked             = [this, alive](const std::string &text) {
        if (alive.expired())
            return;
        resetPromptHistory();
        setHistoryText(text, false);
        _edit->focus();
    };
    hs->onCancelled = [this, alive] {
        if (!alive.expired())
            _edit->focus();
    };
    hs->onClosed = [this, alive, hs] {
        if (!alive.expired() && _historySearch == hs)
            _historySearch = nullptr;
    };
    return true;
}

void Composer::setSuggestion(std::string text) {
    _suggestion = str::simplified(text); // whitespace runs as one space, trimmed
    refreshLook();
}

void Composer::setLockReason(std::string reason) {
    // Unlocked, "Message <name>" follows a rename of the session.
    if (reason == _lock && !_lock.empty())
        return;
    const bool changed = reason != _lock;
    _lock              = std::move(reason);
    if (changed)
        setEnabled(_lock.empty());
    if (!_lock.empty()) {
        _placeholder = _lock;
        refreshLook();
        return;
    }
    const std::string was = _placeholder;
    refreshPlaceholder();
    if (changed && _placeholder == was)
        refreshLook(); // refreshPlaceholder repaints only on a new text
}

void Composer::refreshPlaceholder() {
    const std::string was = _placeholder;
    if (_lock.empty() && _fixedPlaceholder.empty() && _key.conv != model::kNoConv && !_key.thread &&
        _key.conv < _ctx.store().conversationCount()) {
        _placeholder = i18n::arg(tr("Message %1"), convTitle(_ctx.store(), _key.conv));
    }
    if (_placeholder != was)
        refreshLook();
}

bool Composer::acceptSuggestion() {
    if (_suggestion.empty() || _editTs || !_edit->empty())
        return false;
    _edit->insertText(_suggestion);
    return true;
}

void Composer::stashNow() {
    // While editing, the draft is the one set aside, not the message.
    _drafts.stash(
        _key,
        _editTs          ? _editStash
        : _edit->empty() ? std::string()
                         : _edit->html(),
        _files
    );
}

void Composer::beginEdit(model::Ts ts) {
    const model::Message *m = _ctx.store().findMessage(_key.conv, ts);
    if (!m || m->user != _ctx.store().me || m->user == model::kNoUser)
        return;
    if (!_editTs)
        _editStash = _edit->empty() ? std::string() : _edit->html();
    _editTs = ts;
    loadMrkdwn(*_edit, _ctx.store, m->text);
    _editBar->setVisible(true);
    _edit->focus();
    compositionChanged();
}

void Composer::endEdit() {
    if (!_editTs)
        return;
    _editTs = 0;
    _edit->clear();
    if (!_editStash.empty())
        _edit->insertHtml(_editStash);
    _editStash.clear();
    _editBar->setVisible(false);
    compositionChanged();
}

void Composer::compositionChanged() {
    const int state = (_files.empty() ? 0 : 1) | (_editTs ? 2 : 0);
    if (state == _composition)
        return;
    _composition = state;
    if (onCompositionChanged)
        onCompositionChanged();
}

void Composer::setTarget(model::ConvRef conv, model::Ts thread) {
    const DraftStash::Key next{conv, thread};
    if (next == _key)
        return;
    withdrawUndo(); // a conversation switch withdraws the offer
    // A dictation still running finishes for the conversation being left.
    finishVoiceInBackground();
    if (_voiceStrip->mode() == VoiceStrip::Mode::Error)
        _voiceStrip->setMode(VoiceStrip::Mode::Hidden);
    endEdit();
    stashNow();
    if (_pick)
        _pick->close();
    resetPromptHistory();
    if (_historySearch)
        _historySearch->dismiss();
    _lock.clear();       // the shell re-applies the new conversation's (applyComposerAccess)
    _suggestion.clear(); // it answered the conversation being left
    _key = next;
    _edit->clear();
    if (const std::string html = _drafts.get(next); !html.empty())
        _edit->insertHtml(html);
    _files = _drafts.files(next);
    rebuildChips();
    // Back where a dictation is still finishing (the strip shows it), or one
    // failed after we left (the strip says why).
    for (auto it = _voiceErrors.begin(); it != _voiceErrors.end(); ++it)
        if (it->first.key == _key && it->first.scope == _drafts.scope()) {
            _voiceStrip->setMode(VoiceStrip::Mode::Error, it->second);
            _voiceErrors.erase(it);
            break;
        }
    updateVoiceUi();
    _placeholder = !_fixedPlaceholder.empty() ? _fixedPlaceholder
                   : thread                   ? std::string(tr("Reply in thread\xE2\x80\xA6"))
                                              : std::string();
    refreshPlaceholder(); // "Message #name"
    if (conv != model::kNoConv)
        setSuggestion(_ctx.backend.promptSuggestion(conv)); // simplified
    refreshLook();
    compositionChanged();
}

void Composer::setPlaceholder(std::string s) {
    _fixedPlaceholder = s;
    _placeholder      = std::move(s);
    refreshLook();
}

std::string Composer::mrkdwn() const {
    return editorMrkdwn(*_edit, markdownFormatting());
}

void Composer::redialect(bool toMarkdown) {
    if (!_edit->empty()) {
        const uint32_t end = uint32_t(_edit->text().size());
        _edit->setSelection(end, end); // html() is the selection's otherwise
        const std::string html  = _edit->html();
        const std::string other = redialectDraft(_ctx.store, html, toMarkdown);
        if (other != html) {
            _edit->clear();
            _edit->insertHtml(other);
            refreshLook();
        }
    }
    if (!_editStash.empty())
        _editStash = redialectDraft(_ctx.store, _editStash, toMarkdown);
}

bool Composer::send() {
    if (onSendRequest) // an embedded composer (the forward dialog's)
        return onSendRequest();
    // CommonMark habits become mrkdwn, and
    // a list also travels as a rich_text block.
    mrkdwn::Composed  composed = mrkdwn::compose(mrkdwn());
    const std::string text(str::trim(composed.mrkdwn));
    if (_editTs) { // saving an edit (an emptied message keeps its text)
        // Files attached while editing post on their own (chat.update takes
        // none).
        if (!_files.empty()) {
            _ctx.backend.sendWithFiles(_key.conv, {}, _key.thread, std::move(_files), nullptr);
            _files.clear();
            rebuildChips();
        }
        if (!text.empty() && !composed.blocks.empty())
            _ctx.backend.editBlocks(_key.conv, _editTs, text, std::move(composed.blocks));
        else if (!text.empty())
            _ctx.backend.edit(_key.conv, _editTs, text);
        endEdit();
        return true;
    }
    if ((text.empty() && _files.empty()) || _key.conv == model::kNoConv)
        return false;
    resetPromptHistory(); // the next ↑ starts from the newest again
    // "/command [args]" runs a command msga answers itself instead of
    // posting — only a known one, so a message that merely starts with "/"
    // still goes out; the rest are messages to the agent.
    if (_files.empty() && text.size() > 1 && text[0] == '/' && onCommand &&
        _ctx.backend.capabilities().slashCommands) {
        const size_t      sp   = text.find(' ');
        const std::string name = utf8::foldCase(
            text.substr(1, sp == std::string::npos ? std::string::npos : sp - 1)
        ); // folded once, not per command
        for (const auto &c : _ctx.backend.commands(_key.conv)) {
            if (!c.local || utf8::foldCase(c.name) != name)
                continue;
            const std::string args(
                sp == std::string::npos ? std::string_view() : str::trim(text.substr(sp + 1))
            );
            _edit->clear();
            _drafts.erase(_key);
            refreshLook();
            onCommand(name, args);
            return true;
        }
    }
    withdrawUndo(); // a new send supersedes the offer
    const std::string              html  = _edit->empty() ? std::string() : _edit->html();
    const std::vector<std::string> files = _files;
    const bool broadcast = _files.empty() && _key.thread && broadcastWanted && broadcastWanted();
    if (!_files.empty())
        _ctx.backend.sendWithFiles(_key.conv, text, _key.thread, std::move(_files), nullptr);
    else if (!composed.blocks.empty())
        _ctx.backend.sendBlocks(
            _key.conv, text, std::move(composed.blocks), _key.thread, broadcast, nullptr
        );
    else if (broadcast)
        _ctx.backend.sendBroadcast(_key.conv, text, _key.thread, nullptr);
    else
        _ctx.backend.send(_key.conv, text, _key.thread, nullptr);
    offerUndo(html, files); // the pending copy is in the Store: what an undo deletes
    _files.clear();
    rebuildChips();
    _edit->clear();
    _suggestion.clear();
    _drafts.erase(_key);
    refreshLook();
    compositionChanged();
    if (onSent)
        onSent();
    return true;
}

// ── Undo send ───────────────────────────────────────────────────────────────

void Composer::offerUndo(const std::string &html, const std::vector<std::string> &files) {
    // Only where messages can be deleted, never in agent sessions.
    if (_ctx.backend.isAgentSession(_key.conv) || !window())
        return;
    // The ghost: my newest pending message in this list.
    const auto *list = _key.thread ? _ctx.store().replies(_key.conv, _key.thread)
                                   : &_ctx.store().conversation(_key.conv).messages;
    _undoTs          = 0;
    if (list)
        for (auto it = list->rbegin(); it != list->rend() && !_undoTs; ++it)
            if (it->user == _ctx.store().me && it->pending)
                _undoTs = it->ts;
    if (!_undoTs)
        return;
    _undoHtml        = html;
    _undoFiles       = files;
    auto        pill = std::make_unique<UndoPill>([this] { undoSend(); });
    auto       *raw  = pill.get();
    const SizeF s    = raw->measure(kInf, kInf);
    const RectF box  = _box->windowRect();
    raw->setAnchor({box.x + box.w - s.w, box.y - 6 - s.h, 0, 0}, Popup::Place::Over);
    raw->onClosed = [this, raw] {
        if (_undoPill == raw)
            _undoPill = nullptr;
    };
    _undoPill = raw;
    window()->showPopup(std::move(pill));
    _ctx.app.cancelTimer(_undoTimer);
    _undoTimer = _ctx.app.addTimer(5000, false, [this] { // the offer lasts 5 s
        _undoTimer = 0;
        withdrawUndo();
    });
}

void Composer::visibilityChanged(bool on) {
    if (!on) {
        withdrawUndo(); // the composer being hidden withdraws the offer
        // A recording stops; its text lands when the transcription is done.
        finishVoiceInBackground();
    }
}

void Composer::withdrawUndo() {
    _ctx.app.cancelTimer(_undoTimer);
    _undoTimer = 0;
    _undoTs    = 0;
    if (Popup *p = _undoPill) {
        _undoPill = nullptr;
        p->close();
    }
}

bool Composer::undoSend() {
    const model::Ts ts = _undoTs;
    if (!ts)
        return false;
    const std::string              html  = std::move(_undoHtml);
    const std::vector<std::string> files = std::move(_undoFiles);
    withdrawUndo();
    _ctx.backend.remove(_key.conv, ts); // in flight or confirmed: the server copy goes
    endEdit();
    // The text comes back; whatever was typed since follows it on a new line.
    const std::string typed = _edit->empty() ? std::string() : _edit->html();
    _edit->clear();
    if (!html.empty())
        _edit->insertHtml(html);
    if (!typed.empty()) {
        _edit->insertText("\n");
        _edit->insertHtml(typed);
    }
    addAttachments(files);
    refreshLook();
    compositionChanged();
    _edit->focus();
    return true;
}

void Composer::giveBack(std::string_view mrkdwn, const std::vector<std::string> &files) {
    if (_editTs) {
        TextEdit edit;
        loadMrkdwn(edit, _ctx.store, mrkdwn);
        if (!_editStash.empty()) {
            edit.insertText("\n");
            edit.insertHtml(_editStash);
        }
        _editStash = edit.empty() ? std::string() : edit.html();
    } else {
        const std::string typed = _edit->empty() ? std::string() : _edit->html();
        loadMrkdwn(*_edit, _ctx.store, mrkdwn);
        if (!typed.empty()) {
            _edit->insertText("\n");
            _edit->insertHtml(typed);
        }
    }
    addAttachments(files);
    refreshLook();
}

// ── Attachments ─────────────────────────────────────────────────────────────

size_t Composer::addAttachments(const std::vector<std::string> &paths) {
    size_t added = 0;
    for (const std::string &p : paths) {
        if (p.empty() || !file::exists(p) || file::isDir(p) ||
            std::find(_files.begin(), _files.end(), p) != _files.end())
            continue;
        _files.push_back(p);
        ++added;
    }
    if (added) {
        rebuildChips();
        refreshLook();
        compositionChanged();
    }
    return added;
}

void Composer::attachFiles(const std::vector<std::string> &paths) {
    addAttachments(paths);
    _edit->focus();
}

void Composer::removeAttachment(size_t i) {
    if (i >= _files.size())
        return;
    _files.erase(_files.begin() + long(i));
    rebuildChips();
    refreshLook();
    compositionChanged();
    _edit->focus();
}

void Composer::clearAttachments() {
    if (_files.empty())
        return;
    _files.clear();
    rebuildChips();
    refreshLook();
    compositionChanged();
}

void Composer::chooseAttachments() {
    std::weak_ptr<int> alive = _alive;
    screens::pickFiles(
        _ctx,
        [this, alive](std::vector<std::string> paths) {
            if (alive.expired())
                return;
            if (!paths.empty() && setAttachDir)
                setAttachDir(std::string(file::dirName(paths.front())));
            addAttachments(paths);
            _edit->focus();
        },
        true,
        {},
        attachDir ? attachDir() : std::string()
    );
}

// Pasted files: files copied in a file manager arrive as local
// file URIs, raw pictures (screenshots, a browser's "Copy image") are saved
// to a temporary file; both go the attachment way. Anything else (and a URI
// list with no local file in it) pastes as text. `sel` is the clipboard the
// paste reads (Primary: a middle click, which pastes text plain).
bool Composer::pasteMedia(const std::vector<std::string> &mimes, plat::Selection sel) {
    const auto has = [&](std::string_view m) {
        return std::find(mimes.begin(), mimes.end(), m) != mimes.end();
    };
    // The picture types taken, in order of preference.
    static constexpr const char *kImages[] = {
        "image/png", "image/jpeg", "image/gif", "image/webp", "image/bmp"
    };
    const char      *imgMime = nullptr;
    std::string_view imgExt;
    for (const char *k : kImages)
        if (has(k)) {
            imgMime = k;
            imgExt  = mime::extension(k);
            break;
        }
    const bool uris = has("text/uri-list");
    if (!uris && !imgMime)
        return false;
    plat::App         &pa         = _ctx.app.platform();
    std::weak_ptr<int> alive      = _alive;
    const bool         plain      = sel == plat::Selection::Primary;
    auto               pasteImage = [this, alive, &pa, imgMime, imgExt, sel, plain] {
        pa.requestClipboard(
            imgMime,
            [this, alive, &pa, imgExt, sel, plain](std::optional<std::string> data) {
                if (alive.expired())
                    return;
                if (!data || data->empty())
                    return _edit->pasteText(plain, sel);
                // "Pasted image 2026-10-01 142305-1.png": distinct within a second.
                static int            seq = 0;
                const base::CivilTime t   = base::localTime(base::nowSecs());
                char                  stamp[48];
                std::snprintf(
                    stamp,
                    sizeof stamp,
                    "%04d-%02d-%02d %02d%02d%02d-%d",
                    t.year,
                    t.month,
                    t.day,
                    t.hour,
                    t.minute,
                    t.second,
                    ++seq
                );
                const std::string path = file::join(
                    pa.standardDir(plat::StandardDir::Temp),
                    str::concat({"Pasted image ", stamp, ".", imgExt})
                );
                auto bytes = std::make_shared<std::string>(std::move(*data));
                auto ok    = std::make_shared<bool>(false);
                model::runInBackground(
                    pa,
                    [path, bytes, ok] { *ok = file::writeAtomic(path, *bytes); },
                    [this, alive, path, ok] {
                        if (alive.expired() || !*ok)
                            return;
                        attachFiles({path});
                    }
                );
            },
            sel
        );
    };
    if (!uris) {
        pasteImage();
        return true;
    }
    pa.requestClipboard(
        "text/uri-list",
        [this, alive, pasteImage, imgMime, sel, plain](std::optional<std::string> list) {
            if (alive.expired())
                return;
            const std::vector<std::string> paths =
                list ? screens::uriListPaths(*list) : std::vector<std::string>();
            if (!paths.empty()) {
                attachFiles(paths);
            } else if (imgMime) {
                pasteImage();
            } else {
                _edit->pasteText(plain, sel);
            }
        },
        sel
    );
    return true;
}

void Composer::rebuildChips() {
    _chipRow->clearChildren();
    _chips->setVisible(!_files.empty());
    _chipRow->setVisible(!_files.empty());
    // What the files still attached showed already is kept; a new one is
    // read once (its head and size).
    std::vector<ChipInfo> infos;
    infos.reserve(_files.size());
    for (const std::string &path : _files) {
        auto it = std::find_if(_chipInfo.begin(), _chipInfo.end(), [&](const ChipInfo &c) {
            return c.path == path;
        });
        if (it != _chipInfo.end()) {
            infos.push_back(std::move(*it));
            continue;
        }
        ChipInfo c;
        c.path = path;
        c.size = std::max<int64_t>(0, file::size(path));
        if (!looksLikeImage(path) && looksLikeText(path))
            c.preview = textPreview(path);
        infos.push_back(std::move(c));
    }
    _chipInfo = std::move(infos);
    for (size_t i = 0; i < _files.size(); ++i) {
        const std::string &path  = _files[i];
        const ChipInfo    &info  = _chipInfo[i];
        auto              *chip  = _chipRow->add<View>();
        const bool         image = looksLikeImage(path);
        chip->style().size(kChipW, kChipH).stack().noShrink();
        chip->setBorder(C::ChipBorder);
#ifdef MSGA_HAVE_MESSAGES
        if (image) {
            chip->setBackground(C::None, 8);
            auto *img = chip->add<screens::CachedImage>(
                _ctx.images, path, screens::ImageCache::Shape::Rounded, 8
            );
            img->style().size(kChipW, kChipH);
        } else
#endif
        {
            chip->setBackground(C::ChipBg, 8);
            if (looksLikeText(path)) {
                auto *t = chip->add<Label>(info.preview, Font::Tiny, C::FormTextFaint);
                t->style().padding(6).alignSelf(Align::Start);
                t->setLineHeight(1.2f);
            }
        }
        (void)image;
        // Name and size bottom-left on plates.
        auto *plates = chip->add<View>();
        plates->style().padding(8).spacing(2).justifyContent(Justify::End);
        plates->setHitTransparent(true);
        plate(plates, chipName(path), Font::PlateName);
        plate(plates, str::byteSize(info.size), Font::Tiny);
        // The remove cross, top-right.
        auto *x = chip->add<GlyphButton>(
            Icon::X, 16, 10, image ? C::OverlayText : C::FormIcon, tr("Remove attachment")
        );
        x->setBackground(image ? C::OverlayBg : C::ChipBorder, 8);
        x->setLook({C::None, C::DropArrow, C::DropArrow, C::None, 8});
        x->style().alignSelf(Align::Start).margins(kChipW - 20, 4, 0, 0);
        x->onClick = [this, i] { removeAttachment(i); };
    }
    invalidateLayout();
}

bool Composer::onEvent(Event &e) {
    switch (e.type) {
    case EventType::DropEnter:
    case EventType::DropMove:
        if (!screens::dragOffersFiles(e.raw))
            return false;
        e.dropAction = plat::DropAction::Copy;
        if (!_dropHover) { // the accent border doubles as the drop-target cue
            _dropHover = true;
            refreshLook();
        }
        return true;
    case EventType::DropLeave:
        if (_dropHover) {
            _dropHover = false;
            refreshLook();
        }
        return true;
    case EventType::Drop: {
        _dropHover = false;
        refreshLook();
        const auto paths = screens::droppedFiles(e.raw);
        if (paths.empty())
            return false;
        attachFiles(paths);
        return true;
    }
    default:
        return View::onEvent(e);
    }
}

void Composer::layout() {
    // The editor is capped at half the window's height, then it scrolls.
    if (Window *w = window()) {
        const float line = std::ceil(font(Font::Body).size * 1.4f);
        _edit->setMaxLines(std::max(1, int((w->size().h / 2 - 20) / std::max(1.f, line))));
    }
    View::layout();
}

void Composer::typing() {
    // Slack wants a typing event at most every few seconds while typing.
    const double now = _ctx.app.nowMs();
    if (_edit->empty() || now - _lastTyping < 3000 || _key.conv == model::kNoConv)
        return;
    _lastTyping = now;
    _ctx.backend.userTyping(_key.conv, _key.thread);
}

// Each line the selection touches gets the
// prefix ("1. ", "2. " … when ordered); without one, just the caret's line.
void Composer::prefixLines(std::string_view prefix, bool numbered) {
    const std::string    &t    = _edit->text();
    const bool            sel  = _edit->hasSelection();
    uint32_t              from = std::min(_edit->anchor(), _edit->caret());
    uint32_t              to   = std::max(_edit->anchor(), _edit->caret());
    std::vector<uint32_t> starts;
    uint32_t              s = from;
    while (s > 0 && t[s - 1] != '\n')
        --s;
    starts.push_back(s);
    if (sel)
        for (uint32_t i = s; i < to && i < t.size(); ++i)
            if (t[i] == '\n' && i + 1 <= to)
                starts.push_back(i + 1);
    for (size_t k = starts.size(); k-- > 0;) {
        _edit->setSelection(starts[k], starts[k]);
        _edit->insertText(
            numbered ? str::concat({str::number(int64_t(k + 1)), ". "}) : std::string(prefix)
        );
    }
    const std::string &after = _edit->text();
    uint32_t           end   = starts.back();
    while (end < after.size() && after[end] != '\n')
        ++end;
    _edit->setSelection(end, end);
    _edit->focus();
}

// A selection is fenced (trimmed) with the caret
// after the closing fence; without one the caret lands between the fences.
void Composer::wrapCodeBlock() {
    if (_edit->hasSelection()) {
        const std::string sel(str::trim(_edit->selectedText()));
        _edit->insertText(str::concat({"```\n", sel, "\n```"}));
    } else {
        const uint32_t at = _edit->caret();
        _edit->insertText("```\n\n```");
        _edit->setSelection(at + 4, at + 4);
    }
    _edit->focus();
}

// Markers around the selection, or a pair with the
// caret between them.
void Composer::wrapInline(std::string_view marker) {
    if (_edit->hasSelection()) {
        _edit->insertText(str::concat({marker, _edit->selectedText(), marker}));
    } else {
        const uint32_t at = _edit->caret() + uint32_t(marker.size());
        _edit->insertText(str::concat({marker, marker}));
        _edit->setSelection(at, at);
    }
    _edit->focus();
}

void Composer::insertPill(
    uint32_t from, uint32_t to, const std::string &display, const std::string &raw
) {
    _edit->setSelection(from, to);
    _edit->insertText(display);
    _edit->setSelection(from, from + uint32_t(display.size()));
    _edit->setLink(raw);
    const uint32_t end = from + uint32_t(display.size());
    _edit->setSelection(end, end);
    _edit->insertText(" "); // typing formats never continue a link
}

// ── Spelling ────────────────────────────────────────────────────────────────

void Composer::scheduleSpell() {
    if (!spell::Checker::instance().active())
        return;
    // A moment after typing stops: a long text is checked once, not per key.
    _ctx.app.cancelTimer(_spellTimer);
    _spellTimer = _ctx.app.addTimer(150, false, [this] {
        _spellTimer = 0;
        respell();
    });
}

void Composer::respell() {
    const uint32_t seq = ++_spellSeq;
    if (!spell::Checker::instance().active()) {
        _edit->setSquiggles({});
        return;
    }
    // Mention, channel and GIF pills are never checked.
    std::vector<spell::Span> pills;
    for (const TextEdit::Run &r : _edit->runs())
        if (!r.link.empty() && r.link[0] == '<')
            pills.push_back({r.start, r.end - r.start});
    std::weak_ptr<int> alive = _alive;
    spell::Checker::instance().check(
        _edit->text(),
        std::move(pills),
        [this, alive, seq, text = _edit->text()](std::vector<spell::Span> bad) {
            // A newer check is on its way when the text moved on meanwhile.
            if (alive.expired() || seq != _spellSeq || _edit->text() != text ||
                !spell::Checker::instance().active())
                return;
            std::vector<TextEdit::Range> ranges;
            ranges.reserve(bad.size());
            for (const spell::Span &w : bad)
                ranges.push_back({w.start, w.end()});
            _edit->setSquiggles(std::move(ranges));
        }
    );
}

// The spelling menu: on an underlined word, up to
// five suggestions (bold), "Add to dictionary" and "Ignore" lead the
// standard edit menu. The suggestions are made off the UI thread (Hunspell
// can take 15–120 ms), so the menu opens once they are in.
bool Composer::spellMenu(uint32_t offset, PointF at) {
    const TextEdit::Range *hit = _edit->squiggleAt(offset);
    if (!hit || !spell::Checker::instance().active())
        return false;
    const TextEdit::Range range = *hit;
    const std::string     word  = _edit->text().substr(range.from, range.to - range.from);
    std::weak_ptr<int>    alive = _alive;
    spell::Checker::instance().suggest(
        word, 5, [this, alive, range, word, at](std::vector<std::string> suggestions) {
            Window *w = window();
            if (alive.expired() || !w)
                return;
            constexpr int         kSuggest = TextEdit::kFirstOwnerMenuId;
            constexpr int         kAdd = kSuggest + 10, kIgnore = kSuggest + 11;
            std::vector<MenuItem> items;
            for (size_t i = 0; i < suggestions.size(); ++i) {
                MenuItem it;
                it.id    = kSuggest + int(i);
                it.label = suggestions[i];
                it.bold  = true;
                items.push_back(std::move(it));
            }
            if (suggestions.empty()) {
                MenuItem none;
                none.label   = tr("No spelling suggestions");
                none.enabled = false;
                items.push_back(std::move(none));
            }
            items.push_back(MenuItem::separatorItem());
            items.push_back({kAdd, tr("Add to dictionary")});
            // Spelling: stop underlining this word until MSGA restarts.
            items.push_back({kIgnore, tr("Ignore")});
            items.push_back(MenuItem::separatorItem());
            for (MenuItem &it : _edit->standardMenuItems())
                items.push_back(std::move(it));
            Menu::show(
                *w,
                {at.x, at.y, 0, 0},
                std::move(items),
                [this, alive, range, word, suggestions](int id) {
                    if (alive.expired())
                        return;
                    if (id == kAdd)
                        return spell::Checker::instance().addToDictionary(word);
                    if (id == kIgnore)
                        return spell::Checker::instance().ignore(word);
                    if (id >= kSuggest && id < kSuggest + int(suggestions.size())) {
                        // Only onto the word it was offered for: the text may
                        // have moved while the menu was open.
                        const std::string &t = _edit->text();
                        if (range.to > t.size() || t.compare(range.from, word.size(), word) != 0)
                            return;
                        _edit->setSelection(range.from, range.to);
                        _edit->insertText(suggestions[size_t(id - kSuggest)]); // one undo step
                        _edit->focus();
                        return;
                    }
                    _edit->runStandardItem(id);
                },
                Popup::Place::Over
            );
        }
    );
    return true;
}

// ── Popups ──────────────────────────────────────────────────────────────────

void Composer::openLinkPopup() {
    Window *w = window();
    if (!w)
        return;
    const uint32_t     a     = std::min(_edit->anchor(), _edit->caret());
    const uint32_t     c     = std::max(_edit->anchor(), _edit->caret());
    // Below the Link button (the key opens it a quarter along the toolbar).
    const RectF        lb    = _tools[4]->windowRect();
    std::weak_ptr<int> alive = _alive;
    showLinkPopup(
        *w,
        lb,
        _edit->selectedText(),
        [this, alive, a, c](const std::string &url, const std::string &label) {
            if (alive.expired())
                return;
            _edit->setSelection(a, c);
            _edit->insertText(
                label.empty() || label == url ? str::concat({"<", url, ">"})
                                              : str::concat({"<", url, "|", label, ">"})
            );
            _edit->focus();
        }
    );
}

void Composer::openSchedule() {
    Window *w = window();
    if (!w || _editTs || _key.conv == model::kNoConv)
        return;
    // chat.scheduleMessage takes text only: refuse rather than drop the
    // attachments.
    if (!_files.empty()) {
        if (_ctx.backend.onError)
            _ctx.backend.onError(
                tr("Files can't be scheduled. Send them now or remove them first.")
            );
        return;
    }
    if (str::trim(_edit->text()).empty())
        return;
    std::weak_ptr<int> alive = _alive;
    showSchedulePopup(*w, _dropBtn->windowRect(), [this, alive](int64_t at) {
        if (alive.expired())
            return;
        mrkdwn::Composed  composed = mrkdwn::compose(mrkdwn());
        const std::string text(str::trim(composed.mrkdwn));
        if (text.empty() || !_files.empty())
            return;
        // A failure puts the text back (the banner says why) while the
        // composer is still on that conversation and empty.
        const DraftStash::Key key = _key;
        auto done = [this, alive, key, html = _edit->html()](bool ok, const std::string &) {
            if (ok || alive.expired() || !(_key == key) || !_edit->empty() || _editTs)
                return;
            _edit->insertHtml(html);
            refreshLook();
            compositionChanged();
        };
        _edit->clear();
        _drafts.erase(_key);
        refreshLook();
        compositionChanged();
        if (!composed.blocks.empty())
            _ctx.backend.scheduleBlocks(
                key.conv, text, std::move(composed.blocks), key.thread, at, std::move(done)
            );
        else
            _ctx.backend.scheduleMessage(key.conv, text, key.thread, at, std::move(done));
    });
}

void Composer::openEmoji() {
    Window *w = window();
    if (!w)
        return;
#ifdef MSGA_HAVE_MESSAGES
    // The messages screens' searchable picker (shared with reactions); it
    // inserts the shortcode, which Slack renders.
    std::weak_ptr<int> alive = _alive;
    screens::EmojiPicker::show(
        *w, _emojiBtn->windowRect(), _ctx, [this, alive](const std::string &name) {
            if (alive.expired())
                return;
            _edit->insertText(str::concat({":", name, ":"}));
            _edit->focus();
        }
    );
#endif
}

void Composer::openGif() {
    Window *w = window();
    if (!w)
        return;
    GifHooks h;
    h.key    = gifKey;
    h.setKey = [this](const std::string &k) {
        if (setGifKey)
            setGifKey(k);
        refreshTips();
    };
    h.openUrl                = _ctx.openUrl;
    std::weak_ptr<int> alive = _alive;
    h.picked                 = [this, alive](std::string url, std::string title) {
        if (alive.expired())
            return;
        // Held as Slack's labelled link; shown as a pill.
        std::string label;
        for (char ch : title)
            if (ch != '|' && ch != '<' && ch != '>')
                label += ch;
        if (label.size() > 4 && str::endsWith(label, " GIF"))
            label.resize(label.size() - 4);
        const std::string raw =
            label.empty() ? str::concat({"<", url, ">"}) : str::concat({"<", url, "|", label, ">"});
        const std::string display =
            label.empty() ? std::string(tr("GIF")) : i18n::arg(tr("GIF \xC2\xB7 %1"), label);
        uint32_t           at = std::min(_edit->anchor(), _edit->caret());
        const std::string &t  = _edit->text();
        if (at > 0 && t[at - 1] != ' ' && t[at - 1] != '\n') {
            _edit->setSelection(at, std::max(_edit->anchor(), _edit->caret()));
            _edit->insertText(" ");
            ++at;
        }
        insertPill(at, std::max(at, _edit->caret()), display, raw);
        _edit->focus();
    };
    showGifPicker(*w, _gifBtn->windowRect(), _ctx, std::move(h));
}

// The @ popup (users, and @channel / @everyone / @here outside DMs), the #
// channel list, ":" emoji: what the word before the caret asks for.
void Composer::updatePickList() {
    if (_inPick) // the caret probe below moves the selection
        return;
    PickInputs in;
    in.caret   = _edit->caret();
    in.anchor  = _edit->anchor();
    in.conv    = _key.conv;
    in.focused = _edit->focused();
    in.thread  = _threadMode;
    in.window  = window() != nullptr;
    // Nothing moved since the last look, and the list is as that look left
    // it (not closed by Escape or a pick meanwhile): the same answer again.
    if (_pickInValid && in == _pickIn && _pickText == _edit->text() &&
        _pickShown == (_pick != nullptr))
        return;
    _pickIn = in;
    _pickText.assign(_edit->text()); // into the kept buffer: no allocation per key
    _pickInValid = true;
    computePickList();
    _pickShown = _pick != nullptr;
}

void Composer::computePickList() {
    ++_pickRecomputes;
    Window            *w       = window();
    const std::string &t       = _edit->text();
    const uint32_t     cur     = _edit->caret();
    auto               dismiss = [this] {
        if (_pick) {
            PickList *p = _pick;
            _pick       = nullptr;
            p->close();
        }
    };
    if (!w || _edit->hasSelection() || cur == 0 || !_edit->focused()) {
        dismiss();
        return;
    }
    // The word before the caret; the trigger must start it.
    uint32_t start = cur;
    while (start > 0 && t[start - 1] != ' ' && t[start - 1] != '\n')
        --start;
    if (start >= cur) { // right after a blank: no word
        dismiss();
        return;
    }
    const char trig = t[start];
    if (trig != '@' && trig != '/' && trig != '#' && trig != ':') { // nothing asks for a list
        dismiss();
        return;
    }
    const std::string query = t.substr(start + 1, cur - start - 1);
    // Inside a pill: nothing to complete.
    for (const TextEdit::Run &r : _edit->runs())
        if (!r.link.empty() && start >= r.start && start < r.end) {
            dismiss();
            return;
        }
    std::vector<PickList::Item> items;
    const model::Store         &st = _ctx.store;
    const bool        dm   = _key.conv != model::kNoConv && st.conversation(_key.conv).isDirect();
    bool              wide = false;
    // The query folded once; every label is matched folded already.
    const std::string fq   = utf8::foldCase(query);
    if (trig == '@') {
        struct Alias {
            const char *name, *insert, *desc;
        };
        static constexpr Alias kAliases[] = {
            {"@channel", "<!channel>", N_("Notify everyone in this channel")},
            {"@everyone", "<!everyone>", N_("Notify everyone in your workspace")},
            {"@here", "<!here>", N_("Notify every online member here")},
        };
        if (!dm)
            for (const Alias &a : kAliases)
                if (query.empty() || utf8::containsFolded(a.name, query)) {
                    PickList::Item it;
                    it.kind     = PickList::Item::Kind::Alias;
                    it.title    = a.name;
                    it.display  = a.name;
                    it.insert   = _threadMode ? std::string() : std::string(a.insert);
                    it.subtitle = tr(a.desc);
                    if (_threadMode)
                        it.status = tr("Disabled in threads");
                    items.push_back(std::move(it));
                }
        wide      = _threadMode && !dm;
        int added = 0;
        if (_foldedStore != &st) { // another workspace: its revisions are its own
            _folded.clear();
            _foldedStore = &st;
        }
        if (_folded.size() < st.userCount())
            _folded.resize(st.userCount());
        for (model::UserRef u = 0; u < st.userCount() && added < 50; ++u) {
            const model::User &user = st.user(u);
            if (user.placeholder || user.deleted)
                continue;
            const std::string_view label = user.label();
            FoldedUser            &f     = _folded[u];
            // Both names match whichever one shows (Settings → Names).
            if (const uint64_t rev = st.userRevision(u); f.rev != rev || f.id != user.id) {
                f.rev      = rev; // new or changed
                f.id       = user.id;
                f.folded   = utf8::foldCase(label);
                f.labelEnd = uint32_t(f.folded.size());
                f.folded += '\n';
                f.folded += utf8::foldCase(user.name);
                f.nameEnd = uint32_t(f.folded.size());
                f.folded += '\n';
                f.folded += utf8::foldCase(str::concat({user.realName, "\n", user.profileName}));
                ++_mentionFolds;
            }
            // One search over all four: the query is a word (no line break),
            // so a match never spans two of them.
            if (!query.empty() && !utf8::containsPrefolded(f.folded, fq))
                continue;
            const std::string_view flabel(f.folded.data(), f.labelEnd);
            const std::string_view fname(
                f.folded.data() + f.labelEnd + 1, f.nameEnd - f.labelEnd - 1
            );
            PickList::Item it;
            it.kind    = PickList::Item::Kind::Mention;
            // The chip reads as names show elsewhere (Settings → Names).
            it.display = str::concat({"@", label});
            it.title   = str::concat({"@", label});
            if (u == st.me)
                it.title = str::concat({it.title, " ", tr("(you)")});
            it.insert = str::concat({"<@", user.id, ">"});
            if (!user.name.empty() && !utf8::containsPrefolded(fname, flabel) && fname != flabel)
                it.subtitle = user.name;
            it.bot      = user.bot;
            it.avatar   = user.avatar;
            it.presence = user.dnd ? 3 : (user.active || user.bot) ? 1 : 2;
            items.push_back(std::move(it));
            ++added;
        }
    } else if (trig == '/' && start == 0) {
        // Slash commands only at the very start of the message; a bare
        // "/" lists them all, by name.
        if (!_ctx.backend.capabilities().slashCommands || _key.conv == model::kNoConv) {
            dismiss();
            return;
        }
        refreshCommands();
        for (size_t i = 0; i < _cmdSorted.size(); ++i) {
            const model::Backend::Command &c = _cmdSorted[i];
            if (!str::startsWith(_cmdFolded[i], fq))
                continue;
            PickList::Item it;
            it.kind     = PickList::Item::Kind::Command;
            it.title    = "/" + c.name;
            it.insert   = it.title;
            it.display  = it.title;
            it.usage    = c.usage;
            it.subtitle = c.desc;
            it.source   = c.source; // "source · description"
            // The service's own commands bring its icon (the Claude
            // Code avatar); Slack's built-ins draw the Slack mark instead.
            it.avatar   = !c.icon.empty()                ? c.icon
                          : c.app || c.source == "Slack" ? std::string()
                                                         : st.workspaceIcon;
            items.push_back(std::move(it));
            if (items.size() >= 50)
                break;
        }
    } else if (trig == '#') {
        if (_foldedConvs.size() < st.conversationCount())
            _foldedConvs.resize(st.conversationCount());
        for (model::ConvRef c = 0; c < st.conversationCount() && items.size() < 50; ++c) {
            const model::Conversation &cv = st.conversation(c);
            if (cv.isDirect())
                continue;
            FoldedName &f = _foldedConvs[c];
            if (f.name != cv.name) { // new, renamed, another workspace
                f.name   = cv.name;
                f.folded = utf8::foldCase(cv.name);
            }
            if (!utf8::containsPrefolded(f.folded, fq))
                continue;
            PickList::Item it;
            it.kind           = PickList::Item::Kind::Channel;
            it.title          = cv.name;
            it.display        = "#" + cv.name;
            it.insert         = str::concat({"<#", cv.id, "|", cv.name, ">"});
            it.privateChannel = cv.kind == model::ConvKind::Private;
            items.push_back(std::move(it));
        }
    } else if (trig == ':' && !query.empty() && query[0] >= 'a' && query[0] <= 'z') {
        // Only a lowercase word reads as the start of a code (":)" is a smiley).
        bool code = true;
        for (char ch : query)
            code &= (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                    ch == '-' || ch == '+';
        if (code)
            for (const EmojiCompletion &e : emojiCompletions(_ctx.store(), query)) {
                PickList::Item it;
                it.kind = PickList::Item::Kind::Plain;
                if (e.custom) { // no Unicode form: the :code: Slack renders
                    it.display = str::concat({":", e.name, ":"});
                    it.insert  = it.display;
                } else {
                    const std::string g = emoji::toUnicode(e.name);
                    it.display          = str::concat({g, "  :", e.name, ":"});
                    it.insert           = g;
                }
                items.push_back(std::move(it));
            }
    }
    if (items.empty()) {
        dismiss();
        return;
    }
    // Anchored at the trigger character: above its line.
    const RectF    er    = _edit->windowRect();
    const uint32_t keepA = _edit->anchor(), keepC = _edit->caret();
    _inPick = true;
    _edit->setSelection(start, start);
    const RectF cr = _edit->caretRect();
    _edit->setSelection(keepA, keepC);
    _inPick = false;
    // The anchor sits the editor's inset (10, 6) up and left of the
    // character; the @ list hangs from the line's bottom, the others
    // from its top.
    const PointF anchor{er.x + cr.x - 10, er.y + cr.y + (trig == '@' ? cr.h : 0) - 6};
    _pickFrom = start;
    if (_pick) { // the open list takes the new rows (no close and reopen per key)
        _pick->update(std::move(items), wide, anchor);
        return;
    }
    std::weak_ptr<int> alive = _alive;
    _pick                    = PickList::show(
        *w, _avatars, anchor, std::move(items), wide, [this, alive](const PickList::Item &it) {
            if (alive.expired())
                return;
            _pick = nullptr;
            pick(it);
        }
    );
    std::weak_ptr<int> alive2 = _alive;
    PickList          *raw    = _pick;
    _pick->onClosed           = [this, alive2, raw] {
        if (!alive2.expired() && _pick == raw)
            _pick = nullptr;
    };
}

// The "/" list's commands for the open conversation: fetched when the
// backend's revision moved (a session's commands come and go; without one,
// each time), sorted and folded only when they changed.
void Composer::refreshCommands() {
    const uint64_t rev = _ctx.backend.commandsRevision(_key.conv);
    if (rev && rev == _cmdRev && _cmdConv == _key.conv)
        return;
    std::vector<model::Backend::Command> cmds = _ctx.backend.commands(_key.conv);
    bool same = _cmdConv == _key.conv && _cmdRev == rev && cmds.size() == _cmdNames.size();
    _cmdRev   = rev;
    for (size_t i = 0; same && i < cmds.size(); ++i)
        same = cmds[i].name == _cmdNames[i];
    if (same)
        return;
    _cmdConv = _key.conv;
    _cmdNames.clear();
    for (const auto &c : cmds)
        _cmdNames.push_back(c.name);
    std::sort(cmds.begin(), cmds.end(), [](const auto &a, const auto &b) {
        return a.name < b.name;
    });
    _cmdFolded.clear();
    for (const auto &c : cmds)
        _cmdFolded.push_back(utf8::foldCase(c.name));
    _cmdSorted = std::move(cmds);
}

// A pick replaces the trigger and the query typed after it: mentions,
// channels and GIFs as pills, "@here" in a thread and emoji as text.
void Composer::pick(const PickList::Item &it) {
    const uint32_t from = _pickFrom, to = _edit->caret();
    if (to < from)
        return;
    if (!it.insert.empty() && it.insert[0] == '<') {
        insertPill(from, to, it.display, it.insert);
    } else {
        _edit->setSelection(from, to);
        _edit->insertText(str::concat({it.insert.empty() ? it.display : it.insert, " "}));
    }
    _edit->focus();
}

// ── Editing a sent message ──────────────────────────────────────────────────

namespace {

bool giphy(std::string_view url) {
    return url.find("giphy.com/") != std::string_view::npos;
}

} // namespace

namespace {

void loadMrkdwnAs(TextEdit &edit, const model::Store &store, std::string_view text, bool markdown) {
    // Markdown's editor shows Slack's *bold* as **bold**.
    std::string md;
    if (markdown) {
        md   = mrkdwn::markdownBold(text);
        text = md;
    }
    struct Pill {
        uint32_t    from, to;
        std::string raw; // the token, or a link's URL
    };
    std::string       plain;
    std::vector<Pill> pills;
    size_t            i = 0;
    while (i < text.size()) {
        const size_t lt = text.find('<', i);
        if (lt == std::string_view::npos) {
            plain += mrkdwn::decodeEntities(text.substr(i));
            break;
        }
        plain += mrkdwn::decodeEntities(text.substr(i, lt - i));
        const size_t gt = text.find('>', lt);
        if (gt == std::string_view::npos) {
            plain += mrkdwn::decodeEntities(text.substr(lt));
            break;
        }
        const std::string_view tok  = text.substr(lt, gt - lt + 1);
        const std::string_view body = tok.substr(1, tok.size() - 2);
        const size_t           bar  = body.find('|');
        const std::string_view head = body.substr(0, bar);
        const std::string_view lab =
            bar == std::string_view::npos ? std::string_view() : body.substr(bar + 1);
        std::string display;
        if (!head.empty() && head[0] == '@') {
            const model::UserRef u = store.findUser(head.substr(1));
            display = lab.empty()
                          ? str::concat(
                                {"@", u != model::kNoUser ? store.user(u).label() : head.substr(1)}
                            )
                          : str::concat({"@", lab});
        } else if (!head.empty() && head[0] == '#') {
            const model::ConvRef c = store.findConversation(head.substr(1));
            display                = str::concat(
                {"#",
                 !lab.empty()          ? std::string(lab)
                 : c != model::kNoConv ? store.conversation(c).name
                                       : std::string(head.substr(1))}
            );
        } else if (head == "!here" || head == "!channel" || head == "!everyone") {
            display = str::concat({"@", head.substr(1)});
        } else if (giphy(head)) {
            display = lab.empty() ? std::string(tr("GIF")) : i18n::arg(tr("GIF \xC2\xB7 %1"), lab);
        }
        if (display.empty() && mrkdwn::looksLikeUrl(head)) {
            // <url|label> or <url>: its label (the URL when bare) linked, so
            // toMrkdwn writes the token back. Left literal, the brackets
            // would go out as typed text and Slack would escape them.
            const std::string url  = mrkdwn::decodeEntities(head);
            const uint32_t    from = uint32_t(plain.size());
            plain += lab.empty() ? url : mrkdwn::decodeEntities(lab);
            pills.push_back({from, uint32_t(plain.size()), url});
        } else if (display.empty()) { // any other <…> stays literal
            plain += tok;
        } else {
            const uint32_t from = uint32_t(plain.size());
            plain += display;
            pills.push_back({from, uint32_t(plain.size()), std::string(tok)});
        }
        i = gt + 1;
    }
    edit.setText(plain);
    for (const Pill &p : pills) {
        edit.setSelection(p.from, p.to);
        edit.setLink(p.raw);
    }
    edit.setSelection(uint32_t(plain.size()), uint32_t(plain.size()));
}

} // namespace

void loadMrkdwn(TextEdit &edit, const model::Store &store, std::string_view text) {
    loadMrkdwnAs(edit, store, text, markdownFormatting());
}

std::string redialectDraft(const model::Store &store, const std::string &html, bool toMarkdown) {
    TextEdit from;
    from.insertHtml(html);
    if (from.text().find('*') == std::string::npos)
        return html; // nothing reads differently
    TextEdit to;
    loadMrkdwnAs(to, store, editorMrkdwn(from, !toMarkdown), toMarkdown);
    return to.text() == from.text() ? html : to.html();
}

} // namespace shell
