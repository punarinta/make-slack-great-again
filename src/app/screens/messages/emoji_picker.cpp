#include "app/screens/messages/emoji_picker.h"

#include "app/mrkdwn/emoji.h"
#include "app/screens/common/icon_button.h"
#include "app/screens/messages/image_cache.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace ui;
using gfx::Icon;
using i18n::tr;

namespace screens {

namespace {

// The picker's and its grid's metrics.
constexpr float kWidth = 354, kHeight = 460, kCell = 36, kGlyph = 22, kMargin = 6, kHeaderH = 30;
constexpr int   kCols = 9, kRecentMax = 27; // 3 rows of 9

// What was picked, newest first, and the skin tone (kept in the shell's
// settings: restoreState / setStateObserver).
std::vector<std::string> &recents() {
    static std::vector<std::string> r;
    return r;
}
int g_tone = 0;

EmojiPicker::StateObserver &stateObserver() {
    static EmojiPicker::StateObserver fn;
    return fn;
}

void stateChanged() {
    if (stateObserver())
        stateObserver()(recents(), g_tone);
}

std::string toneGlyph(int tone) {
    return tone >= 2 && tone <= 6
               ? emoji::toUnicode(str::concat({"skin-tone-", str::number(int64_t(tone))}))
               : std::string();
}

} // namespace

// The virtual, sectioned grid: header rows and rows of 9 cells, painted only
// where the viewport shows them.
class EmojiGrid final : public View {
public:
    struct Cell {
        std::string name, glyph, image; // image: a custom emoji's path
        bool        skinnable = false;
    };
    struct Section {
        std::string label;
        int         first = 0, count = 0;
    };

    explicit EmojiGrid(Context &ctx) : _ctx(ctx) { setHoverRepaint(true); }
    ~EmojiGrid() override { _ctx.images.forget(this); }

    void setContent(std::vector<Cell> cells, std::vector<Section> sections) {
        _cells    = std::move(cells);
        _sections = std::move(sections);
        _glyphs.clear();
        _glyphs.resize(_cells.size());
        _headers.clear();
        _headers.resize(_sections.size());
        _sel = _hover = -1;
        relayout();
    }
    void relayout() {
        _rows.clear();
        float y = kMargin;
        for (size_t s = 0; s < _sections.size(); ++s) {
            const Section &sec = _sections[s];
            if (sec.count <= 0)
                continue;
            if (!sec.label.empty()) {
                _rows.push_back({y, kHeaderH, true, int(s), 0, 0});
                y += kHeaderH;
            }
            for (int off = 0; off < sec.count; off += kCols) {
                _rows.push_back(
                    {y, kCell, false, int(s), sec.first + off, std::min(kCols, sec.count - off)}
                );
                y += kCell;
            }
        }
        _contentH = y + kMargin;
        style().height(_contentH);
        invalidateLayout();
        update();
    }
    float sectionTop(int s) const {
        for (const Row &r : _rows)
            if (r.section == s)
                return r.y;
        return 0;
    }
    int sectionAt(float y) const { // the section at the top of the viewport
        int s = -1;
        for (const Row &r : _rows) {
            if (r.y > y + kMargin + 1)
                break;
            s = r.section;
        }
        return s;
    }
    std::string emitted(const Cell &c) const {
        return g_tone && c.skinnable
                   ? str::concat({c.name, "::skin-tone-", str::number(int64_t(g_tone))})
                   : c.name;
    }
    int  selected() const { return _sel; }
    void setSelected(int i) {
        _sel = i >= 0 && size_t(i) < _cells.size() ? i : -1;
        if (_sel >= 0 && scroll) {
            const RectF r = cellRect(_sel);
            if (r.y < scroll->scrollOffset())
                scroll->scrollTo(r.y);
            else if (r.y + r.h > scroll->scrollOffset() + scroll->height())
                scroll->scrollTo(r.y + r.h - scroll->height());
        }
        update();
    }
    void moveSelection(int dCol, int dRow) {
        if (_cells.empty())
            return;
        if (_sel < 0)
            return setSelected(0);
        if (dCol)
            return setSelected(std::clamp(_sel + dCol, 0, int(_cells.size()) - 1));
        const int r = rowOf(_sel);
        if (r < 0)
            return;
        const int col = _sel - _rows[size_t(r)].cellStart;
        for (int rr = r + dRow; rr >= 0 && rr < int(_rows.size()); rr += dRow) {
            if (_rows[size_t(rr)].header)
                continue;
            return setSelected(
                _rows[size_t(rr)].cellStart + std::min(col, _rows[size_t(rr)].cellCount - 1)
            );
        }
    }
    const std::vector<Cell> &cells() const { return _cells; }
    void                     resetGlyphs() { // the skin tone changed
        for (auto &l : _glyphs)
            l.reset();
        update();
    }
    const std::vector<Section> &sections() const { return _sections; }

    std::function<void(const std::string &)> onPick;
    ScrollView                              *scroll = nullptr;

    SizeF measureContent(float availW, float) override { return {availW, _contentH}; }
    bool  onEvent(Event &e) override {
        switch (e.type) {
        case EventType::PointerMove:
        case EventType::PointerEnter: {
            const int c = cellAt(e.pos);
            if (c != _hover) {
                _hover = c;
                update();
            }
            return false;
        }
        case EventType::PointerLeave:
            _hover = -1;
            update();
            return false;
        case EventType::PointerDown: {
            const int c = cellAt(e.pos);
            if (c < 0 || e.button != plat::Button::Left)
                return false;
            _sel = c;
            if (onPick)
                onPick(emitted(_cells[size_t(c)]));
            return true;
        }
        default:
            return false;
        }
    }
    uint8_t cursorAt(PointF p) const override {
        return cellAt(p) >= 0 ? uint8_t(plat::Cursor::Hand) : View::cursorAt(p);
    }
    std::string tooltip() const override {
        return _hover >= 0 ? str::concat({":", _cells[size_t(_hover)].name, ":"}) : std::string();
    }
    void paint(gfx::Painter &p) override {
        const float top   = scroll ? scroll->scrollOffset() : 0,
                    vh    = scroll ? scroll->height() : height();
        const float scale = windowScale();
        std::string tg; // the tone's glyph, once a cell is shaped
        for (const Row &r : _rows) {
            if (r.y + r.h < top || r.y > top + vh)
                continue;
            if (r.header) {
                auto &l = _headers[size_t(r.section)];
                if (!l) {
                    text::Style st = font(Font::Caption, C::FormTextMuted);
                    st.weight      = text::Weight::Bold;
                    l = text::layoutPlain(_sections[size_t(r.section)].label, st, scale);
                }
                l->paint(p, snapPx(PointF{kMargin + 2, r.y + std::floor((r.h - l->height()) / 2)}));
                continue;
            }
            for (int col = 0; col < r.cellCount; ++col) {
                const int   idx = r.cellStart + col;
                const RectF cr{kMargin + col * kCell, r.y, kCell, kCell};
                if (idx == _sel || idx == _hover)
                    p.fillRoundRect(
                        {cr.x + 2, cr.y + 2, cr.w - 4, cr.h - 4}, 5, color(C::FormHighlight)
                    );
                const Cell &c = _cells[size_t(idx)];
                if (!c.glyph.empty()) {
                    auto &l = _glyphs[size_t(idx)];
                    if (!l) {
                        if (g_tone && c.skinnable && tg.empty())
                            tg = toneGlyph(g_tone);
                        text::AttributedText t;
                        text::Style          st = font(Font::Body);
                        st.size                 = kGlyph;
                        t.append(g_tone && c.skinnable ? c.glyph + tg : c.glyph, st);
                        text::LayoutOptions o;
                        o.lineHeight = 1.0f;
                        l            = text::Layout::build(std::move(t), o, scale);
                    }
                    l->paint(
                        p,
                        snapPx(
                            PointF{
                                cr.x + std::floor((cr.w - l->width()) / 2),
                                cr.y + std::floor((cr.h - l->height()) / 2)
                            }
                        )
                    );
                } else if (!c.image.empty()) {
                    const int px = int(std::lround(24 * scale));
                    if (auto b = _ctx.images.get(ImageCache::Ref{c.image, px, px}, this))
                        p.drawBitmap(b->view(), {cr.x + 6, cr.y + 6, 24, 24});
                }
            }
        }
    }

private:
    struct Row {
        float y, h;
        bool  header;
        int   section, cellStart, cellCount;
    };
    RectF cellRect(int idx) const {
        for (const Row &r : _rows)
            if (!r.header && idx >= r.cellStart && idx < r.cellStart + r.cellCount)
                return {kMargin + (idx - r.cellStart) * kCell, r.y, kCell, kCell};
        return {};
    }
    int rowOf(int idx) const {
        for (size_t i = 0; i < _rows.size(); ++i)
            if (!_rows[i].header && idx >= _rows[i].cellStart &&
                idx < _rows[i].cellStart + _rows[i].cellCount)
                return int(i);
        return -1;
    }
    int cellAt(PointF p) const {
        for (const Row &r : _rows) {
            if (r.header || p.y < r.y || p.y >= r.y + r.h)
                continue;
            const int col = int((p.x - kMargin) / kCell);
            return p.x >= kMargin && col < r.cellCount ? r.cellStart + col : -1;
        }
        return -1;
    }

    Context                                   &_ctx;
    std::vector<Cell>                          _cells;
    std::vector<Section>                       _sections;
    std::vector<Row>                           _rows;
    std::vector<std::unique_ptr<text::Layout>> _glyphs, _headers;
    float                                      _contentH = 0;
    int                                        _sel = -1, _hover = -1;
};

namespace {

// A category tab: the icon, a 2 px accent underline while its section is on
// top; a highlight wash on hover.
class CatTab final : public IconButton {
public:
    explicit CatTab(Icon i) : IconButton(i, 18, C::FormIcon) {
        setLook({C::None, C::FormHighlight, C::FormHighlight, C::None, 4});
        style().flex(1).height(28);
        setRole(Role::Tab);
    }
    void paintOver(gfx::Painter &p) override {
        if (checked())
            p.fillRect({0, height() - 2, width(), 2}, color(C::Accent));
    }
};

struct CatDef {
    const char *id;
    Icon        icon;
};
constexpr CatDef kCatIcons[] = {
    {"people", Icon::Smile},
    {"nature", Icon::Leaf},
    {"food", Icon::Apple},
    {"travel", Icon::Plane},
    {"activity", Icon::Volleyball},
    {"objects", Icon::Lightbulb},
    {"symbols", Icon::Shapes},
    {"flags", Icon::Flag},
};

} // namespace

EmojiPicker::EmojiPicker(Context &ctx, Pick onPick) : _ctx(ctx), _onPick(std::move(onPick)) {
    setCard(false);
    setBackground(C::FormBg, 8);
    setBorder(C::FormDividerStrong);
    style().size(kWidth, kHeight).padding(8).spacing(8);
    _catBar = add<View>();
    _catBar->style().row().noShrink();
    // "Search all emoji": a StyledLineEdit with its search icon.
    auto *box =
        add<TextField>(tr("Search all emoji"), TextField::Size::Normal, uint16_t(Icon::Search));
    box->setQuiet();
    _search           = &box->edit();
    _search->onChange = [this] { filter(str::trim(_search->text())); };
    _search->onKey    = [this](const Event &e) { return searchKey(e); };
    _scroll           = add<ScrollView>();
    _scroll->style().flex(1);
    _scroll->content()->style().padding(0);
    _grid             = _scroll->content()->add<EmojiGrid>(ctx);
    _grid->scroll     = _scroll;
    _grid->onPick     = [this](const std::string &n) { pick(n); };
    _scroll->onScroll = [this] { syncTabs(); };
    // Bottom: the skin-tone selector, right-aligned; the swatches replace the
    // button while choosing.
    auto *bottom      = add<View>();
    bottom->style().row().padding(2, 0).spacing(2).items(Align::Center).noShrink();
    bottom->add<View>()->style().flex(1);
    _toneRow = bottom->add<View>();
    _toneRow->style().row().spacing(2);
    _toneRow->setVisible(false);
    const std::string hand = emoji::toUnicode("hand");
    for (int tone : {0, 2, 3, 4, 5, 6}) {
        auto *b = _toneRow->add<Clickable>();
        b->setLook({C::None, C::FormHighlight, C::FormHighlight, C::FormHighlightStrong, 4});
        b->style().size(26, 26).stack().items(Align::Center);
        b->add<Label>(hand + toneGlyph(tone), Font::Body)->setHitTransparent(true);
        b->setChecked(tone == g_tone);
        b->onClick = [this, tone] {
            setSkinTone(tone);
            _toneRow->setVisible(false);
            _skinBtn->setVisible(true);
        };
    }
    auto *skin = bottom->add<Clickable>();
    skin->setLook({C::None, C::None, C::None, C::None, 0});
    skin->style().row().padding(6, 3).spacing(4).items(Align::Center);
    skin->add<Label>(hand + toneGlyph(g_tone), Font::Body)->setHitTransparent(true);
    skin->add<Label>(tr("Skin tone"), Font::Caption, C::FormTextMuted)->setHitTransparent(true);
    skin->onClick = [this] {
        _toneRow->setVisible(true);
        _skinBtn->setVisible(false);
    };
    _skinBtn = skin;
    filter({});
}

View *EmojiPicker::addGlyph(View *parent, Context &ctx, const std::string &name, float size) {
    const model::Store::EmojiGlyph g = ctx.store().emojiFor(name);
    if (!g.image.empty()) {
        auto *img = parent->add<CachedImage>(ctx.images, g.image);
        img->setPlaceholder(C::None);
        img->style().size(size, size);
        return img;
    }
    if (!g.unicode.empty())
        return parent->add<Label>(g.unicode, Font::Body);
    // Unknown here (another workspace's emoji): the name, cut to the cell.
    auto *l = parent->add<Label>(":" + name + ":", Font::Small, C::FormTextMuted);
    l->setMaxLines(1);
    l->style().maxW = size;
    return l;
}

std::string EmojiPicker::withSkinTone(const std::string &name) {
    if (!g_tone || name.find("::") != std::string::npos || !emoji::supportsSkinTone(name))
        return name;
    return str::concat({name, "::skin-tone-", str::number(int64_t(g_tone))});
}

EmojiPicker *EmojiPicker::show(Window &w, RectF anchor, Context &ctx, Pick onPick) {
    auto p = std::make_unique<EmojiPicker>(ctx, std::move(onPick));
    p->setAnchor(anchor, Place::Above);
    auto *raw = static_cast<EmojiPicker *>(w.showPopup(std::move(p)));
    raw->_search->focus();
    return raw;
}

int EmojiPicker::selected() const {
    return _grid->selected();
}

size_t EmojiPicker::cellCount() const {
    return _grid->cells().size();
}

const std::string &EmojiPicker::cellName(size_t i) const {
    return _grid->cells()[i].name;
}

std::vector<std::string> EmojiPicker::sections() const {
    std::vector<std::string> out;
    for (const auto &s : _grid->sections())
        out.push_back(s.label);
    return out;
}

int EmojiPicker::skinTone() const {
    return g_tone;
}

void EmojiPicker::restoreState(std::vector<std::string> recent, int tone) {
    if (recent.size() > size_t(kRecentMax))
        recent.resize(kRecentMax);
    recents() = std::move(recent);
    g_tone    = tone >= 2 && tone <= 6 ? tone : 0;
}

void EmojiPicker::setStateObserver(StateObserver fn) {
    stateObserver() = std::move(fn);
}

void EmojiPicker::setSkinTone(int tone) {
    const int before = g_tone;
    g_tone           = tone >= 2 && tone <= 6 ? tone : 0;
    if (g_tone != before)
        stateChanged();
    // The button's hand follows; the swatch row marks the pick.
    if (auto *hand = static_cast<Label *>(_skinBtn->child(0)))
        hand->setText(emoji::toUnicode("hand") + toneGlyph(g_tone));
    for (size_t i = 0; i < _toneRow->childCount(); ++i)
        static_cast<Clickable *>(_toneRow->child(i))
            ->setChecked((i == 0 && !g_tone) || (i > 0 && int(i) + 1 == g_tone));
    _grid->resetGlyphs();
}

// The picker's keys on its search field: Escape closes, Enter picks the
// selected cell, Up/Down move a row keeping the column, Left/Right step
// through the cells once the caret is at that end of the text.
bool EmojiPicker::searchKey(const Event &e) {
    using plat::Key;
    if (e.type != EventType::KeyDown)
        return false;
    switch (e.key) {
    case Key::Enter:
    case Key::KpEnter:
        if (_grid->selected() >= 0)
            pick(_grid->emitted(_grid->cells()[size_t(_grid->selected())]));
        return true;
    case Key::Up:
    case Key::Down:
        _grid->moveSelection(0, e.key == Key::Down ? 1 : -1);
        return true;
    case Key::Left:
        if (_search->caret() != 0 || _search->hasSelection())
            return false;
        _grid->moveSelection(-1, 0);
        return true;
    case Key::Right:
        if (_search->caret() != _search->text().size() || _search->hasSelection())
            return false;
        _grid->moveSelection(1, 0);
        return true;
    default:
        return false;
    }
}

bool EmojiPicker::onEvent(Event &e) {
    // The picker acts as its own popup window: while it is up, no key reaches
    // the window behind it (Tab still moves focus inside).
    if (e.type == EventType::KeyDown && e.key != plat::Key::Escape && e.key != plat::Key::Tab)
        return true;
    return Popup::onEvent(e);
}

void EmojiPicker::pick(const std::string &name) {
    // The tone-stripped name leads "Frequently used".
    if (_remember) {
        const std::string base = name.substr(0, name.find("::"));
        auto             &r    = recents();
        r.erase(std::remove(r.begin(), r.end(), base), r.end());
        r.insert(r.begin(), base);
        if (r.size() > size_t(kRecentMax))
            r.resize(kRecentMax);
        stateChanged();
    }
    Pick cb = std::move(_onPick);
    close(); // deferred destruction: members stay valid for the rest of this call
    if (cb)
        cb(name);
}

void EmojiPicker::filter(std::string_view q) {
    std::vector<EmojiGrid::Cell>    cells;
    std::vector<EmojiGrid::Section> sections;
    _tabSection.clear();
    std::vector<Icon> icons;
    _searching                  = !q.empty();
    const model::Store &store   = _ctx.store();
    const auto         &customs = store.customEmojiImages();
    auto add = [&](std::string label, Icon icon, bool tab, std::vector<EmojiGrid::Cell> sec) {
        if (sec.empty())
            return;
        sections.push_back({std::move(label), int(cells.size()), int(sec.size())});
        if (tab) {
            _tabSection.push_back(int(sections.size()) - 1);
            icons.push_back(icon);
        }
        for (auto &c : sec)
            cells.push_back(std::move(c));
    };
    if (_searching) {
        // One "Search results" section: workspace emoji first, then every
        // built-in whose name contains the query.
        // The query folded once, the names folded once (the workspace's per
        // picker until they change; built-in names are folded as they are).
        const std::string fq = utf8::foldCase(q);
        if (_customRev != store.customEmojiRevision()) {
            _customRev = store.customEmojiRevision();
            _customFolded.clear();
            _customFolded.reserve(customs.size());
            for (const auto &c : customs)
                _customFolded.push_back(utf8::foldCase(c.name));
        }
        std::vector<EmojiGrid::Cell> hits;
        for (size_t i = 0; i < customs.size(); ++i)
            if (utf8::containsPrefolded(_customFolded[i], fq))
                hits.push_back(
                    {std::string(customs[i].name), {}, std::string(customs[i].image), false}
                );
        emoji::forEachName([&](std::string_view n) {
            if (utf8::containsPrefolded(n, fq)) // only a hit's glyph is decoded
                hits.push_back(
                    {std::string(n), emoji::toUnicode(n), {}, emoji::supportsSkinTone(n)}
                );
            return true;
        });
        add(tr("Search results"), Icon::Search, false, std::move(hits));
    } else {
        std::vector<EmojiGrid::Cell> freq;
        for (const std::string &n : recents()) {
            if (std::string u = emoji::toUnicode(n); !u.empty()) {
                freq.push_back({n, std::move(u), {}, emoji::supportsSkinTone(n)});
                continue;
            }
            // customEmojiImages is sorted by name (and names are unique).
            const auto it = std::lower_bound(
                customs.begin(), customs.end(), n, [](const auto &c, const std::string &key) {
                    return c.name < key;
                }
            );
            if (it != customs.end() && it->name == n)
                freq.push_back({n, {}, std::string(it->image), false});
        }
        add(tr("Frequently used"), Icon::Clock, true, std::move(freq));
        std::vector<std::pair<std::string, std::string>> entries;
        for (int c = 0; c < emoji::categoryCount(); ++c) {
            entries.clear();
            emoji::categoryEntries(c, entries);
            std::vector<EmojiGrid::Cell> sec;
            sec.reserve(entries.size());
            for (auto &[n, u] : entries) {
                const bool skin = emoji::supportsSkinTone(n);
                sec.push_back({std::move(n), std::move(u), {}, skin});
            }
            Icon icon = Icon::Smile;
            for (const CatDef &d : kCatIcons)
                if (std::string_view(d.id) == emoji::categoryId(c))
                    icon = d.icon;
            add(emoji::categoryLabel(c), icon, true, std::move(sec));
        }
        std::vector<EmojiGrid::Cell> custom;
        custom.reserve(customs.size());
        for (const auto &c : customs)
            custom.push_back({std::string(c.name), {}, std::string(c.image), false});
        add(tr("Custom"), Icon::SlackMark, true, std::move(custom));
    }
    _grid->setContent(std::move(cells), std::move(sections));
    _scroll->scrollTo(0);
    // The category bar.
    _catBar->clearChildren();
    _tabs.clear();
    for (size_t i = 0; i < icons.size(); ++i) {
        auto     *t   = _catBar->add<CatTab>(icons[i]);
        const int sec = _tabSection[i];
        t->onClick    = [this, sec] { _scroll->scrollTo(_grid->sectionTop(sec) - kMargin); };
        _tabs.push_back(t);
    }
    _catBar->setVisible(!_searching);
    if (_searching)
        _grid->setSelected(0); // Enter picks the top hit; browsing starts unselected
    syncTabs();
}

int EmojiPicker::activeTab() const {
    for (size_t i = 0; i < _tabs.size(); ++i)
        if (_tabs[i]->checked())
            return int(i);
    return -1;
}

void EmojiPicker::syncTabs() {
    if (_searching)
        return;
    const int s = _grid->sectionAt(_scroll->scrollOffset());
    for (size_t i = 0; i < _tabs.size(); ++i)
        _tabs[i]->setChecked(_tabSection[i] == s);
}

} // namespace screens
