#include "app/screens/messages/message_list.h"

#include "app/identity.h"
#include "app/model/jobs.h"
#include "app/screens/common/downloads.h"
#include "app/screens/common/file_dialogs.h"
#include "app/screens/common/icon_button.h"
#include "app/screens/common/loading_indicator.h"
#include "app/screens/common/message_rules.h"
#include "app/screens/common/message_text.h"
#include "app/screens/common/remote_images.h"

#include "app/screens/messages/emoji_picker.h"
#include "app/screens/messages/message_dialogs.h"
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/rich.h"
#include "app/screens/messages/rows.h"
#include "app/screens/messages/summary.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/log.h"
#include "base/mime.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "ui/controls.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace screens {

using i18n::arg;
using i18n::tr;
using ui::C;
using ui::Font;
using Kind = MessageList::ItemKind;

namespace {

constexpr float  kTypingH        = 22;
constexpr int    kEdgeDelayMs    = 60;
// Canvas previews kept for cards scrolled back to (the HTML of each).
constexpr size_t kCanvasPreviews = 16;

// The file of a message by its path (rows name files by path), or null.
const model::File *fileAt(const model::Message *m, const std::string &path) {
    if (m)
        for (const model::File &f : m->files())
            if (f.path == path)
                return &f;
    return nullptr;
}

uint64_t keyOf(const MessageList::Item &it) {
    switch (it.kind) {
    case Kind::Day:
        return (1ull << 63) | uint64_t(it.day & 0xffffffff);
    case Kind::Divider:
        return 1ull << 62;
    default:
        return uint64_t(it.ts);
    }
}

// The hover toolbar card.
// A floating action card: four stacked faint shadow
// halos biased a pixel down, surface.raised, a divider.def hairline, radius 8;
// 28-px buttons, 8/6 padding, 4 apart. The message toolbar and the file bar.
class ToolbarCard final : public ui::View {
public:
    ToolbarCard() {
        setPaintOutset(6);
        style().row().padding(8, 6).spacing(4).items(ui::Align::Center);
    }
    void paint(gfx::Painter &p) override {
        const ui::RectF b = bounds();
        for (int i = 4; i >= 1; --i) {
            const float k = float(i);
            p.fillRoundRect(
                {b.x - k, b.y - k, b.w + 2 * k, b.h + 2 * k + 1},
                8 + k,
                gfx::Color(uint32_t(2 + (4 - i) * 2) << 24)
            );
        }
        p.fillRoundRect(b, 8, ui::color(C::FormBg));
        p.strokeRoundRect(b, 8, 1, ui::color(C::FormDivider));
        View::paint(p);
    }
    bool onEvent(ui::Event &e) override { return e.type == ui::EventType::PointerDown; }
};

// Click anywhere or Escape to close.
class ImageViewer final : public ui::Popup {
public:
    ImageViewer(Context &ctx, const std::string &path, int w, int h, ui::SizeF win) {
        setCard(false);
        setAnchor({0, 0, 0, 0}, Place::Over);
        style().size(win.w - 16, win.h - 16).stack().items(ui::Align::Center);
        setCursor(plat::Cursor::Hand);
        if (w <= 0 || h <= 0)
            ctx.images.naturalSize(path, &w, &h);
        const float maxW = (win.w - 16) * 0.9f, maxH = (win.h - 16) * 0.85f;
        const float s   = w > 0 && h > 0 ? std::min({1.f, maxW / float(w), maxH / float(h)}) : 1;
        auto       *img = add<CachedImage>(ctx.images, path, ImageCache::Shape::Rounded, 6);
        img->style().size(
            std::floor(float(w > 0 ? w : 400) * s), std::floor(float(h > 0 ? h : 300) * s)
        );
        img->setAnimated(isGifPath(path));
        img->setPlaceholder(C::None);
        // text.onDarkDim, centred, until it loads.
        img->setLoadingText(tr("Loading image\xE2\x80\xA6"), C::OnDarkDim);
        auto *name =
            add<ui::Label>(std::string(file::baseName(path)), Font::SmallBold, C::TooltipText);
        name->style().alignSelf(ui::Align::End).margins(0, 0, 16, 14);
    }
    void paint(gfx::Painter &p) override {
        p.fillRoundRect(bounds(), 8, gfx::rgba(0x000000, 0xd8));
    }
    bool onEvent(ui::Event &e) override {
        if (e.type == ui::EventType::PointerDown) {
            close();
            return true;
        }
        return Popup::onEvent(e);
    }
};

// The file viewer: the near-opaque viewer backdrop over the whole
// window, a 56-px bar with the file's name (1.05×, onDark) and round 36-px
// icon buttons, the picture centred below it, scaled to fit, never up.
class FileViewer final : public ui::Popup {
public:
    FileViewer(Context &ctx, MessageList *list, Ts ts, const model::File &f)
        : _ctx(ctx), _list(list), _ts(ts), _file(f) {
        setCard(false);
        setAnchor({0, 0, 0, 0}, Place::Fill);
        style().dir = ui::Dir::None;
        _bar        = add<ui::View>();
        _bar->style()
            .row()
            .padding(24, 0, 24 - 8, 0)
            .spacing(ui::metric(ui::M::SpaceXS))
            .items(ui::Align::Center);
        auto *name = ui::styledLabel(
            _bar,
            f.name,
            ui::pxFont(15 * 1.05f, text::Weight::Regular, ui::color(C::TooltipText)),
            1
        );
        name->style().flex(1);
        const model::Message *m       = list ? list->message(ts) : nullptr;
        // A pending message has no actions yet (the file bar's rule).
        const bool            actions = list && m && !m->pending;
        auto                  button  = [&](gfx::Icon icon, const char *tip, bool on) {
            auto *b = _bar->add<ViewerButton>(icon, tr(tip));
            b->setVisible(on);
            return b;
        };
        button(gfx::Icon::Download, N_("Download"), actions)->onClick = [this] {
            _list->downloadFile(_ts, _file.path);
        };
        button(gfx::Icon::Share2, N_("Forward"), actions && bool(ctx.forwardMessage))->onClick =
            [this] {
                close(); // the forward dialog replaces the viewer
                if (_ctx.forwardMessage)
                    _ctx.forwardMessage(_list->conversation(), _ts, _file.path);
            };
        button(
            gfx::Icon::ExternalLink,
            N_("Open in browser"),
            actions && !(f.permalink.empty() && f.path.empty())
        )
            ->onClick = [this] {
            if (_ctx.openUrl)
                _ctx.openUrl(fileUrl(_file.permalink.empty() ? _file.path : _file.permalink));
        };
        auto *more    = button(gfx::Icon::MoreHorizontal, N_("More actions"), actions);
        more->onClick = [this, more] {
            const ui::RectF r = more->windowRect();
            _list->openFileMenu(_ts, _file.path, {r.x, r.y + r.h + 2});
        };
        button(gfx::Icon::X, N_("Close"), true)->onClick = [this] { close(); };
        // The thumbnail at once, the original over it once it is in (an
        // image's; a PDF shows its rendered page only).
        const std::string &shown                         = f.isImage() ? f.path : f.thumb;
        _thumb = add<CachedImage>(ctx.images, shown, ImageCache::Shape::Square);
        _thumb->setPlaceholder(C::None);
        _thumb->setLoadingText(tr("Loading image\xE2\x80\xA6"), C::OnDarkDim);
        _thumb->setAnimated(isGifPath(shown));
        if (f.isImage() && f.source() != f.path) {
            _full = add<CachedImage>(ctx.images, f.source(), ImageCache::Shape::Square);
            _full->setPlaceholder(C::None);
        }
        setFocusable(true);
    }
    void layout() override {
        const float w = width();
        _bar->setFrame({0, 0, w, kBarH});
        const ui::RectF r = imageRect();
        _thumb->setFrame(r);
        if (_full)
            _full->setFrame(r);
    }
    void paint(gfx::Painter &p) override { p.fillRect(bounds(), ui::color(C::ViewerBackdrop)); }
    bool onEvent(ui::Event &e) override {
        if (e.type == ui::EventType::PointerDown) {
            // The backdrop closes it; the picture and the bar don't.
            if (e.button == plat::Button::Left && e.pos.y > kBarH && !imageRect().contains(e.pos))
                close();
            return true;
        }
        return Popup::onEvent(e); // Escape
    }

private:
    static constexpr float kBarH = 56, kMargin = 24;
    // A round button on the dark bar: 20-px onDark icon, a white wash on hover.
    class ViewerButton final : public IconButton {
    public:
        ViewerButton(gfx::Icon icon, std::string tip) : IconButton(icon, 20, C::TooltipText) {
            style().size(36, 36).noShrink();
            setTooltip(std::move(tip));
            setFocusable(false);
            setCursor(plat::Cursor::Hand);
        }
        void paint(gfx::Painter &p) override {
            if (hovered())
                p.fillRoundRect(bounds(), 18, 0x26ffffffU);
            paintIcon(p);
        }
    };
    ui::RectF imageRect() const {
        const ui::RectF avail{
            kMargin, kBarH + kMargin / 2, width() - 2 * kMargin, height() - kBarH - kMargin * 1.5f
        };
        if (avail.w <= 0 || avail.h <= 0)
            return {};
        float w = float(_file.width), h = float(_file.height);
        if (w <= 0 || h <= 0) {
            int nw = 0, nh = 0;
            _ctx.images.naturalSize(_thumb->path(), &nw, &nh);
            w = nw > 0 ? float(nw) : 400, h = nh > 0 ? float(nh) : 300;
        }
        const float s = std::min({1.f, avail.w / w, avail.h / h});
        w             = std::floor(w * s);
        h             = std::floor(h * s);
        return {
            avail.x + std::floor((avail.w - w) / 2), avail.y + std::floor((avail.h - h) / 2), w, h
        };
    }

    Context     &_ctx;
    MessageList *_list;
    Ts           _ts;
    model::File  _file;
    ui::View    *_bar   = nullptr;
    CachedImage *_thumb = nullptr, *_full = nullptr;
};

// A label's text in window coordinates, not clipped by the list: in a message
// taller than the list its first or last text may lie wholly outside it
// (windowRect() would be empty there).
ui::RectF textRect(SelectableText &l) {
    const ui::View  &v = l.textView();
    const ui::PointF o = v.mapToWindow({0, 0});
    return {o.x, o.y, v.width(), v.height()};
}

// The label under (or, between two, nearest to) a window y, and the base
// offset of its text; null when y is above the first or below the last.
SelectableText *labelAt(const MessageRow &row, float wy, uint32_t *base) {
    const std::vector<SelectableText *> &labels = row.selectionLabels();
    if (labels.empty())
        return nullptr;
    const ui::RectF first = textRect(*labels.front());
    const ui::RectF last  = textRect(*labels.back());
    if (wy < first.y || wy > last.y + last.h)
        return nullptr;
    uint32_t        b    = 0;
    SelectableText *best = nullptr;
    float           dist = 1e9f;
    for (SelectableText *l : labels) {
        const ui::RectF r = textRect(*l);
        const float     d = wy < r.y ? r.y - wy : wy > r.y + r.h ? wy - (r.y + r.h) : 0;
        if (d < dist) {
            dist  = d;
            best  = l;
            *base = b;
        }
        b += l->textSize() + 1;
    }
    return best;
}

} // namespace

namespace {
constexpr size_t kKeptRows = 48; // built rows kept off screen (VirtualList::setKeep)
} // namespace

// ── Adapter ─────────────────────────────────────────────────────────────────

class MessageList::Adapter final : public ui::VirtualList::Adapter {
public:
    explicit Adapter(MessageList &l) : _l(l) {}
    int count() const override { return int(_l._items.size()); }
    int kind(int i) const override {
        const Item &it = _l._items[size_t(i)];
        switch (it.kind) {
        case Kind::Day:
            return kRowDay;
        case Kind::Divider:
            return kRowDivider;
        case Kind::System:
            return kRowSystem;
        default:
            return it.grouped ? kRowGrouped : kRowFull;
        }
    }
    std::unique_ptr<ui::View> create(int k) override {
        if (k == kRowDay)
            return std::make_unique<DayRow>();
        if (k == kRowDivider)
            return std::make_unique<DividerRow>();
        return std::make_unique<MessageRow>(_l, k);
    }
    // A message row is its message's: kept while the list says nothing
    // about it changing (Store updates, users, inline threads, previews).
    uint64_t key(int i) const override {
        const Item &it = _l._items[size_t(i)];
        return it.kind == Kind::Message || it.kind == Kind::System ? uint64_t(it.ts) : 0;
    }
    bool reuse(ui::View &row, int) override { return static_cast<MessageRow &>(row).reuse(); }
    void bind(ui::View &row, int i) override {
        const Item &it = _l._items[size_t(i)];
        if (it.kind == Kind::Day)
            static_cast<DayRow &>(row).setText(_l.itemLabel(size_t(i)));
        else if (it.kind == Kind::Divider)
            static_cast<DividerRow &>(row).setText(_l.itemLabel(size_t(i)));
        else {
            ++_l._rowBinds;
            static_cast<MessageRow &>(row).bind(it);
        }
    }
    float estimateHeight(int i) const override {
        const Item &it = _l._items[size_t(i)];
        switch (it.kind) {
        case Kind::Day:
            return 32;
        case Kind::Divider:
            return 36;
        case Kind::System:
            return 32;
        default:
            break;
        }
        float                 h = it.grouped ? 27 : 58;
        const model::Message *m = _l.message(it.ts);
        if (m) {
            for (const model::File &f : m->files())
                h += f.isImage() ? 260 : 66;
            h += float(m->attachments().size()) * 90;
            if (!m->reactions.empty())
                h += 30;
            if (m->replyCount)
                h += 36;
        }
        return h;
    }

private:
    MessageList &_l;
};

// ── Empty-list states ───────────────────────────────────────────────────────

// The empty viewport: while loading,
// the ring centred in the list and, after 1 / 5 / 15 s, a hint whose top is
// 26 + 24 px under the centre (the app font at 1.15x, text.secondary,
// wrapped, centred between 32-px margins); loaded and empty, "No messages
// yet" centred at 1.5x. Over the (empty) list, transparent to the pointer.
class MessageList::ListState final : public ui::View {
public:
    ListState() : _ring([this] { refresh(); }) {
        setHitTransparent(true);
        style().dir = ui::Dir::None;
        _hint       = add<ui::Label>();
        _hint->setAlign(text::LayoutOptions::Align::Center);
        _empty = ui::styledLabel(this, tr("No messages yet"), font(1.5f), 1);
        _empty->setAlign(text::LayoutOptions::Align::Center);
        _hint->setVisible(false);
        _empty->setVisible(false);
        setVisible(false);
    }
    void set(State s) {
        if (s == _s)
            return;
        const bool was = _s == State::Loading;
        _s             = s;
        if (s == State::Loading && !was) {
            _since = ui::app()->nowMs();
            _ring.start();
        } else if (s != State::Loading) {
            _ring.stop();
        }
        _empty->setVisible(s == State::Empty);
        setVisible(s != State::None);
        refresh();
    }
    State       state() const { return _s; }
    std::string text() const {
        return _s == State::Empty     ? std::string(tr("No messages yet"))
               : _s == State::Loading ? _hintText
                                      : std::string();
    }
    void layout() override {
        const float w = width(), h = height();
        // Top-aligned: the text starts there (a Label centres in its frame).
        const float hw = std::max(0.f, w - 64);
        _hint->setFrame(
            {32,
             std::floor(h / 2) + 26 + 24,
             hw,
             std::min(80.f, std::ceil(_hint->measure(hw, 80).h))}
        );
        const float eh = std::ceil(_empty->measure(w, h).h);
        _empty->setFrame({0, std::floor((h - eh) / 2), w, eh});
    }
    void paint(gfx::Painter &p) override {
        View::paint(p);
        if (_s == State::Loading)
            _ring.paint(p, bounds());
    }

private:
    static text::Style font(float k) {
        return ui::pxFont(15 * k, text::Weight::Regular, ui::themed(C::TextMuted));
    }
    // Every ring step: the hint for how long it has been.
    void refresh() {
        const char *hint = nullptr;
        if (_s == State::Loading) {
            const double ms = ui::app()->nowMs() - _since;
            if (ms >= 15000)
                hint =
                    tr("Oh my gosh, I really apologize, but your company is a reaaaly active "
                       "Slack user. Still loading...");
            else if (ms >= 5000)
                hint = tr("Oh, you must have a lot of co-workers and messages! Still loading...");
            else if (ms >= 1000)
                hint = tr("Loading your stuff...");
        }
        const std::string next = hint ? hint : "";
        if (next != _hintText) {
            _hintText = next;
            text::AttributedText t;
            t.append(_hintText, font(1.15f));
            _hint->setRichText(std::move(t));
            _hint->setVisible(!_hintText.empty());
            layout(); // its height follows the text
        }
        update();
    }

    LoadingIndicator _ring;
    ui::Label       *_hint = nullptr, *_empty = nullptr;
    std::string      _hintText;
    double           _since = 0;
    State            _s     = State::None;
};

// A toolbar button: a 16-px icon in icon.strong, the message.hover wash
// (radius 5, inset 1) under the pointer, its tooltip at once (toolbar tips
// show on mouse move, without the usual delay).
class ActionButton final : public IconButton {
public:
    ActionButton(gfx::Icon icon, std::string tip) : IconButton(icon, 16, C::FormIconStrong) {
        style().size(28, 28).noShrink();
        setTooltip(std::move(tip));
        setFocusable(false);
    }
    bool tooltipImmediate() const override { return true; }
    void paint(gfx::Painter &p) override {
        if (hovered())
            p.fillRoundRect({1, 1, 26, 26}, 5, ui::color(C::RowHover));
        paintIcon(p);
    }
};

// ── MessageList ─────────────────────────────────────────────────────────────

MessageList::MessageList(Context &ctx)
    : _ctx(ctx), _adapter(std::make_unique<Adapter>(*this)), _alive(std::make_shared<char>(0)) {
    _threadsInline = ctx.threadsInline;
    setRole(ui::Role::List);
    setBackground(C::Surface);
    setLayoutBoundary(true);
    style().dir = ui::Dir::None;
    _list       = add<ui::VirtualList>(_adapter.get());
    // The list takes focus on click, so PageUp/PageDown
    // and the arrows scroll it from then on.
    _list->setClickFocus(true);
    _list->setStickToBottom(true);
    _list->setBottomAligned(true);
    _list->setOverscan(200);
    // Rows scrolled out stay built (parsed, shaped) for scrolling back: a
    // couple of screens' worth; every change to a message re-binds its row.
    _list->setKeep(kKeptRows);
    _list->setBackground(C::Surface); // opaque: scrolling blits instead of repainting
    _state          = add<ListState>();
    _list->onScroll = [this] {
        hideToolbar();
        _fileView = nullptr;
        _fileBar->setVisible(false);
        scheduleEdgeCheck();
    };
    _typing = add<ui::Label>("", Font::Small, C::TextMuted);
    _typing->setMaxLines(1);
    // The hover toolbar: Add reaction, Forward message, Save for later, Ask
    // agent (agent thread links), More actions.
    _toolbar          = add<ToolbarCard>();
    _tbEmoji          = _toolbar->add<ActionButton>(gfx::Icon::Smile, tr("Add reaction"));
    _tbEmoji->onClick = [this] {
        if (_toolbarRow) {
            const ui::RectF r = _tbEmoji->windowRect();
            openReactionPicker(_toolbarRow->ts(), {r.x, r.y + r.h, 0, 0});
        }
    };
    _tbForward          = _toolbar->add<ActionButton>(gfx::Icon::Forward, tr("Forward message"));
    _tbForward->onClick = [this] {
        if (_toolbarRow && _ctx.forwardMessage)
            _ctx.forwardMessage(_conv, _toolbarRow->ts(), {});
    };
    _tbSave          = _toolbar->add<ActionButton>(gfx::Icon::Bookmark, tr("Save for later"));
    _tbSave->onClick = [this] {
        if (_toolbarRow)
            toggleSaved(_toolbarRow->ts());
    };
    _tbAgent          = _toolbar->add<ActionButton>(gfx::Icon::Bot, tr("Ask agent"));
    _tbAgent->onClick = [this] {
        if (_toolbarRow && _ctx.agentLinkAction)
            _ctx.agentLinkAction(_conv, _toolbarRow->ts(), Context::AgentLinkAction::Ask);
    };
    _tbMore          = _toolbar->add<ActionButton>(gfx::Icon::MoreHorizontal, tr("More actions"));
    _tbMore->onClick = [this] {
        if (_toolbarRow) {
            const ui::RectF r = _tbMore->windowRect();
            openMenu(_toolbarRow->ts(), {r.x, r.y + r.h});
        }
    };
    _toolbar->setVisible(false);
    // The file action bar: Download, Share, More actions.
    _fileBar          = add<ToolbarCard>();
    auto *download    = _fileBar->add<ActionButton>(gfx::Icon::Download, tr("Download"));
    download->onClick = [this] { downloadFile(_fileTs, _filePath); };
    auto *share       = _fileBar->add<ActionButton>(gfx::Icon::Share2, tr("Share"));
    share->onClick    = [this] {
        if (_ctx.forwardMessage && _fileTs)
            _ctx.forwardMessage(_conv, _fileTs, _filePath);
    };
    auto *fileMore    = _fileBar->add<ActionButton>(gfx::Icon::MoreHorizontal, tr("More actions"));
    fileMore->onClick = [this, fileMore] {
        const ui::RectF r = fileMore->windowRect();
        openFileMenu(_fileTs, _filePath, {r.x, r.y + r.h});
    };
    _fileBar->setVisible(false);
}

void MessageList::toggleSaved(Ts ts) {
    // The Save button: a toggle. Saved (a bookmark or a reminder) →
    // removeMessageReminder drops the whole saved item; else a bookmark.
    const model::Message *m = message(ts);
    if (!m || isSystem(*m))
        return;
    if (_ctx.store().reminderAt(_conv, ts))
        _ctx.backend.setReminder(_conv, ts, 0);
    else
        _ctx.backend.setSaved(_conv, ts, !m->saved);
    placeToolbar();
}

void MessageList::downloadFile(Ts ts, const std::string &path) {
    // "Save file" at ~/<name>, then the
    // original (source(): for audio, path is Slack's transcode) to it, the
    // footer's cog running from the choice until the bytes are on disk.
    const model::Message *m = message(ts);
    const model::File    *f = m && !m->pending ? fileAt(m, path) : nullptr;
    if (!f)
        return;
    const std::string name   = f->name.empty() ? std::string(tr("file")) : f->name;
    const std::string source = f->source();
    Context          &ctx    = _ctx; // outlives the list; the download may outlive it
    screens::saveFile(
        _ctx,
        name,
        [&ctx, name, source](std::string to) {
            if (to.empty())
                return;
            const int job = model::jobs().begin(arg(tr("Downloading %1"), name));
            fetchFile(
                ctx.app.platform(),
                ctx.backend,
                source,
                std::move(to),
                [job](bool ok, const std::string &err) {
                    if (!ok)
                        LOG_WARN("messages", "File download failed: %s", err.c_str());
                    model::jobs().end(job);
                }
            );
        },
        _ctx.app.platform().standardDir(plat::StandardDir::Home)
    );
}

void MessageList::fileHovered(ui::View *v, Ts ts, const std::string &path, bool on) {
    if (on) {
        const model::Message *m = message(ts);
        if (!m || m->pending)
            return;
        _fileView = v;
        _fileTs   = ts;
        _filePath = path;
        placeFileBar();
        return;
    }
    std::weak_ptr<char> alive = _alive;
    _ctx.app.platform().post([this, alive, v] {
        if (alive.expired() || _fileView != v)
            return;
        if (!v->hovered() && !_fileBar->hovered()) {
            _fileView = nullptr;
            _fileBar->setVisible(false);
        }
    });
}

void MessageList::placeFileBar() {
    if (!_fileView || !_fileView->window()) {
        _fileBar->setVisible(false);
        return;
    }
    // cardTop = file top − 20 (straddling it); right edge on the file's
    // last pixel column.
    const ui::SizeF  sz = _fileBar->measure(ui::kInf, ui::kInf);
    const ui::RectF  r  = _fileView->windowRect();
    const ui::PointF o  = mapFromWindow({r.x, r.y});
    _fileBar->setFrame({std::floor(o.x + r.w - 1 - sz.w), std::floor(o.y - 20), sz.w, sz.h});
    _fileBar->setVisible(true);
}

MessageList::~MessageList() {
    if (_observer)
        _ctx.store.unobserve(_observer);
    _ctx.app.cancelTimer(_edgeTimer);
    _ctx.app.cancelTimer(_flashTimer);
    _ctx.app.cancelTimer(_usersTimer);
    stopDragScroll();
    _toolbarRow = nullptr;
    clearChildren(); // rows reach back into this object while being destroyed
}

const model::Message *MessageList::message(Ts ts) const {
    if (_conv == model::kNoConv)
        return nullptr;
    const model::Store &st = _ctx.store();
    if (_root && ts != _root)
        if (const std::vector<model::Message> *r = st.replies(_conv, _root)) {
            const auto it =
                std::lower_bound(r->begin(), r->end(), ts, [](const model::Message &m, Ts t) {
                    return m.ts < t;
                });
            if (it != r->end() && it->ts == ts)
                return &*it;
        }
    return st.findMessage(_conv, ts);
}

void MessageList::subscribe() {
    if (_observer)
        _ctx.store.unobserve(_observer);
    _observer = 0;
    if (_conv != model::kNoConv)
        _observer = _ctx.store.observe(_conv, [this](const model::Change &ch) { onChange(ch); });
}

void MessageList::clear() {
    saveAnchor();
    clearSelection();
    if (_observer)
        _ctx.store.unobserve(_observer);
    _observer = 0;
    _conv     = model::kNoConv;
    _root     = 0;
    _jumpTs   = 0;
    hideToolbar();
    _items.clear();
    resetRows();
    updateTyping();
    updateState();
}

void MessageList::showConversation(ConvRef conv) {
    hideToolbar();
    saveAnchor(); // where the one we leave was left
    _selAnchor = _selFocus = {};
    _selDragging           = false;
    stopDragScroll();
    if (conv != _conv)
        _inlineThreads.clear();
    _conv         = conv;
    _root         = 0;
    _markedTs     = 0;
    _jumpTs       = 0;
    _loadingOlder = false;
    // Where to land: where it was left, else its first unread
    // (switching away and back never jumps the view).
    _openPending  = conv != model::kNoConv;
    _openAnchor   = {};
    _openLastRead = 0;
    if (_openPending) {
        const std::string &id = _ctx.store().conversation(conv).id;
        for (const SavedAnchor &a : _anchors)
            if (a.conv == id)
                _openAnchor = a;
        if (_openAnchor.conv.empty())
            _openLastRead = _ctx.store().conversation(conv).lastRead;
    }
    _list->setBottomAligned(true);
    rebuild(false);
    resetRows();
    applyOpenTarget();
    subscribe();
    updateTyping();
    scheduleEdgeCheck();
}

void MessageList::showThread(ConvRef conv, Ts root) {
    hideToolbar();
    saveAnchor();
    _selAnchor = _selFocus = {};
    _selDragging           = false;
    stopDragScroll();
    _openPending   = false;
    _conv          = conv;
    _root          = root;
    _jumpTs        = 0;
    _loadingThread = false;
    // A thread reads from its root down; a short one sits at the top.
    _list->setBottomAligned(false);
    rebuild(false);
    resetRows();
    subscribe();
    updateTyping();
    if (conv != model::kNoConv && !_ctx.store().replies(conv, root)) {
        // The ring shows, not the root, until the thread's page is in.
        _loadingThread = true;
        rebuild(false);
        resetRows();
        std::weak_ptr<char> alive = _alive;
        _ctx.backend.loadThread(conv, root, [this, alive, conv, root](bool, const std::string &) {
            if (alive.expired() || !_loadingThread || conv != _conv || root != _root)
                return;
            _loadingThread = false;
            rebuild(false);
            resetRows();
        });
    }
    updateState();
}

namespace {
const model::Message &deref(const model::Message &m) {
    return m;
}
const model::Message &deref(const model::Message *m) {
    return *m;
}

// base::localDay for times in order: one localtime per day, not one per
// message. The window it trusts keeps an hour clear of both midnights (a DST
// change moves the wall clock by up to an hour).
class DayOf {
public:
    int64_t operator()(int64_t secs) {
        if (secs < _lo || secs >= _hi) {
            const base::CivilTime c    = base::localTime(secs);
            const int64_t         wall = c.hour * 3600 + c.minute * 60 + c.second;
            _day                       = base::daysFromCivil(c.year, c.month, c.day);
            _lo                        = secs - wall + 3600;
            _hi                        = secs - wall + 22 * 3600;
        }
        return _day;
    }

private:
    int64_t _lo = 0, _hi = 0, _day = 0;
};
} // namespace

void MessageList::rebuild(bool notify) {
    std::vector<Item> items;
    if (_conv != model::kNoConv) {
        const model::Conversation &c = _ctx.store().conversation(_conv);
        DayOf                      dayOf;
        auto                       addRun = [&](const auto &msgs, bool days) {
            const model::Message *prev    = nullptr;
            int64_t               prevDay = INT64_MIN;
            for (const auto &e : msgs) {
                const model::Message &m   = deref(e);
                const int64_t         day = dayOf(model::tsSecs(m.ts));
                if (days && day != prevDay) {
                    items.push_back({m.ts, day, Kind::Day, false});
                    prev = nullptr;
                }
                prevDay        = day;
                const bool sys = isSystem(m);
                items.push_back(
                    {m.ts, day, sys ? Kind::System : Kind::Message, prev && groupable(*prev, m)}
                );
                prev = &m;
            }
        };
        if (_root == 0) {
            addRun(c.messages, true);
        } else if (_loadingThread) {
            // Nothing until the replies are in (the loading state).
        } else if (const model::Message *root = _ctx.store().findMessage(_conv, _root)) {
            // The thread view: the root and its replies as one run with day
            // dividers ("Yesterday" above the root), no reply-count row.
            std::vector<const model::Message *> run{root};
            if (const auto *replies = _ctx.store().replies(_conv, _root))
                for (const model::Message &m : *replies)
                    run.push_back(&m);
            addRun(run, true);
        }
    }
    if (!notify) {
        _items = std::move(items);
        updateState();
        return;
    }
    // Diff by identity (message ts, day number): the common prefix and suffix
    // stay, the middle is removed/inserted — which is exactly what keeps the
    // VirtualList anchored for appends, prepends and single inserts.
    const std::vector<Item> &old = _items;
    size_t                   p   = 0;
    while (p < old.size() && p < items.size() && keyOf(old[p]) == keyOf(items[p]))
        ++p;
    size_t s = 0;
    while (s < old.size() - p && s < items.size() - p &&
           keyOf(old[old.size() - 1 - s]) == keyOf(items[items.size() - 1 - s]))
        ++s;
    const size_t     removed = old.size() - p - s, inserted = items.size() - p - s;
    std::vector<int> changed;
    auto             differs = [](const Item &a, const Item &b) {
        return a.kind != b.kind || a.grouped != b.grouped;
    };
    for (size_t i = 0; i < p; ++i)
        if (differs(old[i], items[i]))
            changed.push_back(int(i));
    for (size_t j = 0; j < s; ++j)
        if (differs(old[old.size() - 1 - j], items[items.size() - 1 - j]))
            changed.push_back(int(items.size() - 1 - j));
    _items = std::move(items);
    if (removed)
        _list->itemsRemoved(int(p), int(removed));
    if (inserted)
        _list->itemsInserted(int(p), int(inserted));
    for (int i : changed)
        rowsChanged(i, 1);
    updateState();
}

void MessageList::updateState() {
    State s = State::None;
    if (_items.empty()) {
        bool loading = _waiting;
        if (_conv != model::kNoConv) {
            const model::Conversation &c = _ctx.store().conversation(_conv);
            // Never loaded: no messages yet, but more before them.
            loading = loading || (_root ? _loadingThread : c.messages.empty() && c.hasMoreBefore);
            s       = loading ? State::Loading : State::Empty;
        } else if (loading) {
            s = State::Loading;
        }
    }
    _state->set(s);
}

void MessageList::setWaiting(bool on) {
    if (on == _waiting)
        return;
    _waiting = on;
    updateState();
}

MessageList::State MessageList::state() const {
    return _state->state();
}

std::string MessageList::stateText() const {
    return _state->text();
}

bool MessageList::groupingHolds(size_t i) const {
    const auto at = [this](size_t j) -> const model::Message * {
        return j < _items.size() &&
                       (_items[j].kind == Kind::Message || _items[j].kind == Kind::System)
                   ? message(_items[j].ts)
                   : nullptr;
    };
    const model::Message *m = at(i);
    if (!m || isSystem(*m) != (_items[i].kind == Kind::System))
        return false;
    const model::Message *prev = i > 0 ? at(i - 1) : nullptr;
    if ((prev && groupable(*prev, *m)) != _items[i].grouped)
        return false;
    const model::Message *next = at(i + 1);
    return !next || groupable(*m, *next) == _items[i + 1].grouped;
}

int MessageList::itemIndex(Ts ts) const {
    // Items are in ts order; a day's item shares its first message's ts.
    auto it = std::lower_bound(_items.begin(), _items.end(), ts, [](const Item &a, Ts t) {
        return a.ts < t;
    });
    for (; it != _items.end() && it->ts == ts; ++it)
        if (it->kind == Kind::Message || it->kind == Kind::System)
            return int(it - _items.begin());
    return -1;
}

void MessageList::onChange(const model::Change &ch) {
    using CK = model::ChangeKind;
    switch (ch.kind) {
    case CK::Typing:
        updateTyping();
        return;
    case CK::Meta:
    case CK::Roster:
        return;
    case CK::Users:
        // A burst (a presence round, a users.list page) → one pass.
        if (!_usersTimer)
            _usersTimer = _ctx.app.addTimer(0, false, [this] {
                _usersTimer = 0;
                usersChanged();
            });
        return;
    default:
        break;
    }
    const bool mine = _root == 0 ? ch.thread == 0
                                 : ch.thread == _root || (ch.thread == 0 && ch.ts == _root) ||
                                       ch.kind == CK::Reset;
    // An inline thread's replies live in its root's row.
    if (!mine && _root == 0 && ch.thread && has(_inlineThreads, ch.thread)) {
        rowChanged(ch.thread);
        return;
    }
    if (!mine)
        return;
    if (ch.kind == CK::Reset) {
        hideToolbar();
        rebuild(false);
        resetRows();
    } else if (ch.kind == CK::Update) {
        // Most updates (a reaction, an edit, a poll's same page) leave the
        // items as they are: only the row is bound again.
        // The row keeps its body when only the message changed and that
        // part of it didn't (MessageRow::bind): no rowsChanged() here.
        int i = itemIndex(ch.ts);
        if (i < 0 || !groupingHolds(size_t(i))) {
            rebuild(true);
            i = itemIndex(ch.ts);
        }
        if (i >= 0)
            _list->itemsChanged(i, 1);
    } else {
        rebuild(true);
    }
    applyOpenTarget();
    applyJump();
    scheduleEdgeCheck();
}

namespace {

// What a Users burst changed that rows draw: profiles, and in the text
// change (custom emoji, user groups, the names of channels the roster
// doesn't list) what each looks like in message text.
struct Changed {
    std::vector<model::UserRef>   users;
    // Found in message text: user ids, ":emoji:" codes, user group ids,
    // "<#C…" channel mentions.
    std::vector<std::string_view> needles;
    std::vector<std::string_view> emoji; // custom emoji names (reactions)
};

// Whether message `m` draws any of it: as its author, pinner, thread
// participant, reactor or huddle attendee, or in its text, attachments or
// blocks (mentions, emoji). A quoted message's author may come from the
// Store's linked authors: such unfurls count when users changed.
bool touches(const model::Message &m, const Changed &c) {
    const auto in = [&](model::UserRef u) {
        return u != model::kNoUser && std::find(c.users.begin(), c.users.end(), u) != c.users.end();
    };
    const auto anyIn = [&](const std::vector<model::UserRef> &v) {
        return std::any_of(v.begin(), v.end(), in);
    };
    const auto names = [&](std::string_view text) {
        for (std::string_view n : c.needles)
            if (text.find(n) != std::string_view::npos)
                return true;
        return false;
    };
    const auto blocks = [&](const std::vector<model::Block> &bs) {
        for (const model::Block &b : bs) {
            if (names(b.text))
                return true;
            for (const auto &row : b.rows)
                for (const std::string &cell : row)
                    if (names(cell))
                        return true;
        }
        return false;
    };
    if (in(m.user) || in(m.pinnedBy) || anyIn(m.replyUsers) || names(m.text))
        return true;
    for (const model::Reaction &r : m.reactions) {
        if (anyIn(r.users))
            return true;
        for (std::string_view e : c.emoji) // "name", or "name::skin-tone-N"
            if (str::startsWith(r.name, e) &&
                (r.name.size() == e.size() ||
                 str::startsWith(std::string_view(r.name).substr(e.size()), "::")))
                return true;
    }
    if (!m.extra)
        return false;
    if (anyIn(m.extra->huddle.attendees) || blocks(m.extra->blocks))
        return true;
    if (!c.emoji.empty()) // a canvas card's preview draws emoji too
        for (const model::File &f : m.extra->files)
            if (f.isCanvas())
                return true;
    for (const model::Attachment &a : m.extra->attachments) {
        if ((a.msgUnfurl && !c.users.empty()) || names(a.pretext) || names(a.author) ||
            names(a.title) || names(a.text) || blocks(a.blocks))
            return true;
        for (const model::AttachmentField &f : a.fields)
            if (names(f.title) || names(f.value))
                return true;
    }
    return false;
}

void addOnce(std::vector<std::string> *v, std::string s) {
    if (std::find(v->begin(), v->end(), s) == v->end())
        v->push_back(std::move(s));
}

// What the Store's text changes after revision `since` look like in message
// text: the custom emoji names that changed (with the aliases of them) first
// in *names, *emoji of them, then the user group ids and "<#C…" mentions of
// renamed channels. False when that can't be listed or is too much to look
// for: re-bind everything.
bool textNeedles(
    const model::Store &st, uint64_t since, std::vector<std::string> *names, size_t *emoji
) {
    constexpr size_t                      kMax = 64;
    std::vector<model::Store::TextChange> changes;
    if (!st.textChangesSince(since, &changes))
        return false;
    using Kind = model::Store::TextChange::Kind;
    for (const model::Store::TextChange &t : changes)
        if (t.kind == Kind::Emoji)
            addOnce(names, t.id);
    if (names->size() > kMax)
        return false;
    if (!names->empty())
        for (const auto &[name, value] : st.customEmoji()) // the aliases of them
            if (str::startsWith(value, "alias:") &&
                std::find(names->begin(), names->end(), std::string_view(value).substr(6)) !=
                    names->end())
                addOnce(names, name);
    *emoji = names->size();
    for (const model::Store::TextChange &t : changes)
        if (t.kind == Kind::Usergroup)
            addOnce(names, t.id);
        else if (t.kind == Kind::Channel)
            addOnce(names, "<#" + t.id);
    return names->size() <= kMax;
}

} // namespace

// Users changed: presence and DND aren't drawn in message rows, so only a
// profile change (a name, an avatar, a bot flag) re-binds the rows that show
// that user; a text change (emoji, user groups, channel names) the rows that
// draw what changed.
void MessageList::usersChanged() {
    const model::Store &st        = _ctx.store();
    const bool          other     = &st != _seenStore;
    const uint64_t      sinceText = _seenText;
    const bool          text      = other || st.textRevision() != sinceText;
    const uint64_t      since     = _seenProfile;
    _seenStore                    = &st;
    _seenText                     = st.textRevision();
    _seenProfile                  = st.profileRevision();
    if (!text && _seenProfile == since)
        return;
    ++_bodyEpoch;                   // rows built before draw names and emoji as they were
    std::vector<std::string> names; // what c's needles point into
    size_t                   emoji = 0;
    // Another Store: everything.
    const bool               full  = other || (text && !textNeedles(st, sinceText, &names, &emoji));
    Changed                  c;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i < emoji) { // found in text as ":name:"
            names[i] = ":" + names[i] + ":";
            c.emoji.push_back(std::string_view(names[i]).substr(1, names[i].size() - 2));
        }
        c.needles.push_back(names[i]);
    }
    rebuild(true); // a bot flag regroups rows
    if (_items.empty())
        return;
    if (!full && _seenProfile != since)
        for (model::UserRef u = 0; u < st.userCount(); ++u)
            if (st.userRevision(u) > since) {
                c.users.push_back(u);
                if (!st.user(u).id.empty())
                    c.needles.push_back(st.user(u).id);
            }
    // A roster reload that changed many people: re-binding all is cheaper
    // than searching every message for each of them.
    if (full || c.users.size() > 64) {
        rowsChanged(0, int(_items.size()));
        return;
    }
    // Only rows built (on screen, kept or spare) can show it: one never
    // built is bound, and measured, from the Store when it scrolls in.
    for (const MessageRow *row = _rows; row; row = row->_nextRow) {
        const Ts              ts = row->ts();
        const int             i  = ts ? itemIndex(ts) : -1;
        const model::Message *m  = i >= 0 ? message(ts) : nullptr;
        if (!m)
            continue;
        bool hit = touches(*m, c);
        // An inline thread's replies draw in their root's row.
        if (!hit && _root == 0 && has(_inlineThreads, ts))
            if (const auto *replies = st.replies(_conv, ts))
                for (const model::Message &r : *replies)
                    if ((hit = touches(r, c)))
                        break;
        if (hit)
            rowsChanged(i, 1);
    }
}

std::string MessageList::itemLabel(size_t i) const {
    if (i >= _items.size())
        return {};
    const Item &it = _items[i];
    if (it.kind == Kind::Day)
        return base::dayLabel(model::tsSecs(it.ts), base::nowSecs());
    if (it.kind == Kind::Divider) {
        const model::Message              *root = message(_root);
        const std::vector<model::Message> *r    = _ctx.store().replies(_conv, _root);
        const size_t                       n    = r ? r->size() : root ? root->replyCount : 0;
        return n == 1 ? std::string(tr("1 reply")) : arg(tr("%1 replies"), str::number(int64_t(n)));
    }
    return {};
}

void MessageList::updateTyping() {
    std::vector<std::string_view> names;
    if (_conv != model::kNoConv)
        for (const model::Typing &t : _ctx.store().typing(_conv))
            if (t.user != _ctx.store().me && t.thread == _root)
                names.push_back(_ctx.store().user(t.user).label());
    std::string s;
    if (names.size() == 1)
        s = arg(tr("%1 is typing…"), names[0]);
    else if (names.size() == 2)
        s = arg(tr("%1 are typing…"), arg(tr("%1 and %2"), names[0], names[1]));
    else if (names.size() > 2)
        s = tr("Several people are typing…");
    _typing->setText(std::move(s));
}

void MessageList::setTypingRow(bool on) {
    _typingH = on ? kTypingH : 0;
    _typing->setVisible(on);
    invalidateLayout();
}

const std::string &MessageList::typingText() const {
    return _typing->text();
}

// ── Edges: mark read, load older ────────────────────────────────────────────

void MessageList::setReading(bool on) {
    if (on == _reading)
        return;
    _reading = on;
    if (on && _conv != model::kNoConv)
        scheduleEdgeCheck();
}

void MessageList::scheduleEdgeCheck() {
    if (_edgeTimer)
        return;
    // After the next frame's layout, when the visible range is known.
    _edgeTimer = _ctx.app.addTimer(kEdgeDelayMs, false, [this] {
        _edgeTimer = 0;
        checkEdges();
    });
}

void MessageList::checkEdges() {
    if (_conv == model::kNoConv)
        return;
    if (_root != 0) {
        // An open thread reads itself — on load and on each new reply
        // (markThreadRead: its cursor, the Threads entry).
        const auto *replies = _ctx.store().replies(_conv, _root);
        if (!_loadingThread && replies && !replies->empty() && !replies->back().pending &&
            replies->back().ts != _markedTs) {
            _markedTs = replies->back().ts;
            _ctx.backend.markThreadRead(_conv, _root, _markedTs);
        }
        return;
    }
    // Land first: the bottom showing before then must not mark it read.
    if (_openPending && applyOpenTarget()) {
        scheduleEdgeCheck(); // once the list is where it landed
        return;
    }
    const model::Conversation &c = _ctx.store().conversation(_conv);
    if (_reading && !_openPending && !c.messages.empty() &&
        _list->lastVisible() >= int(_items.size()) - 1) {
        const Ts newest = c.messages.back().ts;
        if (newest > c.lastRead && newest != _markedTs && !c.messages.back().pending) {
            _markedTs = newest;
            _ctx.backend.markRead(_conv, newest);
        } else if (newest <= c.lastRead) {
            _markedTs = newest; // seen read: a later "mark unread" must stick
            // Read, yet still badged (thread replies counted on the channel,
            // a cursor moved elsewhere): the backend recounts.
            if (c.unread || c.mentions)
                _ctx.backend.markRead(_conv, newest);
        }
    }
    const int first = _list->firstVisible();
    if (c.hasMoreBefore && !_loadingOlder && (c.messages.empty() || (first >= 0 && first <= 3))) {
        _loadingOlder              = true;
        const Ts            before = c.messages.empty() ? 0 : c.messages.front().ts;
        const ConvRef       conv   = _conv;
        std::weak_ptr<char> alive  = _alive;
        _ctx.backend.loadHistory(conv, before, [this, alive, conv](bool, const std::string &) {
            if (alive.expired())
                return;
            _loadingOlder = false;
            if (conv == _conv) {
                // The page is in: a jump target it didn't bring won't come.
                if (!applyJump())
                    _jumpTs = 0;
                updateState(); // an empty first page changes no messages
                scheduleEdgeCheck();
            }
        });
    }
}

// ── Layout, toolbar ─────────────────────────────────────────────────────────

void MessageList::layout() {
    const float w = width(), h = height();
    _list->setFrame({0, 0, w, std::max(0.f, h - _typingH)});
    _state->setFrame(_list->frame());
    _typing->setFrame({16, std::max(0.f, h - _typingH), std::max(0.f, w - 32), _typingH});
    placeToolbar();
    applyOpenTarget(); // the first layout knows the height a third of which it needs
}

void MessageList::placeToolbar() {
    if (!_toolbarRow || !_toolbarRow->visible() || !_toolbarRow->window()) {
        _toolbar->setVisible(false);
        return;
    }
    const model::Message *m = message(_toolbarRow->ts());
    // No toolbar on pending sends and system rows.
    if (!m || m->pending || isSystem(*m)) {
        _toolbar->setVisible(false);
        return;
    }
    const bool agent = _ctx.backend.isAgentSession(_conv);
    // Emoji: caps.reactions, except on my own messages in an agent session;
    // Save: caps.messageReminders (not agent sessions).
    _tbEmoji->setVisible(!(agent && m->user == _ctx.store().me));
    _tbSave->setVisible(!agent && _ctx.backend.capabilities().messageReminders);
    const bool saved = m->saved || _ctx.store().reminderAt(_conv, m->ts) != 0;
    _tbSave->setIcon(saved ? gfx::Icon::BookmarkFilled : gfx::Icon::Bookmark);
    _tbSave->setTooltip(saved ? tr("Remove from saved") : tr("Save for later"));
    // Ask agent: where a Claude Code session can be asked; on a linked
    // thread it shows active and opens the agent's thread.
    const Context::AgentLink link =
        _ctx.agentLink ? _ctx.agentLink(_conv, *m) : Context::AgentLink{};
    _tbAgent->setVisible(link.offered);
    _tbAgent->setInk(link.linked ? C::Accent : C::FormIconStrong);
    _tbAgent->setTooltip(link.linked ? tr("Open agent thread") : tr("Ask agent"));
    const ui::SizeF  sz = _toolbar->measure(ui::kInf, ui::kInf);
    const ui::RectF  rr = _toolbarRow->windowRect();
    const ui::PointF o  = mapFromWindow({rr.x, rr.y});
    // cardTop = message top − cardH/2 (straddling the row's top edge),
    // 12 px from the right; clipped at the viewport's top.
    _toolbar->setFrame({std::floor(width() - 12 - sz.w), std::floor(o.y - sz.h / 2), sz.w, sz.h});
    _toolbar->setVisible(o.y < height() - _typingH);
}

void MessageList::hideToolbar() {
    if (_toolbarRow) {
        _toolbarRow->update();
    }
    _toolbarRow = nullptr;
    _toolbar->setVisible(false);
}

void MessageList::rowHovered(MessageRow *row, bool on) {
    if (on) {
        if (_toolbarRow == row)
            return;
        if (_toolbarRow) {
            _toolbarRow->update();
        }
        _toolbarRow = row;
        placeToolbar();
        return;
    }
    // Leaving a row for the toolbar keeps it; the hover chain settles after
    // this event, so decide once it has.
    std::weak_ptr<char> alive = _alive;
    _ctx.app.platform().post([this, alive, row] {
        if (alive.expired() || _toolbarRow != row)
            return;
        if (!row->hovered() && !_toolbar->hovered() && !_fileBar->hovered())
            hideToolbar();
    });
}

bool MessageList::onEvent(ui::Event &e) {
    switch (e.type) {
    // Text selection: a press on message text starts a drag (a
    // press anywhere else here clears it); two clicks take a word, three a
    // line; Ctrl/Cmd+C copies it, Escape drops it.
    case ui::EventType::PointerDown: {
        if (e.button != plat::Button::Left)
            return false;
        const TextPos tp = textPosAt(e.windowPos);
        if (tp.ts && e.clicks >= 2) {
            uint32_t        base = 0;
            const int       i    = itemIndex(tp.ts);
            auto           *row  = i >= 0 ? static_cast<MessageRow *>(_list->viewFor(i)) : nullptr;
            SelectableText *l    = row ? labelAt(*row, e.windowPos.y, &base) : nullptr;
            if (l) {
                uint32_t a = 0, b = 0;
                if (e.clicks == 2)
                    l->wordAt(tp.offset - base, &a, &b);
                else
                    l->lineAt(l->textView().mapFromWindow(e.windowPos), &a, &b);
                _selDragging = false;
                select({tp.ts, base + a}, {tp.ts, base + b});
                return true;
            }
        }
        clearSelection();
        if (!tp.ts)
            return false;
        _selDragging = true;
        _selPointer  = e.windowPos;
        select(tp, tp);
        return true;
    }
    case ui::EventType::PointerMove:
        if (_selDragging) {
            _selPointer      = e.windowPos;
            const TextPos tp = dragPosAt(e.windowPos);
            if (tp.ts && !(tp == _selFocus))
                select(_selAnchor, tp);
            dragScroll();
            return true;
        }
        return false;
    case ui::EventType::PointerUp:
    case ui::EventType::PointerCancel:
        if (!_selDragging)
            return false;
        _selDragging = false;
        stopDragScroll();
        if (_selAnchor == _selFocus) // a plain click: no selection
            clearSelection();
        return true;
    case ui::EventType::KeyDown:
        if (!hasSelection())
            return false;
        if (e.key == plat::Key::C && (e.mods & plat::primaryMod()) &&
            !(e.mods & ui::kModMask & ~(plat::primaryMod() | plat::ModShift))) {
            copySelection();
            return true;
        }
        if (e.key == plat::Key::Escape) {
            clearSelection();
            return true;
        }
        return false;
    case ui::EventType::PointerLeave:
        hideToolbar();
        _fileView = nullptr;
        _fileBar->setVisible(false);
        return false;
    // Files dropped anywhere on the list attach to the composer below it.
    case ui::EventType::DropEnter:
    case ui::EventType::DropMove:
        if (!_ctx.attachFiles || !dragOffersFiles(e.raw))
            return false;
        e.dropAction = plat::DropAction::Copy;
        return true;
    case ui::EventType::DropLeave:
        return true;
    case ui::EventType::Drop: {
        auto paths = droppedFiles(e.raw);
        if (paths.empty() || !_ctx.attachFiles)
            return false;
        _ctx.attachFiles(std::move(paths), threadMode());
        return true;
    }
    default:
        return false;
    }
}

// ── Actions ─────────────────────────────────────────────────────────────────

void MessageList::reply(Ts ts) {
    if (onReply)
        onReply(_conv, ts);
    else if (_ctx.openThread)
        _ctx.openThread(_conv, ts);
}

void MessageList::openReactionPicker(Ts ts, ui::RectF anchor) {
    if (!window())
        return;
    const ConvRef conv = _conv;
    Context      &ctx  = _ctx;
    EmojiPicker::show(*window(), anchor, _ctx, [&ctx, conv, ts](const std::string &name) {
        ctx.backend.react(conv, ts, name, true);
    });
}

namespace {

// The tooltip chip (near-black, medium white
// text, 10/5 padding, radius 6, the arrow at the point) above a click.
class ClickToast final : public ui::Popup {
public:
    explicit ClickToast(std::string text) : _text(std::move(text)) {
        setModal(false);
        setCard(false);
        setPaintOutset(6);
    }
    ui::View *hitTest(ui::PointF) override { return nullptr; }
    ui::SizeF measureContent(float, float) override {
        if (!_l) {
            text::Style st = ui::font(ui::Font::Body);
            st.weight      = text::Weight::Medium;
            st.color       = ui::color(C::TooltipText);
            _l             = text::layoutPlain(_text, st, windowScale());
        }
        return {std::ceil(_l->width()) + 20, std::ceil(_l->height()) + 10 + 6};
    }
    void paint(gfx::Painter &p) override {
        if (!_l)
            measureContent(0, 0);
        const ui::RectF  w     = windowRect();
        const bool       below = w.y > anchor().y;
        const ui::RectF  body{0, below ? 6.f : 0.f, width(), height() - 6};
        const gfx::Color bg = ui::color(C::TooltipBg);
        p.dropShadow(body, 6, 5, {0, 1}, 0x30000000U);
        p.fillRoundRect(body, 6, bg);
        const float cx = std::clamp(anchor().x + anchor().w / 2 - w.x, 7.f, width() - 7);
        gfx::Path   a;
        if (below) {
            a.moveTo(cx - 7, body.y + 3);
            a.lineTo(cx + 7, body.y + 3);
            a.lineTo(cx, 0);
        } else {
            a.moveTo(cx - 7, body.bottom() - 3);
            a.lineTo(cx + 7, body.bottom() - 3);
            a.lineTo(cx, body.bottom() + 6);
        }
        a.close();
        p.fillPath(a, bg);
        _l->paint(p, snapPx({std::floor((width() - _l->width()) / 2), body.y + 5}));
    }

private:
    std::string                   _text;
    std::unique_ptr<text::Layout> _l;
};

} // namespace

void showClickToast(Context &ctx, ui::Window &w, const std::string &text, int ms, ui::PointF at) {
    auto *t = new ClickToast(text);
    t->setAnchor({at.x, at.y - 2, 1, 4}, ui::Popup::Place::Tip);
    ui::Popup *raw  = w.showPopup(std::unique_ptr<ui::Popup>(t));
    auto       gone = std::make_shared<bool>(false);
    raw->onClosed   = [gone] { *gone = true; };
    ctx.app.addTimer(ms, false, [raw, gone] {
        if (!*gone)
            raw->close();
    });
}

void MessageList::showToast(const std::string &text, int ms, ui::PointF at) {
    if (ui::Window *w = window())
        showClickToast(_ctx, *w, text, ms, at);
}

void MessageList::pressButton(Ts ts, const std::string &buttonId, ui::PointF at) {
    // A link button opens its URL instead.
    if (const model::Message *m = message(ts); m && m->extra)
        for (const model::Button &b : m->extra->buttons)
            if (b.id == buttonId && !b.url.empty()) {
                if (_ctx.openUrl)
                    _ctx.openUrl(b.url);
                return;
            }
    // The agent answers on its own schedule (a new message, this one
    // changing): the toast only confirms the press was taken.
    std::weak_ptr<char> alive = _alive;
    _ctx.backend.pressButton(
        _conv, ts, buttonId, [this, alive, at](bool ok, const std::string &err) {
            if (alive.expired())
                return;
            if (ok)
                showToast(tr("Sent to the app"), 1500, at);
            else if (err == model::Backend::kUnpressableButton)
                showToast(
                    tr("Slack doesn't let third-party apps press this kind of bot button"), 2600, at
                );
            else
                showToast(arg(tr("Couldn't press the button: %1"), err), 3000, at);
        }
    );
}

void MessageList::openImage(const std::string &path, int w, int h) {
    if (window())
        window()->showPopup(std::make_unique<ImageViewer>(_ctx, path, w, h, window()->size()));
}

// ── The message menu ────────────────────────────────────────────────────────
// The menus: the message menu (the hover toolbar's "…", also right click on
// the row), the reminder presets, the link menu (right click on a link) and
// the file menu (an image or a file).

namespace {

// The hint is the shortcut (the key chooses it while the menu is open).
constexpr ui::MenuDef kItemDefs[] = {
    {MessageList::kReply,
     uint16_t(gfx::Icon::MessageSquareReply),
     N_("Reply in thread"),
     nullptr,
     "T"},
    {MessageList::kOpenThread,
     uint16_t(gfx::Icon::MessageSquareReply),
     N_("Open thread"),
     nullptr,
     "T"},
    {MessageList::kMuteThread, uint16_t(gfx::Icon::BellOff), N_("Mute thread"), nullptr, nullptr},
    {MessageList::kUnmuteThread, uint16_t(gfx::Icon::Bell), N_("Unmute thread"), nullptr, nullptr},
    {MessageList::kEdit, uint16_t(gfx::Icon::Edit3), N_("Edit message"), nullptr, "E"},
    {MessageList::kCopyLink, uint16_t(gfx::Icon::Link), N_("Copy link"), nullptr, "L"},
    {MessageList::kCopyLinkInText,
     uint16_t(gfx::Icon::Link),
     N_("Copy link from message"),
     nullptr,
     nullptr},
    {MessageList::kCopyText, uint16_t(gfx::Icon::Copy), N_("Copy message"), nullptr, "Ctrl+C"},
    {MessageList::kPin, uint16_t(gfx::Icon::Pin), N_("Pin to channel"), nullptr, "P"},
    {MessageList::kUnpin, uint16_t(gfx::Icon::PinOff), N_("Unpin from channel"), nullptr, "P"},
    {MessageList::kSave, uint16_t(gfx::Icon::Bookmark), N_("Save for later"), nullptr, nullptr},
    {MessageList::kUnsave,
     uint16_t(gfx::Icon::BookmarkMinus),
     N_("Remove from saved"),
     nullptr,
     nullptr},
    {MessageList::kRemind, uint16_t(gfx::Icon::AlarmClock), N_("Remind me"), nullptr, nullptr},
    {MessageList::kRemoveReminder,
     uint16_t(gfx::Icon::AlarmClock),
     N_("Remove reminder"),
     nullptr,
     nullptr},
    {MessageList::kForward, uint16_t(gfx::Icon::Share2), N_("Forward message"), nullptr, nullptr},
    {MessageList::kMoveToThread,
     uint16_t(gfx::Icon::CornerDownRight),
     N_("Move to thread…"),
     nullptr,
     nullptr},
    {MessageList::kSummarize,
     uint16_t(gfx::Icon::Sparkles),
     N_("Summarize down"),
     nullptr,
     nullptr},
    {MessageList::kDelete, uint16_t(gfx::Icon::Trash2), N_("Delete message…"), nullptr, "Del"},
    {MessageList::kOpenLink, uint16_t(gfx::Icon::ExternalLink), N_("Open link"), nullptr, nullptr},
    {MessageList::kCopyLinkUrl, uint16_t(gfx::Icon::Link), N_("Copy link"), nullptr, nullptr},
    {MessageList::kCopyImageLink,
     uint16_t(gfx::Icon::Link),
     N_("Copy link to image"),
     nullptr,
     nullptr},
    {MessageList::kCopyFileLink,
     uint16_t(gfx::Icon::Link),
     N_("Copy link to file"),
     nullptr,
     nullptr},
    {MessageList::kCopyImage, uint16_t(gfx::Icon::Copy), N_("Copy full image"), nullptr, nullptr},
    {MessageList::kPreview, uint16_t(gfx::Icon::Eye), N_("Preview"), nullptr, nullptr},
    {MessageList::kDeleteFile, uint16_t(gfx::Icon::Trash2), N_("Delete file…"), nullptr, nullptr},
    {MessageList::kAllowAsker,
     uint16_t(gfx::Icon::Users),
     N_("Allow %1 to ask agent"),
     nullptr,
     nullptr},
    {MessageList::kUnlinkAgent, uint16_t(gfx::Icon::X), N_("Unlink agent"), nullptr, nullptr},
};

// Reminder presets, no icons.
constexpr const char *kRemindLabels[] = {
    N_("In 20 minutes"), N_("In 1 hour"), N_("In 3 hours"), N_("Tomorrow"), N_("Next week")
};

ui::MenuItem &addItem(std::vector<ui::MenuItem> &out, int id, bool enabled = true) {
    ui::MenuItem &m = ui::addMenuItem(out, kItemDefs, id, false, enabled);
    m.danger        = id == MessageList::kDelete || id == MessageList::kDeleteFile;
    return m;
}

// An image's type by its name: the known ones, else "image/<ext>", else PNG.
std::string imageMime(std::string_view path) {
    if (const std::string_view m = mime::fromName(path); !m.empty())
        return std::string(m);
    const std::string e = str::asciiLower(file::extension(path));
    return "image/" + (e.empty() ? std::string("png") : e);
}

// A CSV file, by type or name (any case).
bool isCsv(const model::File &f) {
    return f.mime == "text/csv" || mime::fromName(f.name) == "text/csv";
}

// Due times of the presets, from `now` (local): +20 min, +1 h, +3 h,
// tomorrow 9:00, next Monday 9:00.
int64_t remindDue(int preset, int64_t now) {
    switch (preset) {
    case 0:
        return now + 20 * 60;
    case 1:
        return now + 3600;
    case 2:
        return now + 3 * 3600;
    default: {
        const base::CivilTime c  = base::localTime(now);
        const int             wd = c.weekday == 0 ? 7 : c.weekday; // Mon=1…Sun=7
        return base::fromLocal(c.year, c.month, c.day + (preset == 3 ? 1 : 8 - wd), 9, 0);
    }
    }
}

} // namespace

std::vector<ui::MenuItem> MessageList::menuItems(Ts ts) const {
    std::vector<ui::MenuItem> items;
    const model::Message     *m = message(ts);
    if (!m || m->pending)
        return items;
    const Store &st        = _ctx.store;
    const bool   mine      = m->user == st.me && m->user != model::kNoUser;
    // Agent sessions (Claude Code): no permalinks, pins, reminders, edits or
    // moves, and a thread only opens where one exists (a subagent run or a
    // /btw branch) — "Reply in thread" where it takes replies.
    const bool   agent     = _ctx.backend.isAgentSession(_conv);
    // Deletable: my own messages, or any as a workspace admin — any
    // at all in an agent session (deleteAnyMessage) — and only when the
    // backend can take this one now (not while the session is working).
    const bool   canDelete = (agent || mine || (st.me != model::kNoUser && st.user(st.me).admin)) &&
                             _ctx.backend.canDeleteMessage(_conv, ts);
    if (_root == 0 && !m->isReply()) {
        if (!agent)
            addItem(items, kReply);
        else if (m->replyCount > 0)
            addItem(items, _ctx.backend.threadAcceptsReplies(_conv, m->ts) ? kReply : kOpenThread);
    }
    // Mute the thread this message belongs to (a root with replies, a reply);
    // the thread panel has its own toggle.
    if (_root == 0 && (m->replyCount > 0 || m->isReply())) {
        const Ts root = m->isReply() ? m->threadTs : m->ts;
        addItem(items, st.threadMuted(_conv, root) ? kUnmuteThread : kMuteThread);
    }
    ui::addMenuSeparator(items);
    if (mine && onEdit && !agent) {
        addItem(items, kEdit);
        ui::addMenuSeparator(items);
    }
    if (!agent)
        addItem(items, kCopyLink);
    if (!firstLink(m->text).empty())
        addItem(items, kCopyLinkInText);
    addItem(items, kCopyText);
    ui::addMenuSeparator(items);
    if (!agent)
        addItem(items, m->pinned ? kUnpin : kPin);
    // Slack's Later list: session-token workspaces only (saved.*).
    if (!agent && _ctx.backend.capabilities().messageReminders) {
        const bool reminded = st.reminderAt(_conv, ts) != 0;
        if (m->saved && !reminded)
            addItem(items, kUnsave);
        else if (!m->saved)
            addItem(items, kSave);
        addItem(items, reminded ? kRemoveReminder : kRemind).hint = reminded ? "" : "\xE2\x80\xBA";
    }
    ui::addMenuSeparator(items);
    addItem(items, kForward, bool(_ctx.forwardMessage));
    // "Move to thread…": a top-level message without replies of its own, and
    // only where the original can be deleted afterwards.
    if (_root == 0 && canDelete && !agent && !m->isReply() && m->replyCount == 0 && !isSystem(*m))
        addItem(items, kMoveToThread);
    // Always offered where AI is wired: without a provider it says so (and
    // links to Settings → AI assistance).
    addItem(items, kSummarize, _ctx.ai != nullptr);
    // Agent thread links (asking is the toolbar's robot): let the author
    // ask too, unlink from the root.
    if (const Context::AgentLink link =
            _ctx.agentLink && !isSystem(*m) ? _ctx.agentLink(_conv, *m) : Context::AgentLink{};
        link.offered) {
        if (link.canAllow)
            addItem(items, kAllowAsker).label =
                arg(tr("Allow %1 to ask agent"), std::string(st.user(m->user).label()));
        if (link.root)
            addItem(items, kUnlinkAgent);
    }
    if (canDelete) {
        ui::addMenuSeparator(items);
        addItem(items, kDelete);
    }
    if (!items.empty() && items.back().separator)
        items.pop_back();
    return items;
}

std::vector<ui::MenuItem> MessageList::fileMenuItems(Ts ts, const std::string &path) const {
    std::vector<ui::MenuItem> items;
    const model::Message     *m     = message(ts);
    const model::File        *f     = fileAt(m, path);
    const bool                image = !f || f->isImage();
    if (f && isCsv(*f))
        addItem(items, kPreview); // the table viewer
    addItem(items, image ? kCopyImageLink : kCopyFileLink);
    if (image)
        addItem(items, kCopyImage);
    const Store &st = _ctx.store;
    const bool   canDelete =
        m && (m->user == st.me || (st.me != model::kNoUser && st.user(st.me).admin));
    if (f && canDelete && !f->id.empty()) {
        ui::addMenuSeparator(items);
        addItem(items, kDeleteFile).label = image ? tr("Delete image…") : tr("Delete file…");
    }
    return items;
}

std::vector<ui::MenuItem> MessageList::remindItems() {
    std::vector<ui::MenuItem> items;
    items.push_back(ui::MenuItem::headerItem(tr("Remind me about this…")));
    for (int i = 0; i < 5; ++i) {
        ui::MenuItem m;
        m.id    = kRemindPreset + i;
        m.label = tr(kRemindLabels[i]);
        items.push_back(std::move(m));
    }
    ui::addMenuSeparator(items);
    ui::MenuItem custom; // the reminder dialog
    custom.id    = kRemindCustom;
    custom.label = tr("Custom…");
    items.push_back(std::move(custom));
    return items;
}

std::vector<ui::MenuItem> MessageList::linkMenuItems() {
    std::vector<ui::MenuItem> items;
    addItem(items, kOpenLink);
    addItem(items, kCopyLinkUrl);
    return items;
}

std::vector<Ts> MessageList::threadRoots(Ts except) const {
    // Loaded roots with replies, not pending, newest first.
    std::vector<Ts> out;
    if (_conv == model::kNoConv)
        return out;
    const auto &msgs = _ctx.store().conversation(_conv).messages;
    for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
        if (it->replyCount > 0 && !it->isReply() && !it->pending && it->ts != except)
            out.push_back(it->ts);
    return out;
}

void MessageList::openMenu(Ts ts, ui::PointF at) {
    if (!window())
        return;
    std::vector<ui::MenuItem> items = menuItems(ts);
    if (items.empty())
        return;
    std::weak_ptr<char> alive = _alive;
    const ConvRef       conv  = _conv;
    ui::Menu::popupAt(*window(), at, std::move(items), [this, alive, conv, ts, at](int id) {
        if (!alive.expired() && conv == _conv)
            runMenuAction(ts, id, {}, at);
    });
}

void MessageList::openFileMenu(Ts ts, const std::string &path, ui::PointF at) {
    if (!window() || !message(ts) || message(ts)->pending)
        return;
    std::weak_ptr<char> alive = _alive;
    const ConvRef       conv  = _conv;
    ui::Menu::popupAt(
        *window(), at, fileMenuItems(ts, path), [this, alive, conv, ts, path](int id) {
            if (!alive.expired() && conv == _conv)
                runMenuAction(ts, id, path, {});
        }
    );
}

void showLinkMenu(Context &ctx, ui::Window &w, ui::PointF at, const std::string &url) {
    ui::Menu::popupAt(w, at, MessageList::linkMenuItems(), [&ctx, url](int id) {
        if (id == MessageList::kOpenLink && ctx.openUrl)
            ctx.openUrl(url);
        else if (id == MessageList::kCopyLinkUrl)
            ctx.app.platform().setClipboardText(url);
    });
}

void MessageList::runMenuAction(Ts ts, int id, const std::string &path, ui::PointF at) {
    const model::Message *msg = message(ts);
    if (!msg)
        return;
    std::string   copy; // what the item puts on the clipboard
    const ConvRef conv = _conv;
    if (id == kRemindCustom) {
        if (window())
            showReminderDialog(_ctx, *window(), conv, ts);
        return;
    }
    if (id >= kRemindPreset && id < kRemindPreset + 5) {
        _ctx.backend.setReminder(conv, ts, remindDue(id - kRemindPreset, base::nowSecs()));
        return;
    }
    switch (id) {
    case kReply:
    case kOpenThread:
        reply(ts);
        break;
    case kMuteThread:
    case kUnmuteThread:
        _ctx.store().setThreadMuted(conv, msg->isReply() ? msg->threadTs : ts, id == kMuteThread);
        break;
    case kEdit:
        if (onEdit)
            onEdit(conv, ts);
        break;
    case kCopyLink:
        copy = _ctx.store().permalink(conv, ts, msg->isReply() ? msg->threadTs : 0);
        break;
    case kCopyLinkInText:
        copy = firstLink(msg->text);
        break;
    case kCopyText: // shortened link labels copy as their full URLs
        copy = plainText(_ctx.store(), msg->text, true);
        break;
    case kPin:
    case kUnpin:
        _ctx.backend.setPinned(conv, ts, id == kPin);
        break;
    case kSave:
    case kUnsave:
        _ctx.backend.setSaved(conv, ts, id == kSave);
        break;
    case kRemind: {
        // The presets replace the menu at the same point.
        if (!window())
            break;
        std::weak_ptr<char> alive = _alive;
        ui::Menu::popupAt(*window(), at, remindItems(), [this, alive, conv, ts](int preset) {
            if (!alive.expired() && conv == _conv)
                runMenuAction(ts, preset, {}, {});
        });
        break;
    }
    case kRemoveReminder:
        _ctx.backend.setReminder(conv, ts, 0);
        break;
    case kDelete: // the delete dialog first
        if (window())
            showDeleteMessageDialog(_ctx, *window(), conv, ts);
        break;
    case kForward:
        if (_ctx.forwardMessage)
            _ctx.forwardMessage(conv, ts, {});
        break;
    case kMoveToThread:
        if (window())
            showMoveToThreadDialog(_ctx, *window(), conv, ts, threadRoots(ts));
        break;
    case kSummarize: {
        // From the chosen message down to the newest loaded one (the pages
        // below a visible message are always loaded).
        std::vector<Ts> span;
        for (int i = itemIndex(ts); i >= 0 && size_t(i) < _items.size(); ++i)
            if (_items[size_t(i)].kind == Kind::Message)
                if (const model::Message *m = message(_items[size_t(i)].ts); m && !m->pending)
                    span.push_back(m->ts);
        summarizeDown(_ctx, conv, std::move(span), threadMode());
        break;
    }
    case kAllowAsker:
    case kUnlinkAgent:
        if (_ctx.agentLinkAction)
            _ctx.agentLinkAction(
                conv,
                ts,
                id == kAllowAsker ? Context::AgentLinkAction::Allow
                                  : Context::AgentLinkAction::Unlink
            );
        break;
    case kPreview:
        if (const model::File *f = fileAt(msg, path))
            openCsvPreview(*f);
        break;
    case kCopyImageLink:
    case kCopyFileLink: // the file's permalink, else the original (never a thumbnail)
        if (const model::File *f = fileAt(msg, path))
            copy = f->permalink.empty() ? fileUrl(f->source()) : f->permalink;
        else
            copy = fileUrl(path);
        break;
    case kDeleteFile:
        if (const model::File *f = fileAt(msg, path))
            _ctx.backend.deleteFile(conv, ts, f->id);
        break;
    case kCopyImage:
        if (const model::File *f = fileAt(msg, path))
            copyImage(*f);
        break;
    default:
        break;
    }
    if (!copy.empty())
        _ctx.app.platform().setClipboardText(std::move(copy));
}

// The full image (source(), not the
// thumbnail the list shows) as its bytes, as they are (a PNG stays a PNG; no
// re-encode). A local file (a pending upload, the demo) also goes as itself
// for file managers; a remote one comes from the image disk cache when the
// viewer already fetched it, else it is downloaded to a temporary file
// (removed once read). The footer's cog runs from the click until the
// clipboard is set; the disk reads happen on a worker.
void MessageList::copyImage(const model::File &f) {
    const std::string source = f.source();
    if (source.empty())
        return;
    std::string mime = f.mime;
    if (mime.empty()) // from the name; a URL's query is not part of it
        mime = imageMime(std::string_view(source).substr(0, source.find('?')));
    const std::string name = f.name.empty() ? std::string(tr("image")) : f.name;
    const int         job  = model::jobs().begin(arg(tr("Copying %1"), name));
    plat::App        &pa   = _ctx.app.platform();
    // Reads `local` off the UI thread (deleting it when `temp`), then sets the
    // clipboard and ends the job. Never touches the list: it may be gone.
    auto              put  = [&pa, job, mime](std::string local, bool temp, bool withUri) {
        auto bytes = std::make_shared<std::string>();
        auto ok    = std::make_shared<bool>(false);
        model::runInBackground(
            pa,
            [bytes, ok, local, temp] {
                *ok = file::readAll(local, bytes.get());
                if (temp)
                    removeTempDownload(local);
            },
            [&pa, job, mime, bytes, ok, local, withUri] {
                if (*ok) {
                    std::vector<plat::DataItem> items;
                    items.push_back({mime, std::move(*bytes)});
                    if (withUri)
                        items.push_back({"text/uri-list", fileUrl(local)});
                    pa.setClipboard(std::move(items));
                } else {
                    LOG_WARN("messages", "Copy full image failed: unreadable");
                }
                model::jobs().end(job);
            }
        );
    };
    if (!RemoteImages::isRemote(source)) {
        put(source, false, true);
        return;
    }
    if (_ctx.remote)
        if (std::string local = _ctx.remote->cachedPath(source); !local.empty()) {
            put(std::move(local), false, false);
            return;
        }
    std::string temp = tempDownloadPath(pa, name);
    if (temp.empty()) {
        LOG_WARN("messages", "Copy full image failed: no temporary directory");
        model::jobs().end(job);
        return;
    }
    fetchFile(pa, _ctx.backend, source, temp, [put, temp, job](bool ok, const std::string &err) {
        if (ok) {
            put(temp, true, false);
            return;
        }
        LOG_WARN("messages", "Copy full image failed: %s", err.c_str());
        removeTempDownload(temp);
        model::jobs().end(job);
    });
}

// The file fetched to a temporary copy (a local one
// read in place), read and parsed on a worker, then the table viewer; the
// footer's cog runs from the click until it opens (or the download fails).
void MessageList::openCsvPreview(const model::File &f) {
    const std::string source = f.source();
    if (source.empty())
        return;
    const std::string   name  = f.name.empty() ? std::string(tr("file")) : f.name;
    const int           job   = model::jobs().begin(arg(tr("Downloading %1"), name));
    plat::App          &pa    = _ctx.app.platform();
    std::weak_ptr<char> alive = _alive;
    auto                parse = [this, alive, &pa, job](std::string local, bool temp) {
        auto rows = std::make_shared<std::vector<std::vector<std::string>>>();
        model::runInBackground(
            pa,
            [rows, local, temp] {
                *rows = readCsvFile(local);
                if (temp)
                    removeTempDownload(local);
            },
            [this, alive, rows, job] {
                model::jobs().end(job);
                if (!alive.expired() && window())
                    showTableViewer(*window(), std::move(*rows));
            }
        );
    };
    if (!RemoteImages::isRemote(source)) {
        parse(source, false);
        return;
    }
    std::string temp = tempDownloadPath(pa, name);
    if (temp.empty()) {
        LOG_WARN("messages", "CSV preview download failed: no temporary directory");
        model::jobs().end(job);
        return;
    }
    fetchFile(pa, _ctx.backend, source, temp, [parse, temp, job](bool ok, const std::string &err) {
        if (ok) {
            parse(temp, true);
            return;
        }
        LOG_WARN("messages", "CSV preview download failed: %s", err.c_str());
        removeTempDownload(temp);
        model::jobs().end(job);
    });
}

void MessageList::jumpTo(Ts ts) {
    _jumpTs = ts;
    if (applyJump() || _conv == model::kNoConv)
        return;
    // Not loaded: wait for the first page (checkEdges asks for it), or give
    // up when messages are there already and it isn't among them. A thread's
    // replies come in one load (Saved messages jumps to a reply).
    if (threadMode()) {
        if (_ctx.store().replies(_conv, _root))
            _jumpTs = 0;
        return;
    }
    const model::Conversation &c = _ctx.store().conversation(_conv);
    if (!c.messages.empty() || !c.hasMoreBefore)
        _jumpTs = 0;
}

bool MessageList::applyJump() {
    if (!_jumpTs || itemIndex(_jumpTs) < 0)
        return false;
    scrollToMessage(std::exchange(_jumpTs, 0));
    return true;
}

void MessageList::scrollToMessage(Ts ts, bool animated, bool flash) {
    const int i = itemIndex(ts);
    if (i < 0)
        return;
    _list->scrollToItem(i, ui::VirtualList::ItemAlign::Center, animated);
    if (!flash)
        return;
    _flashTs = ts;
    if (ui::View *v = _list->viewFor(i))
        v->update();
    _ctx.app.cancelTimer(_flashTimer);
    _flashTimer = _ctx.app.addTimer(1600, false, [this] {
        _flashTimer = 0;
        const int j = itemIndex(_flashTs);
        _flashTs    = 0;
        if (ui::View *v = j >= 0 ? _list->viewFor(j) : nullptr)
            v->update();
    });
}

// ── Selection ───────────────────────────────────────────────────────────────
// Text selection over the message bodies: a drag from one message's
// text to another's, a double click on a word, a triple click on a line.
// Positions are (message, offset into its selectableTexts joined by '\n'), so
// a selection outlives the rows that show it.

MessageList::TextPos MessageList::textPosAt(ui::PointF wp) const {
    const int first = _list->firstVisible(), last = _list->lastVisible();
    for (int i = std::max(0, first); first >= 0 && i <= last; ++i) {
        if (_items[size_t(i)].kind != Kind::Message)
            continue;
        auto *row = static_cast<MessageRow *>(_list->viewFor(i));
        if (!row)
            continue;
        const ui::RectF r = row->windowRect();
        if (wp.y < r.y || wp.y >= r.y + r.h)
            continue;
        uint32_t        base = 0;
        SelectableText *l    = labelAt(*row, wp.y, &base);
        if (!l)
            return {};
        const ui::RectF  lr = textRect(*l);
        const ui::PointF p  = l->textView().mapFromWindow(
            {std::max(wp.x, lr.x), std::clamp(wp.y, lr.y, lr.y + std::max(0.f, lr.h - 1))}
        );
        return {row->ts(), base + l->textOffsetAt(p)};
    }
    return {};
}

MessageList::TextPos MessageList::dragPosAt(ui::PointF wp) const {
    // Inside the list (a point past its top or bottom is at that edge), on
    // the nearest row with text: its start above the text, its end below.
    const ui::RectF lr = _list->windowRect();
    wp.y               = std::clamp(wp.y, lr.y, lr.y + std::max(0.f, lr.h - 1));
    const int   first = _list->firstVisible(), last = _list->lastVisible();
    MessageRow *best = nullptr;
    float       dist = 1e9f;
    for (int i = std::max(0, first); first >= 0 && i <= last; ++i) {
        if (_items[size_t(i)].kind != Kind::Message)
            continue;
        auto *row = static_cast<MessageRow *>(_list->viewFor(i));
        if (!row || row->selectionLabels().empty())
            continue;
        const ui::RectF r = row->windowRect();
        const float     d = wp.y < r.y ? r.y - wp.y : wp.y >= r.y + r.h ? wp.y - (r.y + r.h) : 0;
        if (d < dist) {
            dist = d;
            best = row;
        }
    }
    if (!best)
        return {};
    const std::vector<SelectableText *> &labels = best->selectionLabels();
    uint32_t                             end    = 0; // the joined text's size
    for (SelectableText *l : labels)
        end += l->textSize() + 1;
    end -= 1;
    const ui::RectF top = textRect(*labels.front());
    const ui::RectF bot = textRect(*labels.back());
    if (wp.y < top.y)
        return {best->ts(), 0};
    if (wp.y > bot.y + bot.h)
        return {best->ts(), end};
    uint32_t        base = 0;
    SelectableText *l    = labelAt(*best, wp.y, &base);
    if (!l)
        return {};
    const ui::RectF  r = textRect(*l);
    const ui::PointF p = l->textView().mapFromWindow(
        {std::max(wp.x, r.x), std::clamp(wp.y, r.y, r.y + std::max(0.f, r.h - 1))}
    );
    return {best->ts(), base + l->textOffsetAt(p)};
}

namespace {
// How far a point is into the list's top (-) or bottom (+) edge band.
float dragEdge(const ui::RectF &r, float y) {
    constexpr float kBand = 24;
    if (y < r.y + kBand)
        return y - (r.y + kBand);
    if (y > r.y + r.h - kBand)
        return y - (r.y + r.h - kBand);
    return 0;
}
} // namespace

void MessageList::dragScroll() {
    if (!_selDragging || dragEdge(_list->windowRect(), _selPointer.y) == 0) {
        stopDragScroll();
        return;
    }
    if (_selScrollTimer)
        return;
    _selScrollTimer = _ctx.app.addTimer(16, true, [this] {
        const float d = _selDragging ? dragEdge(_list->windowRect(), _selPointer.y) : 0;
        if (d == 0) {
            stopDragScroll();
            return;
        }
        // The rows laid out after the last step: extend onto them, then on.
        const TextPos tp = dragPosAt(_selPointer);
        if (tp.ts && !(tp == _selFocus))
            select(_selAnchor, tp);
        // Faster the further out: 4 px a frame at the band, up to 48.
        _list->scrollBy(std::copysign(std::min(48.f, 4 + std::abs(d) / 2), d));
    });
}

void MessageList::stopDragScroll() {
    if (_selScrollTimer)
        _ctx.app.cancelTimer(_selScrollTimer);
    _selScrollTimer = 0;
}

bool MessageList::hasSelection() const {
    return _selAnchor.ts && _selFocus.ts && !(_selAnchor == _selFocus);
}

void MessageList::select(TextPos a, TextPos f) {
    _selAnchor      = a;
    _selFocus       = f;
    const int first = _list->firstVisible(), last = _list->lastVisible();
    for (int i = std::max(0, first - 4); first >= 0 && i <= last + 4; ++i)
        if (size_t(i) < _items.size() && _items[size_t(i)].kind == Kind::Message)
            if (auto *row = static_cast<MessageRow *>(_list->viewFor(i)))
                applySelection(*row);
}

void MessageList::clearSelection() {
    _selDragging = false;
    stopDragScroll();
    if (_selAnchor.ts || _selFocus.ts)
        select({}, {});
}

void MessageList::applySelection(MessageRow &row) const {
    TextPos a = _selAnchor, f = _selFocus;
    if (a.ts > f.ts || (a.ts == f.ts && a.offset > f.offset))
        std::swap(a, f);
    const Ts ts   = row.ts();
    uint32_t from = 0, to = 0; // in the message's text
    if (a.ts && f.ts && ts >= a.ts && ts <= f.ts) {
        from = ts == a.ts ? a.offset : 0;
        to   = ts == f.ts ? f.offset : UINT32_MAX;
    }
    uint32_t base = 0;
    for (SelectableText *l : row.selectionLabels()) {
        const uint32_t n  = l->textSize();
        const uint32_t lo = std::max(from, base), hi = std::min<uint64_t>(to, uint64_t(base) + n);
        if (hi > lo)
            l->selectText(lo - base, hi - base);
        else
            l->selectText(0, 0);
        base += n + 1;
    }
}

std::string MessageList::selectedText() const {
    if (!hasSelection())
        return {};
    TextPos a = _selAnchor, f = _selFocus;
    if (a.ts > f.ts || (a.ts == f.ts && a.offset > f.offset))
        std::swap(a, f);
    std::string out;
    bool        any = false;
    for (const Item &it : _items) {
        if (it.kind != Kind::Message || it.ts < a.ts || it.ts > f.ts)
            continue;
        const model::Message *m = message(it.ts);
        if (!m)
            continue;
        // Joined by '\n', as the offsets count.
        const std::vector<std::string> parts = selectableTexts(const_cast<Context &>(_ctx), *m);
        std::string                    text;
        for (size_t k = 0; k < parts.size(); ++k) {
            if (k)
                text += '\n';
            text += parts[k];
        }
        const size_t from = it.ts == a.ts ? std::min<size_t>(a.offset, text.size()) : 0;
        const size_t to   = it.ts == f.ts ? std::min<size_t>(f.offset, text.size()) : text.size();
        if (to <= from)
            continue;
        if (any)
            out += '\n';
        out.append(text, from, to - from);
        any = true;
    }
    return out;
}

void MessageList::copySelection() {
    if (hasSelection())
        _ctx.app.platform().setClipboardText(selectedText());
}

// ── Opening position ────────────────────────────────────────────────────────
// A conversation shown before
// opens where it was left (the bottom, or a message at its offset); a first
// open lands on the first message after the read cursor, a third down the
// viewport (the bottom when everything is read).

void MessageList::saveAnchor() {
    if (_conv == model::kNoConv || _root != 0 || _items.empty())
        return;
    const std::string &id = _ctx.store().conversation(_conv).id;
    SavedAnchor        a;
    a.conv = id;
    if (_list->pinned() || _list->atEnd()) {
        a.atBottom = true; // returning sticks to the bottom (and new messages)
    } else {
        const ui::VirtualList::Anchor an = _list->anchor();
        if (an.index < 0 || size_t(an.index) >= _items.size())
            return;
        a.ts     = _items[size_t(an.index)].ts;
        a.kind   = _items[size_t(an.index)].kind;
        a.offset = an.offset;
    }
    for (SavedAnchor &x : _anchors)
        if (x.conv == id) {
            x = std::move(a);
            return;
        }
    _anchors.push_back(std::move(a));
}

bool MessageList::applyOpenTarget() {
    if (!_openPending || _root != 0 || _conv == model::kNoConv || _items.empty() ||
        _list->height() <= 0)
        return false;
    _openPending = false;
    if (!_openAnchor.conv.empty()) {
        if (!_openAnchor.atBottom)
            for (size_t i = 0; i < _items.size(); ++i)
                if (_items[i].ts == _openAnchor.ts && _items[i].kind == _openAnchor.kind) {
                    _list->scrollToAnchor({int(i), _openAnchor.offset});
                    return true;
                }
        _list->scrollToBottom();
        return true;
    }
    if (_openLastRead)
        for (size_t i = 0; i < _items.size(); ++i)
            if ((_items[i].kind == Kind::Message || _items[i].kind == Kind::System) &&
                _items[i].ts > _openLastRead) {
                _list->scrollToAnchor({int(i), -std::floor(_list->height() / 3)});
                return true;
            }
    _list->scrollToBottom();
    return true;
}

// ── Inline threads, previews, cards ─────────────────────────────────────────

void MessageList::setThreadsInline(bool on) {
    if (_threadsInline == on)
        return;
    _threadsInline = on;
    _inlineThreads.clear(); // switching modes drops the expansions (an open panel stays)
    if (!_items.empty())
        rowsChanged(0, int(_items.size()));
}

void MessageList::setOpenThreadRoot(Ts root) {
    if (_openThreadRoot == root)
        return;
    const Ts old    = _openThreadRoot;
    _openThreadRoot = root;
    rowChanged(old);
    rowChanged(root);
}

bool MessageList::threadOpen(Ts root) const {
    return _threadsInline ? has(_inlineThreads, root)
                          : _openThreadRoot != 0 && _openThreadRoot == root;
}

void MessageList::rowsChanged(int index, int n) {
    ++_bodyEpoch; // something besides the message: no row keeps its body
    _list->itemsChanged(index, n);
}

void MessageList::resetRows() {
    ++_bodyEpoch;
    _list->reset();
}

void MessageList::rowMade(MessageRow *row) {
    row->_nextRow = _rows;
    if (_rows)
        _rows->_prevRow = row;
    _rows = row;
}

void MessageList::rowGone(MessageRow *row) {
    if (_toolbarRow == row)
        _toolbarRow = nullptr;
    (row->_prevRow ? row->_prevRow->_nextRow : _rows) = row->_nextRow;
    if (row->_nextRow)
        row->_nextRow->_prevRow = row->_prevRow;
}

void MessageList::rowChanged(Ts ts) {
    if (!ts)
        return;
    if (const int i = itemIndex(ts); i >= 0)
        rowsChanged(i, 1);
}

void MessageList::replyBarClicked(Ts root) {
    if (_threadsInline) {
        // The bar toggles the replies under the message.
        if (has(_inlineThreads, root)) {
            std::erase(_inlineThreads, root);
            // Also open in the panel: close that too.
            if (_openThreadRoot == root && _ctx.closeThread)
                _ctx.closeThread();
        } else {
            _inlineThreads.push_back(root);
            loadInline(root);
        }
        rowChanged(root);
    } else if (_openThreadRoot == root && _ctx.closeThread) {
        _ctx.closeThread(); // the open thread's bar closes the panel
    } else {
        reply(root);
    }
}

void MessageList::loadInline(Ts root) {
    std::weak_ptr<char> alive = _alive;
    const ConvRef       conv  = _conv;
    _ctx.backend.loadThread(conv, root, [this, alive, conv, root](bool, const std::string &) {
        if (!alive.expired() && conv == _conv && has(_inlineThreads, root))
            rowChanged(root);
    });
}

bool MessageList::has(const std::vector<Key> &v, Key k) {
    for (const Key &x : v)
        if (x.ts == k.ts && x.a == k.a && x.b == k.b)
            return true;
    return false;
}

void MessageList::toggle(std::vector<Key> &v, Key k) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].ts == k.ts && v[i].a == k.a && v[i].b == k.b) {
            v.erase(v.begin() + long(i));
            return;
        }
    v.push_back(k);
}

bool MessageList::attachmentHidden(const model::Message &m, size_t index) const {
    const model::Attachment &a = m.attachments()[index];
    return (a.linkPreview && !_ctx.linkPreviews) || has(_dismissed, {m.ts, int(index), -1});
}

bool MessageList::removesPreviewServerSide(const model::Message &m) const {
    return !m.pending && _ctx.backend.capabilities().removePreview &&
           _ctx.store().me != model::kNoUser && m.user == _ctx.store().me;
}

void MessageList::dismissAttachment(Ts ts, size_t index) {
    const model::Message *m = message(ts);
    if (!m || index >= m->attachments().size())
        return;
    // Hidden at once either way. An own message's preview is also removed
    // for everyone; once that succeeded the Store's message lost it (the
    // rest renumbered), so the index-keyed hides for it are dropped.
    _dismissed.push_back({ts, int(index), -1});
    rowChanged(ts);
    if (!removesPreviewServerSide(*m))
        return;
    const model::Attachment &a     = m->attachments()[index];
    const int                id    = a.id > 0 ? a.id : int(index) + 1; // positional
    std::weak_ptr<char>      alive = _alive;
    Context                 &ctx   = _ctx;
    _ctx.backend.deleteAttachment(
        _conv, ts, id, [this, alive, &ctx, ts](bool ok, const std::string &err) {
            if (!ok) {
                if (ctx.backend.onError)
                    ctx.backend.onError(arg(tr("Couldn't remove the preview (%1)."), err));
                return;
            }
            if (alive.expired())
                return;
            std::erase_if(_dismissed, [ts](const Key &k) { return k.ts == ts; });
            rowChanged(ts);
        }
    );
}

bool MessageList::imageCollapsed(Ts ts, int attachment, int block) const {
    return has(_folded, {ts, attachment, block});
}

void MessageList::toggleImage(Ts ts, int attachment, int block) {
    toggle(_folded, {ts, attachment, block});
    rowChanged(ts);
}

bool MessageList::unfurlExpanded(Ts ts, int attachment) const {
    return has(_expanded, {ts, attachment, -1});
}

void MessageList::toggleUnfurl(Ts ts, int attachment) {
    toggle(_expanded, {ts, attachment, -1});
    rowChanged(ts);
}

void MessageList::openFileViewer(Ts ts, const std::string &path) {
    if (const model::File *f = fileAt(message(ts), path); f && window())
        showFileViewer(_ctx, *window(), this, ts, *f);
}

void MessageList::openCanvas(const model::File &f) {
    if (_ctx.openCanvas)
        _ctx.openCanvas(_conv, f);
    else if (_ctx.openUrl && !f.permalink.empty())
        _ctx.openUrl(f.permalink);
}

const std::string *MessageList::canvasPreview(const std::string &id, int *state) {
    for (CanvasPreview &p : _canvasPreviews)
        if (p.id == id) {
            *state = p.state;
            return p.state == 1 ? &p.html : nullptr;
        }
    // The oldest finished one makes room (one still loading is waited for).
    if (_canvasPreviews.size() >= kCanvasPreviews)
        for (size_t i = 0; i < _canvasPreviews.size(); ++i)
            if (_canvasPreviews[i].state != 0) {
                _canvasPreviews.erase(_canvasPreviews.begin() + ptrdiff_t(i));
                break;
            }
    _canvasPreviews.push_back({id, {}, 0});
    *state                    = 0;
    std::weak_ptr<char> alive = _alive;
    _ctx.backend.loadCanvasContent(id, [this, alive, id](std::string html, std::string error) {
        if (alive.expired())
            return;
        for (CanvasPreview &p : _canvasPreviews)
            if (p.id == id) {
                p.html  = std::move(html);
                p.state = error.empty() && !p.html.empty() ? 1 : -1;
            }
        // The cards read it when bound again.
        for (size_t i = 0; i < _items.size(); ++i)
            if (const model::Message *m = message(_items[i].ts))
                for (const model::File &f : m->files())
                    if (f.id == id)
                        rowsChanged(int(i), 1);
    });
    return nullptr;
}

void MessageList::openHtmlFile(const model::File &f) {
    // The page itself in the browser, not Slack's file
    // page (which only offers a download). url_private needs credentials:
    // fetched once to the cache, the browser gets the local copy.
    if (!RemoteImages::isRemote(f.path)) { // a pending upload, a demo file
        if (_ctx.openUrl)
            _ctx.openUrl(fileUrl(f.path));
        return;
    }
    std::string name(file::baseName(f.name));
    for (char &c : name)
        if (std::strchr("\\/:*?\"<>|", c))
            c = '_';
    if (mime::fromName(name) != "text/html")
        name += ".html";
    const std::string cache = identity::cacheDir(_ctx.app.platform());
    if (cache.empty())
        return;
    const std::string path   = file::join(cache, "files/" + f.id + "-" + name);
    Context          &ctx    = _ctx; // outlives the list; the download may outlive it
    auto              have   = std::make_shared<bool>(false);
    const std::string source = f.path, fallback = f.permalink, title = f.name;
    model::runInBackground(
        ctx.app.platform(),
        [have, path] {
            *have = file::size(path) > 0;
            if (!*have)
                file::makeDirs(file::dirName(path));
        },
        [&ctx, have, path, source, fallback, title] {
            if (*have) {
                if (ctx.openUrl)
                    ctx.openUrl(file::toFileUrl(path));
                return;
            }
            const int job = model::jobs().begin(arg(tr("Downloading %1"), title));
            fetchFile(
                ctx.app.platform(),
                ctx.backend,
                source,
                path,
                [&ctx, job, path, fallback](bool ok, const std::string &err) {
                    model::jobs().end(job);
                    if (!ok)
                        LOG_WARN("messages", "HTML file download failed: %s", err.c_str());
                    else if (ctx.remote) // counts toward the cache limit
                        ctx.remote->noteWritten(path);
                    if (ctx.openUrl && (ok || !fallback.empty()))
                        ctx.openUrl(ok ? file::toFileUrl(path) : fallback);
                }
            );
        }
    );
}

ui::Popup *
showFileViewer(Context &ctx, ui::Window &w, MessageList *list, Ts ts, const model::File &f) {
    return w.showPopup(std::make_unique<FileViewer>(ctx, list, ts, f));
}

} // namespace screens
