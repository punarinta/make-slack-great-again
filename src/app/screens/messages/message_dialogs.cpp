#include "app/screens/messages/message_dialogs.h"

#include "app/screens/common/message_rules.h"
#include "app/screens/common/message_text.h"
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/rich.h"
#include "app/screens/messages/rows.h"
#include "app/screens/messages/table_view.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "ui/controls.h"
#include "ui/datetime.h"

#include <algorithm>
#include <cmath>

namespace screens {

using i18n::arg;
using i18n::tr;
using ui::Align;
using ui::Button;
using ui::C;
using V = ui::Button::Kind;

namespace {

// The preview card: the file card's frame, radius 6, a stronger border.
class PreviewCard final : public ui::View {
public:
    void paint(gfx::Painter &p) override {
        paintCardFrame(p, bounds(), 6, C::FormHighlightStrong);
        View::paint(p);
    }
};

} // namespace

ui::View *addMessagePreview(
    Context &ctx, ui::View *parent, const model::Message &m, float bodyH, bool files
) {
    auto *card = parent->add<PreviewCard>();
    card->style().padding(0, 8, 0, 8).spacing(4).noShrink();
    // "<b>name</b>  <time>" (the time in 11 px: tertiary in the forward
    // dialog, secondary in the delete dialog; both read the same here).
    auto *head = card->add<ui::View>();
    head->style().row().padding(12, 0, 12, 0).spacing(8).items(Align::End);
    ui::styledLabel(
        head,
        std::string(authorName(ctx.store(), m)),
        ui::pxFont(15, text::Weight::Bold, ui::color(C::FormText)),
        1
    );
    ui::styledLabel(
        head,
        base::formatTime(model::tsSecs(m.ts)),
        ui::pxFont(
            11, text::Weight::Regular, ui::color(files ? C::FormTextFaint : C::FormTextMuted)
        ),
        1
    );
    head->add<ui::View>()->style().flex(1);
    if (!m.text.empty()) {
        auto *scroll = card->add<ui::ScrollView>();
        scroll->style().height(bodyH);
        scroll->content()->style().padding(12, 0, 0, 0).spacing(2);
        buildBody(ctx, scroll->content(), m.text, {}, scroll);
    }
    if (files)
        for (const model::File &f : m.files()) {
            auto *row = card->add<ui::View>();
            row->style().padding(12, 0, 12, 0).spacing(2);
            if (f.isImage()) { // name, then the image within 300×150
                ui::styledLabel(
                    row,
                    f.name,
                    ui::pxFont(15 * 0.82f, text::Weight::Regular, ui::color(C::FileNameDim)),
                    1
                );
                float       w = float(f.width > 0 ? f.width : 300),
                            h = float(f.height > 0 ? f.height : 150);
                const float s = std::min({1.f, 300 / w, 150 / h});
                auto       *img =
                    row->add<CachedImage>(ctx.images, f.path, ImageCache::Shape::Rounded, 4);
                img->style().size(std::floor(w * s), std::floor(h * s)).alignSelf(Align::Start);
            } else {
                addFileChip(row, f, nullptr, 0);
            }
        }
    return card;
}

// ── Delete message ──────────────────────────────────────────────────────────

ui::Popup *showDeleteMessageDialog(Context &ctx, ui::Window &w, ConvRef conv, Ts ts) {
    const model::Message *m = ctx.store().findMessage(conv, ts);
    if (!m)
        return nullptr;
    auto d = ui::Dialog::confirm(
        tr("Delete message"),
        tr("This action cannot be undone."),
        tr("Delete"),
        V::Danger,
        ui::color(C::FormTextMuted),
        [&](ui::View *content) { addMessagePreview(ctx, content, *m, 160, false); }
    );
    ui::Dialog *raw = d.get();
    d->onAccepted   = [&ctx, conv, ts] { ctx.backend().remove(conv, ts); };
    w.showPopup(std::move(d));
    return raw;
}

// ── Move to thread ──────────────────────────────────────────────────────────

std::string movedMessageText(const Context &ctx, const model::Message &m, bool withNote) {
    std::string out;
    if (withNote) {
        const int64_t     secs = model::tsSecs(m.ts);
        const std::string note =
            arg(tr("Moved from the channel \xC2\xB7 originally posted by %1 on %2 at %3"),
                authorName(ctx.store(), m),
                base::formatDate(secs, base::nowSecs()),
                base::formatTime(secs));
        out = str::concat({"_", note, "_"});
    }
    if (!str::trim(m.text).empty())
        out = out.empty() ? m.text : str::concat({out, "\n\n", m.text});
    // The files go along as links (the original, and with it the upload,
    // is deleted once the copy is in).
    for (const model::File &f : m.files()) {
        if (f.permalink.empty())
            continue;
        const std::string link =
            f.name.empty() ? f.permalink : str::concat({"<", f.permalink, "|", f.name, ">"});
        out = out.empty() ? link : str::concat({out, "\n", link});
    }
    return out;
}

namespace {

class ThreadRow final : public ui::Clickable {
public:
    ThreadRow(Context &ctx, const model::Message &root, int index) : index(index) {
        setLook({C::None, C::None, C::None, C::None, 0});
        style().row().height(60).padding(24, 0, 24, 0).spacing(12).items(Align::Center);
        const std::string author(authorName(ctx.store(), root));
        auto             *av = add<CachedImage>(
            ctx.images, authorAvatar(ctx.store(), root), ImageCache::Shape::Circle
        );
        av->style().size(36, 36).noShrink();
        auto *col = add<ui::View>();
        col->style().flex(1).spacing(1);
        std::string title = plainText(ctx.store(), root.text);
        for (char &c : title)
            if (c == '\n')
                c = ' ';
        if (str::trim(title).empty())
            title = root.files().empty() ? std::string(tr("(no text)")) : root.files().front().name;
        ui::styledLabel(col, title, ui::pxFont(15, text::Weight::Bold, ui::color(C::FormText)), 1);
        const std::string sub = str::concat(
            {author,
             " \xC2\xB7 ",
             base::formatDateTime(model::tsSecs(root.ts)),
             " \xC2\xB7 ",
             root.replyCount == 1 ? std::string(tr("1 reply"))
                                  : arg(tr("%1 replies"), str::number(root.replyCount))}
        );
        ui::styledLabel(
            col, sub, ui::pxFont(15 * 0.88f, text::Weight::Regular, ui::color(C::FormTextMuted)), 1
        );
        key = str::concat({title, " ", author});
    }
    void paint(gfx::Painter &p) override {
        p.fillRect(
            bounds(),
            ui::color(
                selected    ? C::FormHighlightStrong
                : hovered() ? C::FormHighlight
                            : C::FormBg
            )
        );
        View::paint(p);
    }
    int         index;
    std::string key; // what the filter matches (title and author)
    bool        selected = false;
};

class MoveDialog final : public ui::Dialog {
public:
    MoveDialog(Context &ctx, ConvRef conv, Ts ts, std::vector<Ts> roots)
        : ui::Dialog(tr("Move to thread"), 0, Scroll::Disabled), _ctx(ctx), _conv(conv), _ts(ts) {
        content()->style().spacing(8);
        ui::styledLabel(
            content(),
            tr("Pick a thread in this channel. The message is posted there again by you, and the "
               "original is deleted."),
            ui::pxFont(12, text::Weight::Regular, ui::color(C::FormTextMuted))
        );
        _filter = content()->add<ui::TextField>(
            tr("Filter threads\xE2\x80\xA6"),
            ui::TextField::Size::Normal,
            uint16_t(gfx::Icon::Search)
        );
        auto *stack = content()->add<ui::View>();
        stack->style().stack().height(280);
        _list = stack->add<ui::ScrollView>();
        for (Ts r : roots)
            if (const model::Message *m = ctx.store().findMessage(conv, r)) {
                auto *row    = _list->content()->add<ThreadRow>(ctx, *m, int(_roots.size()));
                row->onClick = [this, row] { select(row->index); };
                _rows.push_back(row);
                _roots.push_back(r);
            }
        _empty = ui::styledLabel(
            stack,
            _rows.empty() ? tr("No threads in this channel's loaded history yet.")
                          : tr("No threads match."),
            ui::pxFont(14, text::Weight::Regular, ui::color(C::FormTextFaint))
        );
        _empty->setAlign(text::LayoutOptions::Align::Center);
        _empty->style().alignSelf(Align::Center);
        _note = content()->add<ui::CheckBox>(tr("Add a note with the original author and time"));
        _move = makeButton(tr("Move"), V::Primary);
        addButtonRow(_move, makeButton(tr("Cancel"), V::Secondary));
        _move->onClick           = [this] { go(); };
        _filter->edit().onChange = [this] { refilter(); };
        _filter->edit().onSubmit = [this] { return go(), true; };
        _filter->edit().onKey    = [this](const ui::Event &e) {
            if (e.type != ui::EventType::KeyDown ||
                (e.key != plat::Key::Up && e.key != plat::Key::Down))
                return false;
            moveSelection(e.key == plat::Key::Down ? 1 : -1);
            return true;
        };
        refilter();
    }
    void focusFilter() { _filter->edit().focus(); }

private:
    void refilter() {
        // Any case, any script ("örjan" finds "Örjan").
        const std::string q     = utf8::foldCase(str::trim(_filter->edit().text()));
        int               first = -1;
        for (ThreadRow *r : _rows) {
            const bool on = q.empty() || utf8::containsFoldedNeedle(r->key, q);
            r->setVisible(on);
            if (on && first < 0)
                first = r->index;
        }
        _empty->setVisible(first < 0);
        _list->setVisible(first >= 0);
        select(first); // row 0 preselected after every change
    }
    void select(int i) {
        _sel = i;
        for (ThreadRow *r : _rows) {
            r->selected = r->index == i;
            r->update();
        }
        _move->setEnabled(i >= 0);
        if (i >= 0)
            _list->ensureVisible(_rows[size_t(i)]);
    }
    void moveSelection(int dir) { // wraps at both ends, over the visible rows
        std::vector<int> vis;
        for (ThreadRow *r : _rows)
            if (r->visible())
                vis.push_back(r->index);
        if (vis.empty())
            return;
        auto it = std::find(vis.begin(), vis.end(), _sel);
        int  k  = it == vis.end() ? 0 : int(it - vis.begin()) + dir;
        k       = (k % int(vis.size()) + int(vis.size())) % int(vis.size());
        select(vis[size_t(k)]);
    }
    void go() {
        if (_sel < 0)
            return;
        const model::Message *m = _ctx.store().findMessage(_conv, _ts);
        if (!m || m->pending)
            return;
        const Ts          root = _roots[size_t(_sel)];
        const std::string text = movedMessageText(_ctx, *m, _note->checked());
        Context          &ctx  = _ctx;
        const ConvRef     conv = _conv;
        const Ts          ts   = _ts;
        // The original goes only once the copy is in the thread.
        ctx.backend().send(conv, text, root, [&ctx, conv, ts](bool ok, const std::string &) {
            if (ok) {
                ctx.backend().remove(conv, ts);
                return;
            }
            // The send path has said why; this says what it means for the move.
            if (ctx.backend.onError)
                ctx.backend.onError(
                    tr("Couldn't move the message \xE2\x80\x94 the original is still in place.")
                );
        });
        if (ctx.openThread)
            ctx.openThread(conv, root);
        accept();
    }

    Context                 &_ctx;
    ConvRef                  _conv;
    Ts                       _ts;
    ui::TextField           *_filter = nullptr;
    ui::ScrollView          *_list   = nullptr;
    ui::Label               *_empty  = nullptr;
    ui::CheckBox            *_note   = nullptr;
    Button                  *_move   = nullptr;
    std::vector<ThreadRow *> _rows;
    std::vector<Ts>          _roots;
    int                      _sel = -1;
};

} // namespace

ui::Popup *
showMoveToThreadDialog(Context &ctx, ui::Window &w, ConvRef conv, Ts ts, std::vector<Ts> roots) {
    auto  d   = std::make_unique<MoveDialog>(ctx, conv, ts, std::move(roots));
    auto *raw = d.get();
    w.showPopup(std::move(d));
    raw->focusFilter();
    return raw;
}

// ── Reminder ────────────────────────────────────────────────────────────────

ui::Popup *showReminderDialog(Context &ctx, ui::Window &w, ConvRef conv, Ts ts) {
    auto              d  = std::make_unique<ui::Dialog>(tr("Reminder"));
    ui::View         *c  = d->content();
    const text::Style lb = ui::pxFont(15, text::Weight::Semibold, ui::color(C::FormTextMuted));
    c->style().spacing(4);
    ui::styledLabel(c, tr("When"), lb);
    auto *date = c->add<ui::DateTimeField>(ui::DateTimeField::Kind::Date);
    c->add<ui::View>()->style().height(0); // + the 4 px spacing
    ui::styledLabel(c, tr("Time"), lb);
    auto *time = c->add<ui::DateTimeField>(ui::DateTimeField::Kind::Time);
    c->add<ui::View>()->style().height(4);
    // Default: now + 1 h, rounded up to the next 5 minutes.
    const int64_t   now         = base::nowSecs();
    base::CivilTime t           = base::localTime(now + 3600);
    const int       add         = (5 - t.minute % 5) % 5;
    t                           = base::localTime(now + 3600 + add * 60 - t.second);
    const base::CivilTime today = base::localTime(now);
    date->setMinimumDate(today.year, today.month, today.day);
    date->setDate(t.year, t.month, t.day);
    time->setTime(t.hour, t.minute);
    auto *save = d->makeButton(tr("Save"), V::Primary);
    d->addButtonRow(save, d->makeButton(tr("Cancel"), V::Secondary));
    ui::Dialog *raw = d.get();
    save->onClick   = [raw] { raw->accept(); };
    d->onAccepted   = [&ctx, conv, ts, date, time] {
        // Due never earlier than a minute from now.
        const int64_t due =
            base::fromLocal(date->year(), date->month(), date->day(), time->hour(), time->minute());
        ctx.backend().setReminder(conv, ts, std::max(due, base::nowSecs() + 60));
    };
    w.showPopup(std::move(d));
    date->focus();
    return raw;
}

// ── Table viewer ────────────────────────────────────────────────────────────

std::vector<std::vector<std::string>> parseCsv(std::string_view s) {
    if (s.substr(0, 3) == "\xEF\xBB\xBF")
        s.remove_prefix(3);
    // The delimiter, from the first line (outside quotes).
    int  tabs = 0, commas = 0, semis = 0;
    bool q = false;
    for (char c : s) {
        if (c == '"')
            q = !q;
        else if (!q && (c == '\n' || c == '\r'))
            break;
        else if (!q)
            tabs += c == '\t', commas += c == ',', semis += c == ';';
    }
    const char delim = tabs > commas && tabs > semis ? '\t' : semis > commas ? ';' : ',';
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string>              row;
    std::string                           cell;
    bool                                  quoted = false, any = false;
    auto                                  endRow = [&] {
        if (any || !cell.empty() || !row.empty()) {
            row.push_back(std::move(cell));
            rows.push_back(std::move(row));
        }
        row.clear();
        cell.clear();
        any = false;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted) {
            if (c == '"' && i + 1 < s.size() && s[i + 1] == '"')
                cell += '"', ++i;
            else if (c == '"')
                quoted = false;
            else
                cell += c;
        } else if (c == '"' && cell.empty()) {
            quoted = any = true;
        } else if (c == delim) {
            row.push_back(std::move(cell));
            cell.clear();
            any = true;
        } else if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n')
                ++i;
            endRow();
        } else {
            cell += c;
            any = true;
        }
    }
    endRow();
    return rows;
}

namespace {

// rows: plain text (a CSV file), or mrkdwn with a Context (a Block Kit
// table: cells, mentions, links and emoji as in the
// message).
class TableViewer final : public ui::Popup {
public:
    TableViewer(std::vector<std::vector<std::string>> rows, Context *ctx)
        : _rows(std::move(rows)), _ctx(ctx) {
        if (_ctx)
            _anim = std::make_unique<EmojiFrameTimer>(*_ctx, *this);
        setCard(false);
        setAnchor({}, Place::Fill);
        setPaintOutset(0);
        setFocusable(true);
        style().dir = ui::Dir::None;
    }
    void layout() override {
        if (width() != _laidW) { // the table is fitted to the width only
            _laidW = width();
            _built = false;
        }
    }
    void styleChanged() override {
        _grid  = TableGrid(); // the cells' colours and sizes are the style's
        _built = false;
        Popup::styleChanged();
    }
    void paint(gfx::Painter &p) override {
        p.fillRect(bounds(), ui::color(C::ViewerBackdrop));
        build();
        const ui::RectF c = card();
        p.fillRoundRect(c, 8, ui::color(C::FormBg));
        p.save();
        p.clipRect({c.x + 12, c.y + 12, c.w - 24, c.h - 24});
        p.translate(c.x + 12, c.y + 12 - _scroll);
        // Only the rows in the card (a CSV has hundreds).
        _grid.paint(p, *this, 8, _scroll, _scroll + c.h - 24, _ctx, _images, _anim.get());
        p.restore();
    }
    bool onEvent(ui::Event &e) override {
        const ui::RectF c   = card();
        const float     max = std::max(0.f, _docH - (c.h - 24));
        auto            to  = [&](float v) {
            _scroll = std::clamp(v, 0.f, max);
            update();
        };
        switch (e.type) {
        // The cells' links work as in the message (TableView's).
        case ui::EventType::PointerMove:
        case ui::EventType::PointerLeave: {
            ui::RectF      cell = windowRect();
            const uint32_t id   = e.type == ui::EventType::PointerMove ? linkAt(e.pos, &cell) : 0;
            if (_ctx && _hover.move(*_ctx, _targets, id, e.windowPos, cell, [&] {
                    return _grid.linkText(id);
                })) {
                _grid.setUnderlinedLink(_hover.underlined());
                update();
                if (window())
                    window()->rearmTooltip();
            }
            return Popup::onEvent(e);
        }
        case ui::EventType::PointerDown:
            if (!c.contains(e.pos))
                close();
            else if (e.button == plat::Button::Left)
                _pressedLink = linkAt(e.pos);
            return true;
        case ui::EventType::PointerUp: {
            const uint32_t pressed = std::exchange(_pressedLink, 0);
            ui::RectF      cell;
            if (pressed && linkAt(e.pos, &cell) == pressed) {
                const LinkTarget tg = _targets[pressed - 1];
                ui::Window      *w  = window();
                // Going to a channel or a message leaves the viewer; a URL
                // or a profile card keeps it.
                if (tg.kind != mrkdwn::Kind::Link && tg.kind != mrkdwn::Kind::User)
                    close();
                activateTarget(*_ctx, tg, w, e.windowPos, cell);
            }
            return true;
        }
        case ui::EventType::ContextMenu: {
            const uint32_t id = e.raw ? linkAt(e.pos) : 0;
            return id && window() &&
                   showTargetMenu(*_ctx, _targets[id - 1], *window(), e.windowPos);
        }
        case ui::EventType::Scroll:
            to(_scroll - e.dy * (e.precise ? 1.f : 40.f) / 2);
            return true;
        case ui::EventType::KeyDown:
            switch (e.key) {
            case plat::Key::Escape:
                close();
                return true;
            case plat::Key::Up:
                to(_scroll - 40);
                return true;
            case plat::Key::Down:
                to(_scroll + 40);
                return true;
            case plat::Key::PageUp:
                to(_scroll - std::max(40.f, c.h - 48));
                return true;
            case plat::Key::PageDown:
                to(_scroll + std::max(40.f, c.h - 48));
                return true;
            case plat::Key::Home:
                to(0);
                return true;
            case plat::Key::End:
                to(max);
                return true;
            default:
                return true;
            }
        default:
            return Popup::onEvent(e);
        }
    }

    uint8_t cursorAt(ui::PointF local) const override {
        return linkAt(local) ? uint8_t(plat::Cursor::Hand) : Popup::cursorAt(local);
    }
    std::string tooltip() const override { return _hover.tip(); }
    ui::RectF   tooltipAnchor() const override { return _hover.anchor(); }
    bool        tooltipImmediate() const override { return true; }

private:
    // The link under a local point (0: none, or a CSV's plain cells) and
    // its cell (window coordinates).
    uint32_t linkAt(ui::PointF local, ui::RectF *cell = nullptr) const {
        const ui::RectF c = card();
        const ui::RectF view{c.x + 12, c.y + 12, c.w - 24, c.h - 24}; // the table's clip
        if (!_ctx || !_built || !view.contains(local))
            return 0;
        const float    y0 = view.y - _scroll + 8; // the grid's top
        ui::RectF      box;
        const uint32_t id = _grid.linkAt({local.x - view.x, local.y - y0}, &box);
        if (!id || id > _targets.size())
            return 0;
        if (cell) {
            const ui::PointF o = mapToWindow({box.x + view.x, box.y + y0});
            *cell              = {o.x, o.y, box.w, box.h};
        }
        return id;
    }
    ui::RectF card() const {
        const float w = std::min(_idealW + 24, width() - 96),
                    h = std::min(_docH + 24, height() - 96);
        return {std::round((width() - w) / 2), std::round((height() - h) / 2), w, h};
    }
    void build() {
        if (_built)
            return;
        _built            = true;
        const float textW = std::max(50.f, width() - 96 - 24);
        const float k     = windowScale();
        if (_grid.scale() != k) {
            // Row 0 is the header, in bold. Custom emoji boxes are numbered
            // across the whole table.
            std::vector<std::vector<text::AttributedText>> texts(_rows.size());
            _images.clear();
            _targets.clear();
            for (size_t r = 0; r < _rows.size(); ++r)
                for (size_t c = 0; c < _rows[r].size(); ++c) {
                    text::AttributedText &t = texts[r].emplace_back();
                    if (_ctx) {
                        RichOptions o;
                        o.font  = r == 0 ? ui::Font::BodyBold : ui::Font::Body;
                        o.color = C::FormText;
                        t       = richText(*_ctx, _rows[r][c], o, &_images, &_targets);
                        ui::resolveSpans(t);
                    } else {
                        t.append(
                            _rows[r][c],
                            ui::pxFont(
                                15,
                                r == 0 ? text::Weight::Bold : text::Weight::Regular,
                                ui::color(C::FormText)
                            )
                        );
                    }
                }
            _grid.setCells(std::move(texts), k);
        }
        // Automatic table layout, roughly: natural widths, shrunk in
        // proportion (wrapping) when they don't fit.
        _grid.fit(textW, 0, 0);
        _idealW = _grid.naturalWidth();
        _docH   = _grid.height() + 16; // the table's 8-px margins
    }

    std::vector<std::vector<std::string>> _rows;
    Context                              *_ctx = nullptr;
    std::unique_ptr<EmojiFrameTimer>      _anim;
    std::vector<std::string>              _images;  // emoji box id i: [i - 1]
    std::vector<LinkTarget>               _targets; // link id i: [i - 1]
    TargetHover                           _hover;
    uint32_t                              _pressedLink = 0;
    TableGrid                             _grid;
    float                                 _idealW = 0, _docH = 0, _scroll = 0, _laidW = -1;
    bool                                  _built = false;
};

} // namespace

std::vector<std::vector<std::string>> readCsvFile(const std::string &path) {
    std::string text;
    if (!file::readAll(path, &text))
        return {};
    return parseCsv(text);
}

ui::Popup *
showTableViewer(ui::Window &w, Context &ctx, std::vector<std::vector<std::string>> cells) {
    auto  v   = std::make_unique<TableViewer>(std::move(cells), &ctx);
    auto *raw = v.get();
    w.showPopup(std::move(v));
    return raw;
}

ui::Popup *showTableViewer(ui::Window &w, std::vector<std::vector<std::string>> rows) {
    if (rows.empty())
        rows = {{tr("This file is empty")}};
    constexpr size_t kMaxRows = 400;
    if (rows.size() > kMaxRows) {
        const size_t n = rows.size();
        rows.resize(kMaxRows);
        rows.push_back({arg(tr("Showing the first %1 of %2 rows"), "400", str::number(n))});
    }
    auto  v   = std::make_unique<TableViewer>(std::move(rows), nullptr);
    auto *raw = v.get();
    w.showPopup(std::move(v));
    return raw;
}

} // namespace screens
