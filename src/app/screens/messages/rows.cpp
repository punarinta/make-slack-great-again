#include "app/screens/messages/rows.h"

#include "app/mrkdwn/emoji.h"
#include "app/screens/common/avatar_initial.h"
#include "app/screens/common/canvas_doc.h"
#include "app/screens/common/file_dialogs.h"
#include "app/screens/common/message_rules.h"
#include "app/screens/common/message_text.h"
#include "app/screens/common/tag_badge.h"
#include "app/screens/messages/audio_card.h"
#include "app/screens/messages/rich.h"
#include "app/screens/messages/table_view.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "ui/controls.h"

#include <algorithm>
#include <cmath>

namespace screens {

using i18n::arg;
using i18n::tr;
using ui::Align;
using ui::C;
using ui::Font;

namespace {

// Message list metrics.
constexpr float kPadH = 16, kPadV = 8, kPadVBottom = 4, kPadVGrouped = 3;
constexpr float kAvSize = 36, kAvGap = 10, kHdrH = 20, kReactGap = 6;
constexpr float kThreadAv = 24, kImgMax = 360, kAttachBar = 4;
// Inline images at most 400 × 300.
constexpr int   kImgMaxW = 400, kImgMaxH = 300;
constexpr float kBannerH = 18; // the mini-banner rows

// Pinned background, dismiss icon, reminder background, reminder text.
constexpr C kBannerColors[4] = {C::PinnedBg, C::FormIcon, C::ReminderBg, C::FormLink};
gfx::Color  bannerColor(int i) {
    return ui::color(kBannerColors[i]);
}

// An attachment's raw colour ("#3FCB8E"); the strong border when it is
// anything but six hex digits.
gfx::Color barColor(std::string_view hex) {
    gfx::Color c = ui::color(C::BorderStrong);
    if (hex.size() - (!hex.empty() && hex[0] == '#') == 6)
        gfx::parseHexColor(hex, &c);
    return c;
}

// A strip of a raw data colour (attachment side bars).
class ColorBar final : public ui::View {
public:
    explicit ColorBar(gfx::Color c) : _c(c) { style().width(kAttachBar).noShrink(); }
    void paint(gfx::Painter &p) override { p.fillRoundRect(bounds(), 2, _c); }

private:
    gfx::Color _c;
};

// An image thumbnail that opens the viewer on click.
class Thumb final : public CachedImage {
public:
    Thumb(
        MessageList &list,
        std::string  path,
        int          w,
        int          h,
        float        dw,
        float        dh,
        bool         animated,
        Ts           ts,
        bool         file
    )
        : CachedImage(list.ctx().images, path, ImageCache::Shape::Rounded, 8), _list(list),
          _path(std::move(path)), _ts(ts), _w(w), _h(h), _dw(dw), _dh(dh), _file(file) {
        setAnimated(animated);
        setCursor(plat::Cursor::Hand);
        style().noShrink();
        // Text in text.tertiary, centred, until the pixels are there (failed
        // downloads keep it too). A link
        // preview's picture shows nothing until then.
        if (file)
            setLoadingText(tr("Loading image\xE2\x80\xA6"), C::FormTextFaint);
        else
            setPlaceholder(C::None);
    }
    // The display size, scaled down (aspect kept) to fit a narrow column.
    ui::SizeF measureContent(float aw, float) override {
        const float s = aw > 0 && aw < _dw ? aw / _dw : 1.f;
        return {std::floor(_dw * s), std::floor(_dh * s)};
    }
    void setTs(Ts ts) { _ts = ts; }
    // A gallery tile: the placeholder is clipped to the rounded tile
    // and has no "Loading image…" (a tile is too narrow for it).
    void setTile() {
        _tile = true;
        setLoadingText({}, C::FormTextFaint);
    }
    // A file's preview: the viewer opens on the file (with its actions),
    // the file bar names it by `key` (its path; a PDF shows its thumb).
    std::function<void()> onOpen;
    std::string           key;

    bool onEvent(ui::Event &e) override {
        if (e.type == ui::EventType::PointerDown && e.button == plat::Button::Left)
            return true;
        if (e.type == ui::EventType::PointerUp) {
            if (e.button == plat::Button::Left && bounds().contains(e.pos)) {
                if (onOpen)
                    onOpen();
                else
                    _list.openImage(_path, _w, _h);
            }
            return true;
        }
        if (e.type == ui::EventType::PointerCancel)
            return true;
        if ((e.type == ui::EventType::PointerEnter || e.type == ui::EventType::PointerLeave) && _ts)
            _list.fileHovered(
                this, _ts, key.empty() ? _path : key, e.type == ui::EventType::PointerEnter
            );
        return false; // nothing on right click
    }

protected:
    // The single-image placeholder: message.imagePlaceholderBg under a
    // 1-px imagePlaceholderBorder, square.
    void paintPlaceholder(gfx::Painter &p, ui::RectF r, float radius) override {
        if (!_file) {
            CachedImage::paintPlaceholder(p, r, radius);
            return;
        }
        const bool  dark = ui::app()->dark();
        const float rr   = _tile ? radius : 0;
        p.fillRoundRect(r, rr, dark ? 0xff262626U : 0xfff5f5f5U);
        p.strokeRoundRect(r, rr, 1, dark ? 0xff3e3e3eU : 0xffccccccU);
    }

private:
    MessageList &_list;
    std::string  _path;
    Ts           _ts; // the message (0: no file menu)
    int          _w, _h;
    float        _dw, _dh;
    bool         _file;
    bool         _tile = false;
};

// A picture shown inline (a canvas is a card).
bool isInlinePreview(const model::File &f) {
    return f.hasPreview() && !(f.isCanvas() && !f.id.empty());
}

// The multi-image gallery (2+ previews): equal
// cover-cropped tiles 180 px high in rows of up to three (two for four
// pictures), 8 px apart, at most 520 wide; each row fills the width (a short
// last row's tiles get wider). No file names. The children are the tiles.
class Gallery final : public ui::View {
public:
    static constexpr float kTileH = 180, kGap = 8, kMaxW = 520;

    ui::SizeF measureContent(float aw, float) override {
        const int n    = int(childCount());
        const int rows = (n + columns(n) - 1) / std::max(1, columns(n));
        return {regionW(aw), rows * kTileH + std::max(0, rows - 1) * kGap};
    }
    void layout() override {
        const int   n = int(childCount()), cols = columns(n);
        const float w = regionW(width());
        float       y = 0;
        for (int i = 0; i < n;) {
            const int rowCount = std::min(cols, n - i);
            float     x        = 0;
            for (int c = 0; c < rowCount; ++c) {
                const int   left = rowCount - c;
                const float tw   = std::max(1.f, std::floor((w - x - (left - 1) * kGap) / left));
                child(size_t(i + c))->setFrame({x, y, tw, kTileH});
                x += tw + kGap;
            }
            i += rowCount;
            y += kTileH + kGap;
        }
    }

private:
    // Columns: 2 side by side, 4 as 2×2, else up to three.
    static int   columns(int n) { return n == 4 ? 2 : std::min(n, 3); }
    static float regionW(float aw) { return aw > 0 ? std::min(aw, kMaxW) : kMaxW; }
};

// A file card: click opens the file; hovering shows the file bar.
// The plain file chip: 60 px high,
// radius 8, fileChipBorder / fileChipBg; a 36-px type square (radius 6) in
// the type's colour with the extension (or a code glyph); the name in bold
// (middle-elided), 2 px, then "type · size" at 0.82×. As wide as the text
// needs, 220…640. No buttons: hovering shows the file bar.
class FileChip final : public ui::Clickable {
public:
    FileChip(MessageList *list, Ts ts, const model::File &f)
        : _list(list), _ts(ts), _path(f.path), _name(f.name) {
        setLook({C::None, C::None, C::None, C::None, 8});
        std::string_view ext = file::extension(f.name);
        _code                = isCodeExt(ext);
        _color               = typeColor(f, _code);
        if (ext.size() >= 1 && ext.size() <= 5)
            _label = str::asciiUpper(ext.substr(0, 4));
        else if (!f.prettyType.empty())
            _label = str::asciiUpper(std::string_view(f.prettyType).substr(0, 4));
        else
            _label = "FILE";
        _sub = f.prettyType;
        if (const std::string sz = str::byteSize(f.size, str::ByteSize::File); !sz.empty())
            _sub = _sub.empty() ? sz : _sub + " \xC2\xB7 " + sz;
    }
    bool onEvent(ui::Event &e) override {
        if (_list &&
            (e.type == ui::EventType::PointerEnter || e.type == ui::EventType::PointerLeave))
            _list->fileHovered(this, _ts, _path, e.type == ui::EventType::PointerEnter);
        return Clickable::onEvent(e); // nothing on right click
    }
    void styleChanged() override {
        _n.reset();
        Clickable::styleChanged();
    }
    ui::SizeF measureContent(float aw, float) override {
        build();
        float w = std::clamp(12 + 36 + 12 + textWidth() + 12 + 1, 220.f, 640.f);
        if (aw > 0 && aw < w)
            w = aw;
        return {w, 60};
    }
    void paint(gfx::Painter &p) override {
        build();
        const text::Layout *n = _n.get(), *s = _s.get();
        if (const float aw = width() - 60 - 12; aw < textWidth()) {
            // Narrower than the name or the type line: those elided to it.
            if (!_nCut || _cutW != aw) {
                _nCut = line(_name, nameStyle(), aw);
                _sCut = _sub.empty() ? nullptr : line(_sub, subStyle(), aw);
                _cutW = aw;
            }
            n = _nCut.get(), s = _sCut.get();
        }
        const ui::RectF b = bounds();
        paintCardFrame(p, b);
        const ui::RectF icon{12, 12, 36, 36};
        p.fillRoundRect(icon, 6, _color);
        if (_code) {
            gfx::drawIcon(p, gfx::Icon::CodeFile, {20, 20, 20, 20}, 0xffffffffU);
        } else {
            _l->paint(
                p,
                snapPx(
                    {icon.x + std::floor((36 - _l->width()) / 2),
                     icon.y + std::floor((36 - _l->height()) / 2)}
                )
            );
        }
        const float textH = n->height() + (s ? 2 + s->height() : 0);
        const float top   = std::floor((60 - textH) / 2);
        n->paint(p, snapPx({60, top}));
        if (s)
            s->paint(p, snapPx({60, top + n->height() + 2}));
    }

private:
    static bool isCodeExt(std::string_view e) {
        static const char *const kCode[] = {"c",    "cpp", "h",     "hpp", "cs",  "css", "go",
                                            "java", "js",  "ts",    "tsx", "jsx", "py",  "rb",
                                            "rs",   "sh",  "swift", "kt",  "php", "sql", "diff"};
        for (const char *c : kCode)
            if (e == c)
                return true;
        return false;
    }
    static gfx::Color typeColor(const model::File &f, bool code) {
        // The file type's colour.
        const std::string &m   = f.mime;
        auto               has = [&](const char *s) { return m.find(s) != std::string::npos; };
        if (code)
            return 0xffde4e2bU;
        if (has("pdf"))
            return 0xffe44d4dU;
        if (has("word") || has("document"))
            return 0xff2b579aU;
        if (has("excel") || has("spreadsheet"))
            return 0xff217346U;
        if (has("powerpoint") || has("presentation"))
            return 0xffd24726U;
        if (m.rfind("video/", 0) == 0)
            return 0xff7b2d8bU;
        if (m.rfind("audio/", 0) == 0)
            return 0xff1e7a6eU;
        if (has("zip") || has("x-tar") || has("gzip") || has("x-7z") || has("x-rar"))
            return 0xff8b6914U;
        if (m.rfind("text/", 0) == 0 || has("json") || has("xml"))
            return 0xff555555U;
        return 0xff888888U;
    }
    static text::Style nameStyle() {
        return ui::pxFont(15, text::Weight::Bold, ui::color(C::FormText));
    }
    static text::Style subStyle() {
        return ui::pxFont(15 * 0.82f, text::Weight::Regular, ui::color(C::FormTextMuted));
    }
    // One line, "…" past maxW (the whole text when it fits).
    std::unique_ptr<text::Layout> line(const std::string &s, const text::Style &st, float maxW) {
        text::AttributedText t;
        t.append(s, st);
        text::LayoutOptions o;
        o.maxLines = 1;
        o.ellipsis = true;
        o.maxWidth = maxW;
        return text::Layout::build(std::move(t), o, windowScale());
    }
    // The texts at their natural width, shaped once per style and scale.
    void build() {
        if (_n && _scale == windowScale())
            return;
        _scale = windowScale();
        _n     = line(_name, nameStyle(), 1e9f);
        _s     = _sub.empty() ? nullptr : line(_sub, subStyle(), 1e9f);
        _nCut.reset();
        _sCut.reset();
        _l = text::layoutPlain(
            _label, ui::pxFont(15 * 0.66f, text::Weight::Bold, 0xffffffffU), _scale
        );
    }
    float textWidth() const {
        return std::max(std::ceil(_n->width()), _s ? std::ceil(_s->width()) : 0.f);
    }

    MessageList                  *_list; // null: a preview (no file bar)
    Ts                            _ts;
    std::string                   _path, _name, _label, _sub;
    std::unique_ptr<text::Layout> _n, _s, _l;
    std::unique_ptr<text::Layout> _nCut, _sCut; // elided to _cutW (a narrow frame)
    float                         _cutW = 0, _scale = 0;
    gfx::Color                    _color = 0;
    bool                          _code  = false;
};

// A message author's avatar: hovering it shows the profile card.
// A message's avatar. Until the picture is there (downloading, or none):
// a tile with the name's first letter in white
// on that letter's hue.
class LetterAvatar : public CachedImage {
public:
    LetterAvatar(ImageCache &images, const std::string &path, std::string_view name)
        : CachedImage(images, path, ImageCache::Shape::Rounded, 6),
          _letter(avatarInitial(name.empty() ? std::string_view("?") : name)) {}
    void styleChanged() override {
        _layout.reset();
        CachedImage::styleChanged();
    }

protected:
    void paintPlaceholder(gfx::Painter &p, ui::RectF r, float radius) override {
        paintInitial(p, *this, r, radius, initialHue(_letter), _letter, _layout);
    }

private:
    std::string                   _letter;
    std::unique_ptr<text::Layout> _layout;
};

class AuthorAvatar final : public LetterAvatar {
public:
    AuthorAvatar(Context &ctx, const std::string &path, model::UserRef user)
        : LetterAvatar(ctx.images, path, ctx.store().user(user).label()), _ctx(ctx), _user(user) {}
    bool onEvent(ui::Event &e) override {
        if ((e.type == ui::EventType::PointerEnter || e.type == ui::EventType::PointerLeave) &&
            _ctx.profileHover)
            _ctx.profileHover(_user, windowRect(), e.type == ui::EventType::PointerEnter ? 1 : 0);
        return false;
    }

private:
    Context       &_ctx;
    model::UserRef _user;
};

// A huddle_thread row's avatar, the headphones on
// a rounded surface.highlightStrong square.
class HuddleTile final : public ui::View {
public:
    void paint(gfx::Painter &p) override {
        const ui::RectF r{0, 0, width(), height()};
        p.fillRoundRect(r, 4, color(C::FormHighlightStrong));
        const float g = std::floor(r.w * 5 / 9);
        gfx::drawIcon(
            p, gfx::Icon::Headphones, {(r.w - g) / 2, (r.h - g) / 2, g, g}, color(C::FormIcon)
        );
    }
};

// A huddle's duration: "45m", "2h", "1h 30m" (at least a minute).
std::string huddleDuration(int64_t secs) {
    const int64_t mins = std::max<int64_t>(1, (secs + 30) / 60);
    if (mins < 60)
        return arg(tr("%1m"), str::number(mins));
    const int64_t h = mins / 60, m = mins % 60;
    return m ? arg(tr("%1h %2m"), str::number(h), str::number(m)) : arg(tr("%1h"), str::number(h));
}

} // namespace

// Who is (or was) in the huddle and for how long —
// "You" first, then the others in Slack's order, more than three cut to two
// and "N others".
std::string huddleSummaryText(const Store &st, const model::Huddle &h) {
    std::vector<std::string> names;
    bool                     withMe = false;
    for (model::UserRef u : h.attendees) {
        if (u == st.me && st.me != model::kNoUser) {
            withMe = true;
            continue;
        }
        const std::string_view l = st.user(u).label();
        names.emplace_back(l.empty() ? std::string_view(st.user(u).id) : l);
    }
    if (withMe)
        names.insert(names.begin(), tr("You"));
    if (names.empty())
        return h.ended ? tr("Nobody joined the huddle.")
                       : tr("The huddle is waiting for people to join.");
    const bool plural = names.size() > 1 || withMe;
    if (names.size() > 3) {
        const int64_t rest = int64_t(names.size()) - 2;
        names.resize(2);
        names.push_back(i18n::trn("%n other", "%n others", rest));
    }
    std::string who = names.back();
    if (names.size() > 1) {
        std::string lead;
        for (size_t i = 0; i + 1 < names.size(); ++i)
            lead += (i ? ", " : "") + names[i];
        who = arg(tr("%1 and %2"), lead, names.back());
    }
    if (!h.ended)
        return arg(plural ? tr("%1 are in the huddle.") : tr("%1 is in the huddle."), who);
    if (h.startSec <= 0 || h.endSec < h.startSec)
        return arg(plural ? tr("%1 were in the huddle.") : tr("%1 was in the huddle."), who);
    return arg(
        plural ? tr("%1 were in the huddle for %2.") : tr("%1 was in the huddle for %2."),
        who,
        huddleDuration(h.endSec - h.startSec)
    );
}

namespace {

text::AttributedText styled(std::string_view s, Font f, C c, uint32_t linkId = 0) {
    text::AttributedText t;
    text::Style          st = ui::font(f);
    st.color                = ui::themed(c);
    st.linkId               = linkId;
    t.append(s, st);
    return t;
}

// A bot button: a
// 13-px semibold label in a radius-4 face; primary is message.botButtonFill,
// danger danger.def (both with onDark text), the rest botButtonBg under a
// botButtonBorder hairline. Pressing it is Backend::pressButton.
class BotButton final : public ui::Clickable {
public:
    BotButton(MessageList &list, Ts ts, const model::Button &b)
        : _list(list), _ts(ts), _id(b.id), _style(b.style) {
        setLook({C::None, C::None, C::None, C::None, 4});
        // Padding for the 15-px body: 7 px over a 13-px
        // label's line (two body lines tall), 12 px at the sides.
        style().padding(12, 7).noShrink();
        const bool filled = _style != model::Button::Style::Default;
        ui::styledLabel(
            this,
            b.label,
            ui::pxFont(
                13, text::Weight::Semibold, ui::color(filled ? C::TooltipText : C::FormText)
            ),
            1
        )
            ->setHitTransparent(true);
        onClick = [this] {
            const ui::RectF r = windowRect();
            _list.pressButton(_ts, _id, {r.x + r.w / 2, r.y});
        };
    }
    void paint(gfx::Painter &p) override {
        const bool      dark = ui::app()->dark();
        const bool      hov  = hovered();
        const ui::RectF b    = bounds();
        using S              = model::Button::Style;
        if (_style == S::Danger) {
            p.fillRoundRect(b, 4, ui::color(hov ? C::DangerFillHover : C::DangerFill));
        } else if (_style == S::Primary) {
            p.fillRoundRect(
                b, 4, dark ? (hov ? 0xff1a9a78U : 0xff148567U) : (hov ? 0xff148567U : 0xff007a5aU)
            );
        } else {
            p.fillRoundRect(
                b, 4, dark ? (hov ? 0xff2e2e2eU : 0xff222222U) : (hov ? 0xfff8f8f8U : 0xffffffffU)
            );
            p.strokeRoundRect(b, 4, 1, dark ? 0xff5e5e5eU : 0x4d1d1c1dU);
        }
    }

private:
    MessageList         &_list;
    Ts                   _ts;
    std::string          _id;
    model::Button::Style _style;
};

// An attachment card: tells the row it is hovered (the dismiss "×").
class AttachCard final : public ui::View {
public:
    AttachCard(MessageRow *row, int index) : _row(row), _index(index) {}
    bool onEvent(ui::Event &e) override {
        if (_row && _index >= 0 &&
            (e.type == ui::EventType::PointerEnter || e.type == ui::EventType::PointerLeave))
            _row->attachHovered(_index, this, e.type == ui::EventType::PointerEnter);
        return false;
    }

private:
    MessageRow *_row;
    int         _index;
};

// An attachment that
// is nothing but image blocks (the GIF picker's) or a table (table messages)
// draws without the colour bar.
bool onlyBlocks(const model::Attachment &a, model::Block::Kind kind) {
    using K  = model::Block::Kind;
    bool any = false;
    for (const model::Block &b : a.blocks) {
        if (b.kind == kind && (kind != K::Image || !b.image.empty()))
            any = true;
        else if (
            b.kind == K::Divider || b.kind == K::Image || b.kind == K::Table || !b.text.empty()
        )
            return false;
    }
    return any && a.pretext.empty() && a.author.empty() && a.title.empty() && a.text.empty() &&
           a.fields.empty() && a.footer.empty() && a.image.empty();
}

} // namespace

// A canvas title: entities decoded, :codes: as emoji, <@U…>
// mentions as names.
std::string canvasTitle(const Context &ctx, const model::File &f) {
    return f.title.empty() ? f.name : std::string(str::trim(plainText(ctx.store(), f.title)));
}

text::AttributedText canvasPreviewText(
    Context &ctx, const std::string &raw, const model::File &f, std::vector<std::string> *images
) {
    // In a canvas preview's HTML, standard
    // emoji come as <img data-is-slack>:name:</img> — the code is what is
    // drawn —, member mentions as a bare <a>@U…</a>, shown as a chip with
    // the member's name.
    constexpr std::string_view kUser = "msga-user:";
    std::string                html;
    html.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] != '<') {
            const size_t e = std::min(raw.find('<', i), raw.size());
            html.append(raw, i, e - i);
            i = e;
            continue;
        }
        const size_t      e   = std::min(raw.find('>', i), raw.size() - 1);
        std::string_view  tag = std::string_view(raw).substr(i, e - i + 1);
        const std::string low = str::asciiLower(tag);
        if ((str::startsWith(low, "<img") && low.find("data-is-slack") != std::string::npos) ||
            str::startsWith(low, "</img")) {
            i = e + 1;
            continue;
        }
        if (low == "<a>" && raw.compare(e + 1, 1, "@") == 0) {
            size_t j = e + 2;
            while (j < raw.size() && std::isalnum(uint8_t(raw[j])))
                ++j;
            if (j > e + 3 && (raw[e + 2] == 'U' || raw[e + 2] == 'W')) {
                html += str::concat({"<a href=\"", kUser, raw.substr(e + 2, j - e - 2), "\">"});
                i = e + 1;
                continue;
            }
        }
        html.append(tag);
        i = e + 1;
    }
    std::string              t;
    std::vector<uint16_t>    fmt;
    std::vector<std::string> links;
    ui::rich::fromHtml(
        canvas::editorHtml(html, {f.title, canvasTitle(ctx, f)}, nullptr), &t, &fmt, &links
    );
    const Store         &store = ctx.store;
    text::AttributedText out;
    const text::Style    body = ui::font(Font::Body, C::Text);
    // Text with :codes: as Unicode emoji or custom emoji boxes (never
    // inside code).
    auto                 emit = [&](std::string_view s, const text::Style &st, bool code) {
        size_t from = 0;
        for (size_t i = 0; !code && i < s.size(); ++i) {
            if (s[i] != ':')
                continue;
            size_t j = i + 1;
            while (j < s.size() && s[j] != ':' && s[j] != ' ' && s[j] != '\n')
                ++j;
            if (j >= s.size() || s[j] != ':' || j == i + 1)
                continue;
            const Store::EmojiGlyph g = store.emojiFor(s.substr(i + 1, j - i - 1));
            if (!g.resolved())
                continue;
            out.append(s.substr(from, i - from), st);
            text::Style es = st;
            if (!g.unicode.empty()) {
                out.append(g.unicode, es);
            } else if (images) {
                images->push_back(g.image);
                es.inlineBoxId = uint32_t(images->size());
                es.boxWidth = es.boxHeight = std::round(es.size * 1.35f);
                out.append(s.substr(i, j + 1 - i), es);
            } else {
                out.append(s.substr(i, j + 1 - i), es);
            }
            from = i = j + 1;
            --i;
        }
        out.append(s.substr(from), st);
    };
    bool first = true;
    for (size_t at = 0; at <= t.size();) {
        const size_t nl = std::min(t.find('\n', at), t.size());
        size_t       a  = at;
        at              = nl + 1;
        std::string_view line(t.data() + a, nl - a);
        if (str::startsWith(str::trim(line), "```"))
            continue;
        text::Style st = body;
        std::string marker;
        if (str::startsWith(line, "# ") || str::startsWith(line, "## ") ||
            str::startsWith(line, "### ")) {
            const size_t level = line.find(' ');
            st.weight          = text::Weight::Bold;
            st.size *= level == 1 ? 1.4f : level == 2 ? 1.2f : 1.1f;
            a += level + 1;
        } else if (str::startsWith(line, "- [ ] ") || str::startsWith(line, "- [x] ")) {
            marker = line[3] == 'x' ? "\xE2\x98\x91 " : "\xE2\x98\x90 ";
            a += 6;
        } else if (str::startsWith(line, "- ")) {
            marker = "\xE2\x80\xA2 ";
            a += 2;
        } else if (str::startsWith(line, "> ")) {
            st.color = ui::themed(C::TextMuted);
            a += 2;
        }
        if (!first)
            out.append("\n", body);
        first = false;
        if (!marker.empty())
            out.append(marker, st);
        for (size_t i = a; i < nl;) {
            size_t j = i + 1;
            while (j < nl && j < fmt.size() && i < fmt.size() && fmt[j] == fmt[i])
                ++j;
            text::Style    s    = st;
            const uint16_t fm   = i < fmt.size() ? fmt[i] : 0;
            const size_t   link = fm >> 8;
            if (fm & ui::TextEdit::Bold)
                s.weight = text::Weight::Bold;
            if (fm & ui::TextEdit::Italic)
                s.italic = true;
            if (fm & ui::TextEdit::Strike)
                s.strike = true;
            if (fm & ui::TextEdit::Code) {
                s.mono       = true;
                s.background = ui::themed(C::CodeBg);
            }
            if (link && link <= links.size() && str::startsWith(links[link - 1], kUser)) {
                // A member mention: a chip with the name, as in messages.
                const std::string_view id = std::string_view(links[link - 1]).substr(kUser.size());
                const model::UserRef   u  = store.findUser(id);
                const std::string_view name =
                    u != model::kNoUser ? store.user(u).mentionLabel() : std::string_view();
                // The editor's spacing can put a blank inside the anchor:
                // it stays outside the chip.
                const std::string_view run  = std::string_view(t).substr(i, j - i);
                const size_t           lead = run.find_first_not_of(' ');
                const size_t           tail = run.find_last_not_of(' ');
                text::Style            ps   = s;
                ps.color                    = ui::themed(C::MentionText);
                ps.background               = ui::themed(
                    u != model::kNoUser && u == store.me ? C::MentionSelfBg : C::MentionBg
                );
                ps.weight =
                    s.weight == text::Weight::Bold ? text::Weight::Bold : text::Weight::Medium;
                if (lead != std::string_view::npos && lead > 0)
                    out.append(run.substr(0, lead), s);
                out.append(str::concat({"@", name.empty() ? id : name}), ps);
                if (tail != std::string_view::npos && tail + 1 < run.size())
                    out.append(run.substr(tail + 1), s);
                i = j;
                continue;
            }
            if (link)
                s.color = ui::themed(C::Link);
            emit(std::string_view(t).substr(i, j - i), s, fm & ui::TextEdit::Code);
            i = j;
        }
    }
    return out;
}

namespace {

// A canvas document past what its card can show (at most 40 lines, 4 KB:
// still taller than the card's 229-px body at any width up to 600), so it
// isn't shaped whole. Never inside a UTF-8 sequence or an inline box.
void cutPreview(text::AttributedText &t) {
    constexpr size_t kMaxBytes = 4096;
    constexpr int    kMaxLines = 40;
    size_t           cut       = std::min(t.text.size(), kMaxBytes);
    int              lines     = 0;
    for (size_t i = 0; i < cut; ++i)
        if (t.text[i] == '\n' && ++lines == kMaxLines)
            cut = i;
    if (cut == t.text.size())
        return;
    while (cut > 0 && (uint8_t(t.text[cut]) & 0xC0) == 0x80)
        --cut;
    while (!t.spans.empty() && t.spans.back().start >= cut)
        t.spans.pop_back();
    if (!t.spans.empty()) {
        text::Span &last = t.spans.back();
        if (last.style.inlineBoxId && last.end > cut) {
            cut = last.start;
            t.spans.pop_back();
        } else {
            last.end = uint32_t(std::min<size_t>(last.end, cut));
        }
    }
    t.text.resize(cut);
}

// The canvas preview card: 300 px high, up to 600
// wide, the attachment card colours; a 60-px header with the blue canvas
// tile, the title and "Canvas" over a hairline, then the start of the
// document, cut by the card and fading out.
class CanvasCard final : public ui::Clickable {
public:
    CanvasCard(MessageList &list, const model::File &f)
        : _list(list), _file(f), _titleText(canvasTitle(list.ctx(), f)), _anim(list.ctx(), *this) {
        setLook({C::None, C::None, C::None, C::None, 8});
        style().height(300).noShrink();
        onClick = [this] { _list.openCanvas(_file); };
    }
    ui::SizeF measureContent(float aw, float) override { return {std::min(600.f, aw), 300}; }
    void      styleChanged() override {
        _title.reset(), _sub.reset(), _body.reset();
        _parsed = false; // its colours are the theme's
        Clickable::styleChanged();
    }
    void layout() override {
        if (width() != _laidW) { // the texts are wrapped to the width
            _title.reset(), _sub.reset(), _body.reset();
            _laidW = width();
        }
        Clickable::layout();
    }
    void paint(gfx::Painter &p) override {
        constexpr float kPad = 14, kTile = 36, kHdr = 60;
        const ui::RectF b     = bounds();
        const float     scale = windowScale();
        paintCardFrame(p, b);
        const ui::RectF tile{kPad, std::floor((kHdr - kTile) / 2), kTile, kTile};
        p.fillRoundRect(tile, 8, 0xff1d9bd1U);
        gfx::drawIcon(p, gfx::Icon::Canvas, {tile.x + 8, tile.y + 8, 20, 20}, 0xffffffffU);
        const float textX = tile.x + kTile + 12, textW = std::max(1.f, b.w - kPad - textX);
        if (!_title) {
            text::LayoutOptions o;
            o.maxLines = 1;
            o.ellipsis = true;
            o.maxWidth = textW;
            text::AttributedText t, s;
            t.append(_titleText, ui::font(Font::BodyBold));
            s.append(
                _file.prettyType.empty() ? std::string(tr("Canvas")) : _file.prettyType,
                ui::pxFont(15 * 0.85f, text::Weight::Regular, ui::color(C::TextMuted))
            );
            _title = text::Layout::build(std::move(t), o, scale);
            _sub   = text::Layout::build(std::move(s), o, scale);
        }
        const float blockH = _title->height() + 2 + _sub->height();
        const float ty     = std::floor((kHdr - blockH) / 2);
        _title->paint(p, snapPx({textX, ty}));
        _sub->paint(p, snapPx({textX, ty + _title->height() + 2}));
        p.fillRect({1, kHdr, b.w - 2, 1}, ui::color(C::FileChipBorder));
        const ui::RectF    body{kPad, kHdr + 10, b.w - 2 * kPad, b.h - kHdr - 10 - 1};
        int                state = 0;
        const std::string *html  = _list.canvasPreview(_file.id, &state);
        if (!html) {
            if (!_body) {
                _body = text::layoutPlain(
                    state < 0 ? tr("Preview unavailable") : tr("Loading preview\xE2\x80\xA6"),
                    ui::pxFont(15 * 0.85f, text::Weight::Regular, ui::color(C::TextMuted)),
                    scale
                );
                _bodyLoaded = false;
            }
            _body->paint(p, snapPx({body.x, body.y}));
            return;
        }
        if (!_body || !_bodyLoaded) {
            if (!_parsed) {
                _images.clear();
                _bodyText = canvasPreviewText(_list.ctx(), *html, _file, &_images);
                cutPreview(_bodyText);
                ui::resolveSpans(_bodyText);
                _parsed = true;
            }
            text::LayoutOptions o;
            o.maxWidth  = body.w;
            _body       = text::Layout::build(_bodyText, o, scale);
            _bodyLoaded = true;
        }
        p.save();
        p.clipRect(body);
        const ui::PointF o = snapPx({body.x, body.y});
        _body->paint(p, o);
        if (!_images.empty())
            _anim.schedule(paintEmojiBoxes(_list.ctx(), p, *_body, o, _images, this));
        p.restore();
        if (_body->height() > body.h) { // fade the cut into the card
            const gfx::Color bg = ui::color(C::FileChipBg);
            p.save();
            p.clipRoundRect({1, 1, b.w - 2, b.h - 2}, 7);
            p.fillRectGradient(
                {1, b.h - 37, b.w - 2, 36}, {0, b.h - 37}, bg & 0x00ffffffU, {0, b.h - 1}, bg
            );
            p.restore();
        }
    }

private:
    MessageList                  &_list;
    model::File                   _file;
    std::string                   _titleText;
    std::unique_ptr<text::Layout> _title, _sub, _body;
    text::AttributedText          _bodyText; // the document as parsed (once _parsed)
    std::vector<std::string>      _images;   // the body's custom emoji, box id i: [i - 1]
    EmojiFrameTimer               _anim;
    float                         _laidW      = -1;
    bool                          _bodyLoaded = false, _parsed = false;
};

// The reply bar: participants, "N replies" and — by state — "Last reply
// …", "View thread ›" under the pointer, "Close thread ×" while the thread
// is open (in the panel, or expanded inline). Half the text column wide; a
// border and wash on hover.
class ReplyBar final : public ui::Clickable {
public:
    ReplyBar(MessageList &list, const model::Message &m) : _list(list), _ts(m.ts) {
        setLook({C::None, C::None, C::None, C::None, 6});
        style().row().height(36).padding(6, 0, 6, 0).spacing(6).items(Align::Center);
        Context     &ctx = list.ctx();
        const Store &st  = ctx.store;
        auto        *avs = add<ui::View>();
        avs->style().row().spacing(3).margins(0, 0, 2, 0);
        for (size_t i = 0; i < m.replyUsers.size() && i < 5; ++i)
            avs->add<CachedImage>(
                   ctx.images, st.user(m.replyUsers[i]).avatar, ImageCache::Shape::Rounded, 5
            )
                ->style()
                .size(kThreadAv, kThreadAv);
        add<ui::Label>(
            m.replyCount == 1 ? std::string(tr("1 reply"))
                              : arg(tr("%1 replies"), str::number(m.replyCount)),
            Font::SmallBold,
            C::ReplyLink
        );
        _sub = add<ui::Label>("", Font::Small, C::TextMuted);
        _sub->setMaxLines(1);
        _sub->style().shrink = 1;
        add<ui::View>()->style().flex(1);
        _glyph = add<ui::Label>("", Font::Small, C::TextMuted);
        if (m.latestReply) {
            const std::string when =
                base::lastReplyLabel(model::tsSecs(m.latestReply), base::nowSecs());
            _last = when.empty() ? std::string(tr("Last reply")) : arg(tr("Last reply %1"), when);
        }
        refresh();
    }
    ui::SizeF measureContent(float aw, float ah) override {
        // The bar is half the text column (the spacer makes the flex
        // measure take all of it); the "Last reply" text shrinks first.
        const ui::SizeF s = measureFlex(aw, ah);
        return {aw < ui::kInf ? std::floor(aw / 2) : s.w, s.h};
    }
    bool onEvent(ui::Event &e) override {
        if (e.type == ui::EventType::PointerEnter || e.type == ui::EventType::PointerLeave) {
            _hover = e.type == ui::EventType::PointerEnter;
            refresh();
        }
        return Clickable::onEvent(e);
    }
    void paint(gfx::Painter &p) override {
        if (!_hover)
            return;
        const ui::RectF b = bounds();
        p.fillRoundRect(b, 6, ui::byTheme(0xff282828U, 0xfff8f8f8U));
        p.strokeRoundRect(b, 6, 1, ui::byTheme(0xff3e3e3eU, 0xffd1d5dbU));
    }

private:
    void refresh() {
        const bool open = _list.threadOpen(_ts);
        _sub->setText(
            open     ? std::string(tr("Close thread"))
            : _hover ? std::string(tr("View thread"))
                     : _last
        );
        _glyph->setText(open ? "\xC3\x97" : _hover ? "\xE2\x80\xBA" : "");
        update();
    }
    MessageList &_list;
    Ts           _ts;
    std::string  _last;
    ui::Label   *_sub = nullptr, *_glyph = nullptr;
    bool         _hover = false;
};

// The inline thread's "Reply to thread": bold, 0.9×, the reply-link colour,
// underlined under the pointer.
class FooterLink final : public ui::Clickable {
public:
    explicit FooterLink(std::string text) : _text(std::move(text)) {
        setLook({C::None, C::None, C::None, C::None, 0});
        style().height(24).margins(0, 6, 0, 6).alignSelf(Align::Start).stack().items(Align::Center);
        _l = add<ui::Label>();
        _l->setHitTransparent(true);
        setText(false);
    }
    bool onEvent(ui::Event &e) override {
        if (e.type == ui::EventType::PointerEnter || e.type == ui::EventType::PointerLeave)
            setText(e.type == ui::EventType::PointerEnter);
        return Clickable::onEvent(e);
    }

private:
    void setText(bool underline) {
        text::Style st = ui::pxFont(15 * 0.9f, text::Weight::Bold, ui::themed(C::ReplyLink));
        st.underline   = underline;
        text::AttributedText t;
        t.append(_text, st);
        _l->setRichText(std::move(t));
    }
    std::string _text;
    ui::Label  *_l = nullptr;
};

} // namespace

void paintCardFrame(gfx::Painter &p, ui::RectF r, float radius, ui::C border) {
    p.fillRoundRect(r, radius, ui::color(C::FileChipBg));
    p.strokeRoundRect(r, radius, 1, ui::color(border));
}

std::vector<std::string> selectableTexts(Context &ctx, const model::Message &m) {
    // What MessageRow registers, in the same order (buildBlocks / buildBody).
    if (!m.extra || m.extra->blocks.empty()) {
        RichOptions o;
        o.edited = m.edited;
        return bodyTexts(ctx, m.text, o);
    }
    std::vector<std::string> out;
    for (const model::Block &b : m.extra->blocks) {
        if (b.kind == model::Block::Kind::Table) {
            out.push_back(tableText(ctx, b.rows));
            continue;
        }
        if (b.kind != model::Block::Kind::Text && b.kind != model::Block::Kind::Header)
            continue;
        RichOptions o;
        if (b.kind == model::Block::Kind::Header) {
            o.font  = Font::BodyBold;
            o.scale = 1.1f;
        }
        for (std::string &t : bodyTexts(ctx, b.text, o))
            out.push_back(std::move(t));
    }
    if (m.edited) {
        RichOptions o;
        o.edited = true;
        for (std::string &t : bodyTexts(ctx, {}, o))
            out.push_back(std::move(t));
    }
    return out;
}

ui::Clickable *addFileChip(ui::View *parent, const model::File &f, MessageList *list, Ts ts) {
    auto *chip = parent->add<FileChip>(list, ts, f);
    chip->style().alignSelf(Align::Start);
    return chip;
}

// ── FlowRow ─────────────────────────────────────────────────────────────────

ui::SizeF FlowRow::place(float width, bool apply) {
    float x = 0, y = 0, lineH = 0, maxW = 0;
    for (size_t i = 0; i < childCount(); ++i) {
        ui::View *c = child(i);
        if (!c->visible())
            continue;
        const ui::SizeF s = c->measure(width, ui::kInf);
        if (x > 0 && x + s.w > width) {
            x = 0;
            y += lineH + _gap;
            lineH = 0;
        }
        if (apply)
            c->setFrame({x, y, s.w, s.h});
        x += s.w + _gap;
        lineH = std::max(lineH, s.h);
        maxW  = std::max(maxW, x - _gap);
    }
    return {maxW, y + lineH};
}

ui::SizeF FlowRow::measureContent(float aw, float) {
    return place(aw, false);
}
void FlowRow::layout() {
    place(width(), true);
}

// ── Day / divider rows ──────────────────────────────────────────────────────

DayRow::DayRow() {
    style().stack().height(32).items(Align::Center);
    _label = add<ui::Label>("", Font::SmallBold, C::Text);
    _label->setBorder(C::Border);
    _label->setBackground(C::Surface, 12);
    _label->style().padding(12, 3);
}

void DayRow::paint(gfx::Painter &p) {
    p.fillRect({kPadH, std::floor(height() / 2), width() - 2 * kPadH, 1}, ui::color(C::Border));
}

DividerRow::DividerRow() {
    style().row().padding(kPadH, 8).spacing(12).items(Align::Center);
    _label = add<ui::Label>("", Font::Small, C::TextMuted);
    add<ui::Separator>()->style().flex(1);
}

// ── MessageRow ──────────────────────────────────────────────────────────────

MessageRow::MessageRow(MessageList &list, int kind) : _list(list), _kind(kind) {
    _list.rowMade(this);
    setHoverRepaint(true);
    setRole(ui::Role::ListItem);
}

MessageRow::~MessageRow() {
    _list.rowGone(this);
    _list.ctx().images.forget(this);
}

void MessageRow::bind(const MessageList::Item &item) {
    const model::Message *m     = _list.message(item.ts);
    const int64_t         today = base::localDay(base::nowSecs());
    // The same message changed only past its body (a reaction, a reply, a
    // pin, saved): the body stays as built — no mrkdwn parse, no shaping,
    // no image lookups — and the rest is built again.
    const bool            keep  = m && _col && item.ts == _ts && today == _day && bodyHolds(*m);
    if (keep) {
        while (_col->childCount() > _tailAt)
            _col->remove(_col->child(_col->childCount() - 1));
    } else {
        clearChildren();
        _sel.clear();
        _col = nullptr;
        _bodyExtras.reset();
    }
    _attachCard  = nullptr;
    _attach      = -1;
    _overDismiss = false;
    _hoverLayout = nullptr;
    _pinLayout   = nullptr;
    _savedLayout = nullptr;
    _pinText.clear();
    _savedText.clear();
    _reminded = false;
    _dueAt    = 0;
    _day      = today;
    _ts       = item.ts;
    if (!m)
        return;
    _pending = m->pending;
    if (keep) {
        applyBanners(*m, _kind == kRowGrouped);
        buildTail(_col, *m);
    } else if (_kind == kRowSystem) {
        buildSystem(*m);
    } else {
        buildMessage(*m, _kind == kRowGrouped);
    }
    _list.applySelection(*this);
}

namespace {

// What a message's body is drawn from besides its extras (MessageRow::
// bodyHolds); the Store's app-local marks too (an AI transcript a card shows
// comes as an Update of the message).
uint64_t bodyHash(const model::Message &m, const Store &st) {
    const uint64_t flags = uint64_t(m.user) << 2 | uint64_t(m.edited) << 1 | uint64_t(m.pending);
    return std::hash<std::string_view>{}(m.text) ^ (flags + 1) * 0x9e3779b97f4a7c15ull ^
           (st.localRevision() + 1) * 0xc2b2ae3d27d4eb4full;
}

} // namespace

bool MessageRow::bodyHolds(const model::Message &m) const {
    // Anything else a body shows (names, emoji, folded images, expanded
    // cards, settings) moves the list's epoch when it changes.
    return _bodyEpoch == _list.bodyEpoch() && _bodyHash == bodyHash(m, _list.ctx().store) &&
           (m.extra ? _bodyExtras && *_bodyExtras == *m.extra : !_bodyExtras);
}

void MessageRow::rememberBody(const model::Message &m) {
    _bodyHash   = bodyHash(m, _list.ctx().store);
    _bodyEpoch  = _list.bodyEpoch();
    _bodyExtras = m.extra ? std::make_unique<model::MessageExtras>(*m.extra) : nullptr;
}

bool MessageRow::reuse() {
    const int64_t now = base::nowSecs();
    if ((_dueAt && now >= _dueAt) || base::localDay(now) != _day)
        return false;
    _attachCard  = nullptr;
    _attach      = -1;
    _overDismiss = false;
    _list.applySelection(*this);
    return true;
}

void MessageRow::buildSystem(const model::Message &m) {
    Context &ctx = _list.ctx();
    style().row().padding(kPadH, 6, kPadH, 6).spacing(kAvGap).items(Align::Center);
    auto *gutter = add<ui::View>();
    gutter->style().size(kAvSize, 20).stack().items(Align::End);
    auto *av = gutter->add<CachedImage>(
        ctx.images, ctx.store().user(m.user).avatar, ImageCache::Shape::Rounded, 4
    );
    av->style().size(20, 20);
    std::string text = plainText(ctx.store(), m.text);
    if (text.empty())
        text = arg(tr("%1 joined."), ctx.store().user(m.user).label());
    auto *l = add<ui::Label>(std::move(text), Font::Body, C::TextMuted);
    l->style().flex(1);
}

void MessageRow::buildHeader(ui::View *col, const model::Message &m, bool tight) {
    Context     &ctx = _list.ctx();
    const Store &st  = ctx.store;
    const bool   bot = isBot(st, m);
    const auto  &x   = m.extra;
    auto        *hdr = col->add<ui::View>();
    hdr->style().row().height(kHdrH + 2).items(Align::Center);
    if (tight)
        hdr->style().margins(0, 0, 0, 2);
    const std::string name =
        m.isHuddle()
            ? std::string(x->huddle.ended ? tr("A huddle happened") : tr("A huddle started"))
            : std::string(authorName(st, m));
    auto                          *nameL = hdr->add<RichLabel>(ctx, this);
    std::vector<RichLabel::Target> targets;
    uint32_t                       link = 0;
    if (m.user != model::kNoUser && !bot) {
        targets.push_back({mrkdwn::Kind::User, st.user(m.user).id});
        link = 1;
    }
    nameL->setContent(styled(name, Font::BodyBold, C::Text, link), std::move(targets), {});
    nameL->setMaxLines(1);
    // Tags after the name: "APP" for bots, "EXT" for Slack Connect
    // users; the time 8 px after the last.
    if (bot)
        addTagBadge(hdr, false)->style().margins(6, 0, 0, 0);
    if (m.user != model::kNoUser && st.user(m.user).stranger)
        addTagBadge(hdr, true)->style().margins(6, 0, 0, 0);
    hdr->add<ui::Label>(base::formatTime(model::tsSecs(m.ts)), Font::Small, C::TextFaint)
        ->style()
        .margins(8, 0, 0, 0);
}

void MessageRow::applyBanners(const model::Message &m, bool grouped) {
    const Store &st = _list.ctx().store;
    // The banners stack above the message, before its padding.
    _pinText.clear();
    _savedText.clear();
    if (m.pinned)
        _pinText = m.pinnedBy != model::kNoUser
                       ? arg(tr("Pinned by %1"), st.user(m.pinnedBy).label())
                       : std::string(tr("Pinned"));
    const int64_t due = st.reminderAt(_list.conversation(), m.ts);
    _reminded         = due != 0;
    if (_reminded && due > base::nowSecs())
        _dueAt = due; // the strip turns "past due" then
    if (_reminded)
        _savedText = due <= base::nowSecs()
                         ? std::string(tr("Reminder \xE2\x80\x94 past due"))
                         : arg(tr("Reminder \xE2\x80\x94 %1"), base::formatDateTime(due));
    else if (m.saved)
        _savedText = tr("Saved for later");
    const float banners = kBannerH * float(!_pinText.empty() + !_savedText.empty());
    style()
        .row()
        .padding(
            kPadH,
            banners + (grouped ? kPadVGrouped : kPadV),
            kPadH,
            grouped ? kPadVGrouped : kPadVBottom
        )
        .spacing(kAvGap)
        .items(Align::Start);
}

void MessageRow::buildMessage(const model::Message &m, bool grouped) {
    applyBanners(m, grouped);
    if (grouped) {
        // The gutter shows this message's time on hover (painted by the row).
        add<ui::View>()->style().size(kAvSize, 1);
        _hoverTime = base::formatTime(model::tsSecs(m.ts));
    } else {
        ui::View *av = m.isHuddle() ? add<HuddleTile>() : addAvatar(this, m);
        av->style().size(kAvSize, kAvSize).margins(0, 2, 0, 0).noShrink();
    }
    auto *col = add<ui::View>();
    col->style().flex(1).spacing(0);
    if (!grouped)
        buildHeader(col, m, false);
    buildContent(col, m, true);
    _col = col;
    rememberBody(m);
    buildTail(col, m);
}

void MessageRow::buildTail(ui::View *col, const model::Message &m) {
    if (!m.reactions.empty())
        buildReactions(col, m);
    if (m.replyCount > 0 && !(_list.threadMode() && m.ts == _list.threadRoot())) {
        buildThreadSummary(col, m);
        if (_list.inlineOpen(m.ts))
            buildInlineThread(col, m);
    }
}

ui::View *MessageRow::addAvatar(ui::View *parent, const model::Message &m) {
    Context           &ctx    = _list.ctx();
    const Store       &st     = ctx.store;
    const std::string &avatar = authorAvatar(st, m);
    if (m.user != model::kNoUser && !isBot(st, m))
        return parent->add<AuthorAvatar>(ctx, avatar, m.user);
    return parent->add<LetterAvatar>(ctx.images, avatar, authorName(st, m));
}

void MessageRow::buildContent(ui::View *col, const model::Message &m, bool root) {
    Context    &ctx  = _list.ctx();
    const auto &x    = m.extra;
    auto       *body = col->add<ui::View>();
    body->style().spacing(2).margins(0, root && _kind != kRowGrouped ? 1 : 0, 0, 0);
    // The root's text is what the list selects across (not inline replies').
    std::vector<SelectableText *> *labels = root ? &_sel : nullptr;
    if (m.isHuddle()) {
        // A huddle row: one sentence in text.secondary, never "(edited)"
        // (Slack edits every huddle message when it ends).
        body->add<ui::Label>(huddleSummaryText(ctx.store, x->huddle), Font::Body, C::TextMuted);
    } else if (x && !x->blocks.empty()) {
        buildBlocks(body, x->blocks.data(), x->blocks.size(), m.ts, -1, m.edited, labels);
    } else {
        RichOptions o;
        o.edited = m.edited;
        o.labels = labels;
        buildBody(ctx, body, m.text, o, this);
    }
    buildButtons(col, m, 0);
    buildFilesSection(col, m.files(), root ? m.ts : 0);
    for (size_t i = 0; i < m.attachments().size(); ++i)
        if (!_list.attachmentHidden(m, i))
            buildAttachment(col, m, i, root);
    if (root) { // the row's tail (buildTail) follows
        _tailAt = col->childCount();
        return;
    }
    if (!m.reactions.empty())
        buildReactions(col, m);
}

void MessageRow::buildButtons(ui::View *col, const model::Message &m, int32_t owner) {
    if (!m.extra)
        return;
    // The button row: floats that wrap between buttons, 8 px apart,
    // 4 px between rows, 4 px above and 2 below (in a card, its spacing).
    FlowRow *row = nullptr;
    for (const model::Button &b : m.extra->buttons) {
        if (b.attachment != owner)
            continue;
        if (!row) {
            row = col->add<FlowRow>(8);
            if (owner == 0)
                row->style().margins(0, 4 + 4, 0, 2 + 4);
            else
                row->style().margins(0, 2, 0, 2);
        }
        row->add<BotButton>(_list, m.ts, b);
    }
}

void MessageRow::buildBlocks(
    ui::View                      *col,
    const model::Block            *blocks,
    size_t                         count,
    Ts                             ts,
    int                            attachment,
    bool                           edited,
    std::vector<SelectableText *> *labels
) {
    // Text blocks as paragraphs (2 px apart), a header
    // 1.1x bold, a divider hairline, an image under its title (the title
    // folds it away), a data table.
    Context &ctx = _list.ctx();
    using K      = model::Block::Kind;
    for (size_t i = 0; i < count; ++i) {
        const model::Block &b = blocks[i];
        switch (b.kind) {
        case K::Text:
        case K::Header: {
            auto *w = col->add<ui::View>();
            w->style().spacing(2).margins(0, 2, 0, 2);
            RichOptions o;
            o.labels = labels;
            if (b.kind == K::Header) {
                o.font  = Font::BodyBold;
                o.scale = 1.1f;
            }
            buildBody(ctx, w, b.text, o, this);
            break;
        }
        case K::Divider:
            col->add<ui::Separator>()->style().margins(0, 4, 0, 4);
            break;
        case K::Image: {
            if (b.image.empty()) {
                text::Style st = ui::font(Font::Body, C::TextFaint);
                st.italic      = true;
                ui::styledLabel(col, b.alt, st)->style().margins(0, 1, 0, 1);
                break;
            }
            const bool collapsed = _list.imageCollapsed(ts, attachment, int(i));
            if (!b.text.empty()) {
                auto *t = col->add<ui::Clickable>();
                t->style()
                    .row()
                    .spacing(4)
                    .items(Align::Center)
                    .alignSelf(Align::Start)
                    .margins(0, 2, 0, 2);
                ui::styledLabel(
                    t,
                    b.text,
                    ui::pxFont(15 * 0.9f, text::Weight::Regular, ui::themed(C::TextMuted))
                )
                    ->setHitTransparent(true);
                t->add<ui::IconView>(
                     collapsed ? gfx::Icon::ChevronRight : gfx::Icon::ChevronDown, 10, C::TextMuted
                )
                    ->setHitTransparent(true);
                const int index = int(i);
                t->onClick      = [this, ts, attachment, index] {
                    _list.toggleImage(ts, attachment, index);
                };
            }
            if (!collapsed)
                addThumb(col, b.image, b.width, b.height, kImgMaxW, kImgMaxH, 2);
            break;
        }
        case K::Table: {
            auto *t = col->add<TableView>(ctx, b.rows);
            t->style().alignSelf(Align::Start);
            if (labels) {
                t->setSelectable(true);
                labels->push_back(t);
            }
            break;
        }
        }
    }
    if (edited) {
        RichOptions o;
        o.edited = true;
        o.labels = labels;
        buildBody(ctx, col, {}, o, this);
    }
}

ui::View *MessageRow::addThumb(
    ui::View          *col,
    const std::string &path,
    int                w,
    int                h,
    int                maxW,
    int                maxH,
    float              gapAbove,
    bool               file
) {
    Context &ctx = _list.ctx();
    if (w <= 0 || h <= 0)
        ctx.images.naturalSize(path, &w, &h);
    float       tw = w > 0 ? float(w) : float(maxW), th = h > 0 ? float(h) : float(maxH) * 0.66f;
    const float s = std::min({1.f, float(maxW) / tw, float(maxH) / th});
    tw            = std::floor(tw * s);
    th            = std::floor(th * s);
    auto *img     = col->add<Thumb>(_list, path, w, h, tw, th, isGifPath(path), _ts, file);
    img->style().alignSelf(Align::Start).margins(0, gapAbove, 0, 2);
    return img;
}

void MessageRow::buildGallery(ui::View *col, const std::vector<const model::File *> &files, Ts ts) {
    auto *g = col->add<Gallery>();
    g->style().alignSelf(Align::Start).margins(0, 6, 0, 2);
    for (const model::File *f : files) {
        const std::string &src = f->isImage() ? f->path : f->thumb;
        auto              *t   = g->add<Thumb>(
            _list,
            src,
            f->width,
            f->height,
            Gallery::kTileH,
            Gallery::kTileH,
            isGifPath(src),
            ts,
            true
        );
        t->setShape(ImageCache::Shape::Rounded, 8);
        t->setTile();
        const Ts          msg  = _ts;
        const std::string path = f->path;
        t->key                 = path;
        t->onOpen              = [this, msg, path] { _list.openFileViewer(msg, path); };
    }
}

void MessageRow::buildFile(ui::View *col, const model::File &f, Ts ts) {
    if (f.isCanvas() && !f.id.empty()) {
        col->add<CanvasCard>(_list, f)->style().alignSelf(Align::Start).margins(0, 6, 0, 2);
        return;
    }
    if (f.hasPreview()) {
        // The name in a 14-px band right above the picture, 0.82× in
        // fileNameDim, nothing else (the file bar does the rest). A PDF
        // shows its first page; the viewer opens on the file either way.
        const text::Style st =
            ui::pxFont(15 * 0.82f, text::Weight::Regular, ui::color(C::FileNameDim));
        auto *name = ui::styledLabel(col, f.name, st, 1);
        name->style().height(14).margins(0, 6, 0, 0);
        name->setLineHeight(1.0f);
        auto             *img  = static_cast<Thumb *>(addThumb(
            col, f.isImage() ? f.path : f.thumb, f.width, f.height, kImgMaxW, kImgMaxH, 0, true
        ));
        const Ts          msg  = _ts;
        const std::string path = f.path;
        img->key               = path;
        img->setTs(ts);
        img->onOpen = [this, msg, path] { _list.openFileViewer(msg, path); };
        return;
    }
    if (f.isAudio()) { // the audio card: the player (and the transcript line)
        addAudioCard(col, _list.ctx(), ts ? &_list : nullptr, ts, f)
            ->style()
            .alignSelf(Align::Start)
            .margins(0, 6, 0, 0);
        return;
    }
    auto *chip = col->add<FileChip>(ts ? &_list : nullptr, ts, f);
    chip->style().alignSelf(Align::Start).margins(0, 6, 0, 0);
    const model::File file = f;
    // A chip click: an HTML file renders in the browser (fetched to a
    // local copy), anything else opens its Slack page (else the file).
    chip->onClick          = [this, file] {
        if (file.isHtml() && !file.path.empty())
            _list.openHtmlFile(file);
        else if (_list.ctx().openUrl) {
            const std::string &url = file.permalink.empty() ? file.path : file.permalink;
            if (!url.empty())
                _list.ctx().openUrl(fileUrl(url));
        }
    };
}

void MessageRow::buildFilesSection(ui::View *col, const std::vector<model::File> &files, Ts ts) {
    std::vector<const model::File *> previews;
    for (const model::File &f : files)
        if (isInlinePreview(f))
            previews.push_back(&f);
    if (previews.size() >= 2)
        buildGallery(col, previews, ts);
    else if (previews.size() == 1)
        buildFile(col, *previews[0], ts);
    for (const model::File &f : files)
        if (!isInlinePreview(f))
            buildFile(col, f, ts);
}

bool MessageRow::dismissable(int index) const {
    // Dismissable: link previews (a shared message too), not
    // table messages.
    const model::Message *m = _list.message(_ts);
    if (!m || index < 0 || size_t(index) >= m->attachments().size() || m->pending)
        return false;
    const model::Attachment &a = m->attachments()[size_t(index)];
    return (a.linkPreview || a.msgUnfurl) && !onlyBlocks(a, model::Block::Kind::Table);
}

void MessageRow::buildAttachment(ui::View *col, const model::Message &m, size_t index, bool root) {
    Context                 &ctx = _list.ctx();
    const model::Attachment &a   = m.attachments()[index];
    if (a.msgUnfurl) {
        buildUnfurl(col, m, root ? index : SIZE_MAX);
        buildButtons(col, m, int32_t(index + 1));
        return;
    }
    if (!a.pretext.empty()) {
        auto *pre = col->add<ui::View>();
        pre->style().margins(0, 4, 0, 0);
        buildBody(ctx, pre, a.pretext, {}, this);
    }
    auto *card = col->add<AttachCard>(this, root ? int(index) : -1);
    card->style().row().spacing(10).margins(0, 6, 0, 2).alignSelf(Align::Start);
    card->style().maxW = 560;
    // GIF-picker images and table messages draw bar-less, like Slack.
    const bool barless =
        onlyBlocks(a, model::Block::Kind::Image) || onlyBlocks(a, model::Block::Kind::Table);
    if (!barless)
        card->add<ColorBar>(barColor(a.color));
    auto *c = card->add<ui::View>();
    c->style().flex(1).spacing(3).padding(0, 2);
    if (!a.service.empty() || !a.favicon.empty()) {
        auto *svc = c->add<ui::View>();
        svc->style().row().spacing(6).items(Align::Center);
        if (!a.favicon.empty())
            svc->add<CachedImage>(ctx.images, a.favicon, ImageCache::Shape::Rounded, 3)
                ->style()
                .size(16, 16);
        if (!a.service.empty())
            svc->add<ui::Label>(a.service, Font::SmallBold, C::Text);
    }
    if (!a.author.empty())
        c->add<ui::Label>(a.author, Font::BodyBold, C::Text);
    if (!a.title.empty() && a.link.empty()) {
        // A title without its own link is mrkdwn (dates, links), as Slack
        // draws it: Outlook Calendar's "<!date^…> - <!date^…> <url|Event>".
        RichOptions o;
        o.font = Font::BodyBold;
        buildBody(ctx, c, a.title, o, this);
    } else if (!a.title.empty()) {
        auto                          *t = c->add<RichLabel>(ctx, this);
        std::vector<RichLabel::Target> tg;
        uint32_t                       link = 0;
        if (!a.link.empty()) {
            tg.push_back({mrkdwn::Kind::Link, a.link});
            link = 1;
        }
        t->setContent(
            styled(a.title, Font::BodyBold, a.link.empty() ? C::Text : C::Link, link),
            std::move(tg),
            {}
        );
    }
    if (!a.text.empty()) {
        // A bot card's text: 5 lines / 700 characters until "Show more".
        RichOptions o;
        o.maxLines          = a.linkPreview ? 3 : 0;
        const int  key      = int(index);
        const bool expanded = root && _list.unfurlExpanded(m.ts, key);
        int        chars = 700, lines = 5;
        if (root && !a.linkPreview && !expanded)
            o.cutChars = &chars, o.cutLines = &lines;
        if (buildBody(ctx, c, a.text, o, this) != UINT32_MAX || expanded) {
            auto *more = c->add<RichLabel>(ctx, this);
            more->setContent(
                styled(expanded ? tr("Show less") : tr("Show more"), Font::Body, C::Link, 1),
                {{mrkdwn::Kind::Link, {}}},
                {}
            );
            more->onLink = [this, ts = m.ts, key](uint32_t) { _list.toggleUnfurl(ts, key); };
        }
    }
    for (const model::AttachmentField &fl : a.fields) {
        c->add<ui::Label>(fl.title, Font::SmallBold, C::Text)->style().margins(0, 4, 0, 0);
        buildBody(ctx, c, fl.value, {}, this);
    }
    // An attachment's blocks are drawn when nothing above said anything.
    if (a.title.empty() && a.text.empty() && a.fields.empty() && a.author.empty() &&
        !a.blocks.empty())
        buildBlocks(c, a.blocks.data(), a.blocks.size(), m.ts, int(index), false, nullptr);
    if (!a.image.empty())
        addThumb(c, a.image, a.imageWidth, a.imageHeight, int(kImgMax), 240);
    if (!a.footer.empty())
        c->add<ui::Label>(a.footer, Font::Caption, C::TextFaint);
    buildButtons(c, m, int32_t(index + 1));
}

void MessageRow::buildUnfurl(ui::View *col, const model::Message &m, size_t index) {
    // The shared-message card: the
    // quoted author's avatar, name, APP tag and time, "Posted in #channel",
    // the quoted text (400 characters / 6 lines until "Show more"), its
    // files as chips; framed in the attachment card colours.
    Context                 &ctx  = _list.ctx();
    const model::Attachment &a    = m.attachments()[index == SIZE_MAX ? 0 : index];
    auto                    *card = col->add<AttachCard>(this, index == SIZE_MAX ? -1 : int(index));
    card->style().margins(0, 4, 0, 2).padding(10).spacing(0);
    card->setBackground(C::FileChipBg, 8);
    card->setBorder(C::FileChipBorder);
    auto *hdr = card->add<ui::View>();
    hdr->style().row().spacing(kAvGap).items(Align::Start);
    const std::string name =
        a.author.empty() ? std::string(tr("Unknown user")) : mrkdwn::decodeEntities(a.author);
    hdr->add<LetterAvatar>(ctx.images, a.authorIcon, name)
        ->style()
        .size(kAvSize, kAvSize)
        .noShrink();
    auto *lines = hdr->add<ui::View>();
    lines->style().flex(1).spacing(2);
    auto *top = lines->add<ui::View>();
    top->style().row().items(Align::Center);
    auto *nameL = top->add<ui::Label>(name, Font::BodyBold, C::Text);
    nameL->setMaxLines(1);
    nameL->style().shrink = 1;
    if (a.app)
        addTagBadge(top, false)->style().margins(6, 0, 0, 0);
    if (a.ts)
        top->add<ui::Label>(base::formatTime(model::tsSecs(a.ts)), Font::Small, C::TextFaint)
            ->style()
            .margins(8, 0, 0, 0);
    if (const std::string place = placeLabel(ctx.store, a.channel); !place.empty()) {
        // A thread reply opens in its thread (Slack's from_url has its thread_ts).
        const mrkdwn::MessageRef ref    = mrkdwn::parseMessageLink(a.link);
        const Ts                 thread = ref.conv == a.channel ? model::parseTs(ref.threadTs) : 0;

        // Jumps to the quoted message, same as a permalink link (rich.cpp).
        // Muted, underlines on hover like Slack's own client --
        // Kind::Link targets already get that for free (RichLabel::hoverLink).
        auto *posted = lines->add<RichLabel>(ctx, this);
        posted->setContent(
            styled(arg(tr("Posted in %1"), place), Font::Small, C::TextMuted, 1),
            {{mrkdwn::Kind::Link, {}}},
            {}
        );
        posted->onLink = [this, channel = a.channel, ts = a.ts, thread](uint32_t) {
            const model::ConvRef c = _list.ctx().store().findConversation(channel);
            if (c != model::kNoConv && _list.ctx().openMessage)
                _list.ctx().openMessage(c, ts, thread);
        };
        posted->setMaxLines(1);
    }
    auto *body = card->add<ui::View>();
    body->style().spacing(0).margins(0, 6, 0, 0);
    const int  key      = index == SIZE_MAX ? -1 : int(index);
    const bool expanded = key >= 0 && _list.unfurlExpanded(m.ts, key);
    int        chars = expanded ? INT32_MAX : 400, lineBudget = expanded ? INT32_MAX : 6;
    bool       truncated = false;
    auto       addText   = [&](const std::string &t, RichOptions o) {
        o.cutChars = &chars;
        o.cutLines = &lineBudget;
        auto *w    = body->add<ui::View>();
        w->style().margins(0, 2, 0, 2);
        truncated = buildBody(ctx, w, t, o, this) != UINT32_MAX || truncated;
    };
    if (!a.blocks.empty()) {
        for (size_t b = 0; b < a.blocks.size() && !truncated; ++b) {
            const model::Block &blk = a.blocks[b];
            if ((blk.kind == model::Block::Kind::Text || blk.kind == model::Block::Kind::Header) &&
                !blk.text.empty()) {
                RichOptions o;
                if (blk.kind == model::Block::Kind::Header)
                    o.font = Font::BodyBold, o.scale = 1.1f;
                addText(blk.text, o);
            } else {
                buildBlocks(body, &blk, 1, m.ts, key, false, nullptr); // no copy of it
            }
        }
    } else if (!a.text.empty()) {
        addText(a.text, {});
    }
    if ((truncated || expanded) && key >= 0) {
        auto *more = body->add<RichLabel>(ctx, this);
        more->setContent(
            styled(expanded ? tr("Show less") : tr("Show more"), Font::BodyBold, C::Link, 1),
            {{mrkdwn::Kind::Link, {}}},
            {}
        );
        more->onLink = [this, ts = m.ts, key](uint32_t) { _list.toggleUnfurl(ts, key); };
        more->style().margins(0, 2, 0, 0);
    }
    buildFilesSection(card, a.files, 0);
}

void MessageRow::buildReactions(ui::View *col, const model::Message &m) {
    Context     &ctx  = _list.ctx();
    const Store &st   = ctx.store;
    auto        *flow = col->add<FlowRow>(4);
    flow->style().margins(0, kReactGap, 0, 2);
    const ConvRef conv = _list.conversation();
    const Ts      ts   = m.ts;
    for (const model::Reaction &r : m.reactions) {
        const bool mine = st.reactedByMe(r);
        auto      *pill = flow->add<ui::Clickable>();
        pill->setLook(
            {mine ? C::MentionBg : C::Hover,
             mine ? C::MentionBg : C::Pressed,
             C::Pressed,
             C::None,
             12}
        );
        if (mine) {
            pill->setBackground(C::None, 12);
            pill->setBorder(C::Link);
        }
        pill->style().row().height(24).padding(8, 0).spacing(4).items(Align::Center);
        const Store::EmojiGlyph g = st.emojiFor(r.name);
        if (!g.image.empty()) {
            // Animated custom emoji play here too (Settings → Animate emoji).
            auto *img = pill->add<CachedImage>(ctx.images, g.image);
            img->setAnimated(true, true);
            img->setPlaceholder(C::None);
            img->style().size(16, 16);
        } else {
            pill->add<ui::Label>(g.unicode.empty() ? ":" + r.name + ":" : g.unicode, Font::Body);
        }
        pill->add<ui::Label>(
            str::number(r.count), Font::SmallBold, mine ? C::MentionText : C::TextMuted
        );
        std::string who;
        for (size_t i = 0; i < r.users.size() && i < 5; ++i) {
            const std::string_view u =
                r.users[i] == st.me ? std::string_view(tr("You")) : st.user(r.users[i]).label();
            who = !i                        ? std::string(u)
                  : i + 1 == r.users.size() ? arg(tr("%1 and %2"), who, u)
                                            : arg(tr("%1, %2"), who, u);
        }
        pill->setTooltip(arg(tr("%1 reacted with :%2:"), who, r.name));
        const std::string name = r.name;
        pill->onClick = [&ctx, conv, ts, name, mine] { ctx.backend.react(conv, ts, name, !mine); };
    }
    // No "+" pill after the chips: reactions are added from the
    // toolbar's Add reaction.
}

void MessageRow::buildThreadSummary(ui::View *col, const model::Message &m) {
    auto *bar = col->add<ReplyBar>(_list, m);
    bar->style().alignSelf(Align::Start).margins(0, 6, 0, 0);
    const Ts ts  = m.ts;
    bar->onClick = [this, ts] { _list.replyBarClicked(ts); };
}

void MessageRow::buildInlineThread(ui::View *col, const model::Message &root) {
    // The inline thread (Appearance → Threads: Inline): the replies under
    // the bar, avatars in the root's text column; "Loading replies…" until
    // they are in; "Reply to thread" opens the panel.
    Context     &ctx = _list.ctx();
    const Store &st  = ctx.store;
    auto        *box = col->add<ui::View>();
    box->style().spacing(0).margins(0, 8, 0, 0);
    const std::vector<model::Message> *replies = st.replies(_list.conversation(), root.ts);
    if (!replies || replies->empty()) {
        auto *l = ui::styledLabel(
            box,
            tr("Loading replies\xE2\x80\xA6"),
            ui::pxFont(15 * 0.9f, text::Weight::Regular, ui::themed(C::TextMuted))
        );
        l->style().height(28);
    } else {
        const model::Message *prev = nullptr;
        for (const model::Message &r : *replies) {
            // Collapsed as in the list: same author within five minutes.
            const bool grouped = prev && groupable(*prev, r);
            prev               = &r;
            auto *rr           = box->add<ui::View>();
            rr->style()
                .row()
                .spacing(kAvGap)
                .items(Align::Start)
                .padding(
                    0, grouped ? kPadVGrouped : kPadV, 0, grouped ? kPadVGrouped : kPadVBottom
                );
            if (grouped)
                rr->add<ui::View>()->style().size(kAvSize, 1).noShrink();
            else
                addAvatar(rr, r)->style().size(kAvSize, kAvSize).margins(0, 2, 0, 0).noShrink();
            auto *rc = rr->add<ui::View>();
            rc->style().flex(1).spacing(0);
            if (!grouped)
                buildHeader(rc, r, false);
            buildContent(rc, r, false);
        }
    }
    auto    *foot = box->add<FooterLink>(tr("Reply to thread"));
    const Ts ts   = root.ts;
    foot->onClick = [this, ts] { _list.reply(ts); };
}

void MessageRow::attachHovered(int index, ui::View *card, bool on) {
    if (on) {
        if (!dismissable(index))
            return;
        _attach     = index;
        _attachCard = card;
        update();
        return;
    }
    if (_attachCard != card)
        return;
    // Leaving the card for its "×" keeps it.
    if (window() && dismissReach().contains(mapFromWindow(window()->pointerPos())))
        return;
    _attach     = -1;
    _attachCard = nullptr;
    if (_overDismiss) {
        _overDismiss = false;
        if (window())
            window()->rearmTooltip();
    }
    update();
}

ui::RectF MessageRow::dismissRect() const {
    if (_attach < 0 || !_attachCard || !_attachCard->window())
        return {};
    const ui::RectF  r = _attachCard->windowRect();
    const ui::PointF o = mapFromWindow({r.x, r.y});
    // The dismiss button: 18 px, 4 px left of the card.
    return {o.x - 4 - 18, o.y, 18, 18};
}

ui::RectF MessageRow::dismissReach() const {
    const ui::RectF r = dismissRect();
    if (r.w <= 0)
        return {};
    return {r.x, r.y, r.w + 4, std::max(r.h, _attachCard->height())};
}

void MessageRow::paint(gfx::Painter &p) {
    // Pending sends: everything in the row (children included) at half alpha.
    if (_pending)
        p.setOpacity(0.5f);
    if (_reminded) // a reminder tints the whole row
        p.fillRect(bounds(), bannerColor(2));
    if (_list.flashing(_ts))
        p.fillRect(bounds(), ui::color(C::MentionBg));
    else if (hovered() || _list.toolbarRow() == this)
        p.fillRect(bounds(), ui::color(C::SurfaceHover));
    const float scale  = windowScale();
    auto        banner = [&](std::unique_ptr<text::Layout> &l,
                             const std::string             &s,
                             gfx::Icon                      icon,
                             gfx::Color                     c,
                             float                          y) {
        if (!l) { // one line (a name, a date)
            text::Style st = ui::font(Font::Caption);
            st.color       = c;
            l              = text::layoutPlain(s, st, scale);
        }
        gfx::drawIcon(p, icon, {kPadH, y + (kBannerH - 12) / 2, 12, 12}, c);
        l->paint(p, snapPx({kPadH + 16, y + std::floor((kBannerH - l->height()) / 2)}));
    };
    float by = 0;
    if (!_pinText.empty()) {
        p.fillRect({0, 0, width(), kBannerH}, bannerColor(0));
        banner(_pinLayout, _pinText, gfx::Icon::Pin, bannerColor(1), 0);
        by = kBannerH;
    }
    if (!_savedText.empty())
        banner(
            _savedLayout,
            _savedText,
            _reminded ? gfx::Icon::AlarmClock : gfx::Icon::Bookmark,
            bannerColor(3),
            by
        );
    by += _savedText.empty() ? 0 : kBannerH;
    if (!_hoverTime.empty() && (hovered() || _list.toolbarRow() == this)) {
        if (!_hoverLayout) // a time: one line
            _hoverLayout =
                text::layoutPlain(_hoverTime, ui::font(Font::Caption, C::TextFaint), windowScale());
        const float x = kPadH + kAvSize - _hoverLayout->width();
        _hoverLayout->paint(p, snapPx({std::floor(x), by + kPadVGrouped + 3}));
    }
}

void MessageRow::paintOver(gfx::Painter &p) {
    // The dismiss "×" (1.15× the app font, message.attachmentDismiss).
    const ui::RectF r = dismissRect();
    if (r.w <= 0)
        return;
    // Shaped once per colour and scale, not on every paint while hovered.
    const gfx::Color c = bannerColor(1);
    const float      k = windowScale();
    if (!_dismissLayout || _dismissColor != c || _dismissScale != k) {
        _dismissLayout =
            text::layoutPlain("\xC3\x97", ui::pxFont(15 * 1.15f, text::Weight::Regular, c), k);
        _dismissColor = c;
        _dismissScale = k;
    }
    const text::Layout *l = _dismissLayout.get();
    l->paint(
        p,
        snapPx(
            {r.x + std::floor((r.w - l->width()) / 2), r.y + std::floor((r.h - l->height()) / 2)}
        )
    );
}

void MessageRow::styleChanged() {
    _dismissLayout.reset(); // the text size is in it
    View::styleChanged();
}

std::string MessageRow::tooltip() const {
    if (!_overDismiss)
        return {};
    const model::Message *m = _list.message(_ts);
    // Own message + server support: removed for everyone; else hidden here.
    return m && _list.removesPreviewServerSide(*m) ? std::string(tr("Remove preview"))
                                                   : std::string(tr("Hide preview"));
}

ui::RectF MessageRow::tooltipAnchor() const {
    const ui::RectF  r = dismissRect();
    const ui::PointF o = mapToWindow({r.x, r.y});
    return {o.x, o.y, r.w, r.h};
}

bool MessageRow::onEvent(ui::Event &e) {
    switch (e.type) {
    case ui::EventType::PointerEnter:
        _list.rowHovered(this, true);
        return false;
    case ui::EventType::PointerLeave:
        _list.rowHovered(this, false);
        if (_attach >= 0 && !(_attachCard && _attachCard->hovered())) {
            _attach      = -1;
            _attachCard  = nullptr;
            _overDismiss = false;
            update();
        }
        return false;
    case ui::EventType::PointerMove: {
        const bool over = dismissRect().contains(e.pos);
        if (_attach >= 0 && !dismissReach().contains(e.pos) &&
            !(_attachCard && _attachCard->hovered())) {
            _attach     = -1;
            _attachCard = nullptr;
            update();
        }
        if (over != _overDismiss) {
            _overDismiss = over;
            setCursor(over ? plat::Cursor::Hand : plat::Cursor(kCursorInherit));
            if (window())
                window()->rearmTooltip();
        }
        return false;
    }
    case ui::EventType::PointerDown:
        if (e.button == plat::Button::Left && dismissRect().contains(e.pos)) {
            _list.dismissAttachment(_ts, size_t(_attach));
            return true;
        }
        return false;
    default:
        return false; // no menu on right click (the toolbar's "…" has it)
    }
}

} // namespace screens
