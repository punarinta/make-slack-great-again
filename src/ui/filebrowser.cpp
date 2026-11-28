#include "ui/filebrowser.h"

#include "base/i18n.h"
#include "base/utf8.h"
#include "base/str.h"
#include "gfx/icons_generated.h"
#include "ui/textedit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {

using Mode = plat::FileDialogDesc::Mode;
using i18n::tr;

constexpr float kCardW = 640, kCardH = 460, kRowH = 28, kIcon = 16;

std::string parentOf(const std::string &dir) {
    std::string_view p = file::dirName(dir);
    if (p.empty() || p == dir)
        return {};
    // "C:" is a drive, and its root is "C:/".
    if (p.size() == 2 && p[1] == ':')
        return std::string(p) + "/";
    return std::string(p);
}

std::string stripSlashes(std::string p) {
    while (p.size() > 1 && p.back() == '/' && !(p.size() == 3 && p[1] == ':'))
        p.pop_back();
    return p;
}

// The title and OK label per mode.
struct ModeText {
    const char *title, *ok;
};
constexpr ModeText kModeText[] = {
    {N_("Open file"), N_("Open")},
    {N_("Open files"), N_("Open")},
    {N_("Save file"), N_("Save")},
    {N_("Choose folder"), N_("Choose")}
};

// A folder glyph (none in the icon set): a tab and a body.
void drawFolder(gfx::Painter &p, RectF r, Color c) {
    p.fillRoundRect({r.x + 1, r.y + 2, r.w * 0.45f, 4}, 1.5f, c);
    p.fillRoundRect({r.x + 1, r.y + 4, r.w - 2, r.h - 6}, 2, c);
}

} // namespace

// ── The entry list: one view painting only its visible rows ──────────────────

class FileBrowser::List : public View {
public:
    explicit List(FileBrowser &b) : _b(b) {
        setRole(Role::List);
        setFocusable(true);
        setHoverRepaint(true);
    }
    void reset() {
        _names.clear();
        _sizes.clear();
        _empty.reset();
        _names.resize(_b._entries.size());
        _nameWs.assign(_b._entries.size(), -1.f);
        _sizes.resize(_b._entries.size());
        invalidateLayout();
        update();
    }
    ScrollView *scroller() const { return static_cast<ScrollView *>(parent()->parent()); }
    void        reveal(int i) {
        ScrollView *s = scroller();
        if (i < 0 || !s)
            return;
        const float top = float(i) * kRowH, off = s->scrollOffset(), h = s->height();
        if (top < off)
            s->scrollTo(top);
        else if (top + kRowH > off + h)
            s->scrollTo(top + kRowH - h);
    }
    int rowsPerPage() const {
        const ScrollView *s = scroller();
        return std::max(1, int((s ? s->height() : 300) / kRowH) - 1);
    }

    SizeF measureContent(float aw, float) override {
        // Empty: room for the "empty" line.
        return {aw, std::max(float(_b._entries.size()) * kRowH, 3 * kRowH)};
    }
    void styleChanged() override { reset(); }

    void paint(gfx::Painter &p) override {
        View::paint(p);
        const float       scale = windowScale();
        // Only the rows inside the viewport (the list is as tall as all rows).
        const ScrollView *s     = scroller();
        const float       off   = s ? s->scrollOffset() : 0;
        const float       viewH = s ? s->height() : height();
        const size_t      n     = _b._entries.size();
        size_t            first = size_t(std::max(0.f, std::floor(off / kRowH)));
        size_t            last  = std::min(n, size_t(std::ceil((off + viewH) / kRowH)) + 1);
        const int         hover =
            hovered() && window() ? rowAt(mapFromWindow(window()->pointerPos()).y) : -1;
        _hoverRow = hover; // what is painted (a scroll moves rows under a still pointer)
        for (size_t i = first; i < last; ++i) {
            const file::DirEntry &e = _b._entries[i];
            const float           y = float(i) * kRowH;
            const RectF           row{4, y + 1, width() - 8, kRowH - 2};
            if (_b.selected(i))
                p.fillRoundRect(row, metric(M::RadiusS), color(C::Selection));
            else if (int(i) == hover)
                p.fillRoundRect(row, metric(M::RadiusS), color(C::Hover));
            if (int(i) == _b._current && focused())
                p.strokeRoundRect(row, metric(M::RadiusS), 1, color(C::FocusRing));
            const RectF ir{12, snapPx(y + (kRowH - kIcon) / 2), kIcon, kIcon};
            if (e.isDir)
                drawFolder(p, ir, color(C::Link));
            else
                gfx::drawIcon(p, gfx::Icon::CodeFile, ir, color(C::TextMuted));
            float sizeW = 0;
            if (!e.isDir) {
                if (!_sizes[i]) {
                    text::AttributedText t;
                    t.append(
                        str::byteSize(e.size, str::ByteSize::Exact), font(Font::Small, C::TextMuted)
                    );
                    _sizes[i] = text::Layout::build(std::move(t), {}, scale);
                }
                sizeW = std::ceil(_sizes[i]->width());
                _sizes[i]->paint(
                    p,
                    snapPx(
                        {width() - 16 - sizeW, y + std::floor((kRowH - _sizes[i]->height()) / 2)}
                    )
                );
            }
            const float nameX = 12 + kIcon + 10, nameW = width() - nameX - 16 - sizeW - 12;
            if (!_names[i] || _nameWs[i] != nameW) { // files: narrower by their size
                _nameWs[i] = nameW;
                text::AttributedText t;
                t.append(e.name, font(Font::Body, e.hidden ? C::TextMuted : C::Text));
                text::LayoutOptions o;
                o.maxWidth = std::max(20.f, nameW);
                o.maxLines = 1;
                o.ellipsis = true;
                _names[i]  = text::Layout::build(std::move(t), o, scale);
            }
            _names[i]->paint(p, snapPx({nameX, y + std::floor((kRowH - _names[i]->height()) / 2)}));
        }
        if (n == 0) {
            if (!_empty) { // shaped once per listing, not per paint
                text::AttributedText t;
                t.append(
                    _b._desc.mode == Mode::PickFolder ? tr("No folders here")
                                                      : tr("This folder is empty"),
                    font(Font::Body, C::TextMuted)
                );
                _empty = text::Layout::build(std::move(t), {}, scale);
            }
            _empty->paint(p, snapPx({std::floor((width() - _empty->width()) / 2), 24}));
        }
    }

    int rowAt(float y) const {
        const int i = int(std::floor(y / kRowH));
        return y >= 0 && i < int(_b._entries.size()) ? i : -1;
    }

    bool onEvent(Event &e) override {
        switch (e.type) {
        case EventType::PointerDown: {
            if (e.button != plat::Button::Left)
                return true;
            const int  i      = rowAt(e.pos.y);
            const bool toggle = e.mods & (plat::primaryMod());
            const bool range  = e.mods & plat::ModShift;
            if (i < 0)
                return true;
            if (e.clicks >= 2 && !toggle && !range)
                _b.activate(i);
            else
                _b.select(i, toggle, range);
            return true;
        }
        case EventType::PointerMove: {
            // Only the rows whose hover fill changes.
            const int row = rowAt(e.pos.y);
            if (row != _hoverRow) {
                for (int r : {_hoverRow, row})
                    if (r >= 0)
                        update({0, float(r) * kRowH, width(), kRowH});
                _hoverRow = row;
            }
            return false;
        }
        case EventType::PointerLeave:
            _hoverRow = -1; // the hover repaint clears it
            return false;
        case EventType::PointerUp:
            return true;
        case EventType::KeyDown:
            // Before the ScrollView, which would take the arrows to scroll.
            return _b.keyDown(e);
        default:
            return false;
        }
    }

    // Names are laid out at a width; a resize re-lays them.
    void layout() override {
        if (width() != _laidW) {
            _laidW = width();
            for (auto &l : _names)
                l.reset();
        }
        View::layout();
    }

private:
    FileBrowser                               &_b;
    std::vector<std::unique_ptr<text::Layout>> _names, _sizes;
    std::unique_ptr<text::Layout>              _empty;  // "This folder is empty"
    std::vector<float>                         _nameWs; // the width each name was built for
    float                                      _laidW    = -1;
    int                                        _hoverRow = -1;
};

// ── FileBrowser ─────────────────────────────────────────────────────────────

bool FileBrowser::globMatch(std::string_view pat, std::string_view name) {
    // Iterative '*' backtracking; '?' is one byte (names are UTF-8, patterns
    // in practice ASCII extensions).
    size_t p = 0, n = 0, star = std::string_view::npos, mark = 0;
    while (n < name.size()) {
        if (p < pat.size() &&
            (pat[p] == '?' || str::asciiLower(pat[p]) == str::asciiLower(name[n]))) {
            ++p;
            ++n;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*')
        ++p;
    return p == pat.size();
}

FileBrowser::FileBrowser(const plat::FileDialogDesc &d, Done done)
    : _desc(d), _done(std::move(done)) {
    _desc.parent = nullptr;
    setCard(false); // the scrim is the popup; the card is a child
    setRole(Role::Group);
    style().stack().items(Align::Center);

    _card = add<View>();
    _card->style().size(kCardW, kCardH).padding(16).spacing(10);
    _card->style().shrink = 1;
    _card->setBackground(C::PopupBg, metric(M::RadiusL));
    _card->setBorder(C::PopupBorder);
    _card->setPaintOutset(16);

    const ModeText &mt = kModeText[int(_desc.mode)];
    _card->add<Label>(
        _desc.title.empty() ? std::string(tr(mt.title)) : _desc.title, Font::BodyBold
    );

    auto *bar = _card->add<View>();
    bar->style().row().items(Align::Center).spacing(4).height(28);
    _path = bar->add<View>();
    _path->style().row().items(Align::Center).spacing(0).flex(1);
    _path->setClipChildren(true);
    _eye          = bar->add<IconButton>(gfx::Icon::EyeOff, tr("Show hidden files (Ctrl+H)"));
    _eye->onClick = [this] { setShowHidden(!_hidden); };

    auto *scroll = _card->add<ScrollView>();
    scroll->style().flex(1);
    scroll->setBackground(C::InputBg);
    scroll->setBorder(C::InputBorder);
    scroll->content()->style().padding(0, 4, 0, 4);
    _list = scroll->content()->add<List>(*this);

    _status = _card->add<Label>("", Font::Small, C::TextMuted);
    _status->setMaxLines(2);
    _status->setVisible(false);

    if (_desc.mode == Mode::Save) {
        auto *row = _card->add<View>();
        row->style().row().items(Align::Center).spacing(10);
        row->add<Label>(tr("Name"), Font::Body, C::TextMuted);
        _name = row->add<TextEdit>();
        _name->setMaxLines(1);
        _name->setBackground(C::InputBg, 6);
        _name->setBorder(C::InputBorder);
        _name->style().padding(8, 6).flex(1);
        _name->setText(_desc.suggestedName);
        _name->onSubmit = [this] { return accept(), true; };
        _name->onChange = [this] {
            _confirmName.clear();
            refreshOk();
        };
        // The field keeps its editing keys; the list's Up/Down, Escape and
        // Ctrl+H still work from it.
        _name->onKey = [this](const Event &e) {
            using plat::Key;
            if (e.type != EventType::KeyDown)
                return false;
            if (e.key == Key::Escape || e.key == Key::Up || e.key == Key::Down ||
                (e.key == Key::H && (e.mods & plat::primaryMod())))
                return keyDown(e);
            return false;
        };
    }

    auto *bottom = _card->add<View>();
    bottom->style().row().items(Align::Center).spacing(8);
    if (!_desc.filters.empty() && _desc.mode != Mode::PickFolder) {
        _filterBtn = bottom->add<Button>(_desc.filters[0].name, Button::Kind::Secondary);
        _filterBtn->setIcon(gfx::Icon::ChevronDown);
        _filterBtn->setIconSize(14);
        _filterBtn->style().shrink = 1;
        _filterBtn->onClick        = [this] {
            std::vector<MenuItem> items;
            for (size_t i = 0; i < _desc.filters.size(); ++i) {
                MenuItem it;
                it.id      = int(i);
                it.label   = _desc.filters[i].name;
                it.checked = i == _filter;
                items.push_back(std::move(it));
            }
            if (Window *w = window())
                Menu::show(
                    *w,
                    _filterBtn->windowRect(),
                    std::move(items),
                    [this](int id) { setFilter(size_t(id)); },
                    Place::Above
                );
        };
    }
    bottom->add<View>()->style().flex(1);
    bottom->add<Button>(tr("Cancel"), Button::Kind::Secondary)->onClick = [this] { cancel(); };
    _ok          = bottom->add<Button>(tr(mt.ok), Button::Kind::Primary);
    _ok->onClick = [this] { accept(); };

    std::string start = stripSlashes(_desc.initialDir);
    if (start.empty() || !file::isDir(start)) {
        const plat::App *pa = ui::app() ? &ui::app()->platform() : nullptr;
        start               = pa ? pa->standardDir(plat::StandardDir::Home) : std::string();
    }
    if (start.empty() || !setDir(start))
        setDir("/");
}

FileBrowser::~FileBrowser() {
    if (!_answered && _done) // closed some other way (the window went away)
        _done({});
}

FileBrowser *FileBrowser::show(Window &w, const plat::FileDialogDesc &d, Done done) {
    auto       *raw = new FileBrowser(d, std::move(done));
    auto       *b   = static_cast<FileBrowser *>(w.showPopup(std::unique_ptr<Popup>(raw)));
    const SizeF ws  = w.size();
    b->setAnchor({0, 0, ws.w, ws.h}, Place::Over);
    if (b->_name) {
        b->_name->focus();
        // The stem selected, as native dialogs do: typing replaces the name
        // and keeps the extension.
        const std::string &t   = b->_name->text();
        const size_t       dot = t.rfind('.');
        b->_name->setSelection(0, uint32_t(dot == std::string::npos || dot == 0 ? t.size() : dot));
    } else {
        b->_list->focus();
    }
    return b;
}

SizeF FileBrowser::measureContent(float aw, float ah) {
    // The whole window (the scrim); placeIn keeps it inside the margin and
    // paint() covers that too.
    const SizeF c = _card->measure(aw, ah);
    return {std::max(aw, c.w), std::max(ah, c.h)};
}

void FileBrowser::paint(gfx::Painter &p) {
    // Dim everything behind, margin included (it is within the paint outset).
    const float o = float(paintOutset());
    p.fillRect({-o, -o, width() + 2 * o, height() + 2 * o}, color(C::Shadow));
    p.dropShadow(_card->frame(), metric(M::RadiusL), 12, color(C::Shadow));
    Popup::paint(p);
}

bool FileBrowser::setDir(std::string dir) {
    dir = stripSlashes(std::move(dir));
    std::vector<file::DirEntry> all;
    if (!file::listDir(dir, &all)) {
        setStatus(tr("This folder can\xE2\x80\x99t be opened."), true);
        return false;
    }
    _dir = std::move(dir);
    _all = std::move(all);
    setStatus({}, false);
    relist();
    rebuildPath();
    if (ScrollView *s = _list->scroller())
        s->scrollTo(0);
    return true;
}

void FileBrowser::relist() {
    const std::vector<std::string> *pats = _desc.filters.empty() || _desc.mode == Mode::PickFolder
                                               ? nullptr
                                               : &_desc.filters[_filter].patterns;
    _entries.clear();
    for (const auto &e : _all) {
        if (e.hidden && !_hidden)
            continue;
        if (!e.isDir && _desc.mode == Mode::PickFolder)
            continue;
        if (!e.isDir && pats && !pats->empty()) {
            bool ok = false;
            for (const auto &pt : *pats)
                ok = ok || globMatch(pt, e.name);
            if (!ok)
                continue;
        }
        _entries.push_back(e);
    }
    // Folders first, then by name (case-folded, then as is for a stable order).
    std::vector<std::string> keys;
    std::vector<size_t>      order(_entries.size());
    keys.reserve(_entries.size());
    for (size_t i = 0; i < _entries.size(); ++i) {
        keys.push_back(utf8::foldCase(_entries[i].name));
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (_entries[a].isDir != _entries[b].isDir)
            return _entries[a].isDir;
        if (keys[a] != keys[b])
            return keys[a] < keys[b];
        return _entries[a].name < _entries[b].name;
    });
    std::vector<file::DirEntry> sorted;
    sorted.reserve(order.size());
    for (size_t i : order)
        sorted.push_back(std::move(_entries[i]));
    _entries = std::move(sorted);
    _sel.assign(_entries.size(), 0);
    _current = _anchor = -1;
    _list->reset();
    refreshOk();
}

void FileBrowser::rebuildPath() {
    _path->clearChildren();
    // Segments from the root: "/", "home", "robin" (Windows: "C:", "Users").
    std::vector<std::string> dirs;
    for (std::string d = _dir; !d.empty(); d = parentOf(d))
        dirs.push_back(d);
    std::reverse(dirs.begin(), dirs.end());
    constexpr size_t kMaxSegments = 5;
    const size_t     from         = dirs.size() > kMaxSegments ? dirs.size() - kMaxSegments : 0;
    auto             crumb        = [&](const std::string &target, std::string label, bool last) {
        auto *b = _path->add<Button>(std::move(label), Button::Kind::Ghost);
        b->style().padding(6, 0).height(26);
        b->style().shrink = 1;
        if (last)
            b->setTextColor(C::Text);
        else
            b->setTextColor(C::TextMuted);
        b->onClick = [this, target] { setDir(target); };
    };
    if (from > 0)
        crumb(dirs[from - 1], "\xE2\x80\xA6", false);
    for (size_t i = from; i < dirs.size(); ++i) {
        std::string label(file::baseName(stripSlashes(dirs[i])));
        if (label.empty())
            label = dirs[i]; // the root
        if (i > from || from > 0)
            _path->add<IconView>(gfx::Icon::ChevronRight, 12, C::TextFaint)->style().size(12, 12);
        crumb(dirs[i], std::move(label), i + 1 == dirs.size());
    }
}

int FileBrowser::find(std::string_view name) const {
    for (size_t i = 0; i < _entries.size(); ++i)
        if (_entries[i].name == name)
            return int(i);
    return -1;
}

std::string FileBrowser::pathOf(size_t i) const {
    return file::join(_dir, _entries[i].name);
}

void FileBrowser::select(int i, bool toggle, bool range) {
    if (i < 0 || size_t(i) >= _entries.size())
        return;
    const bool multi = _desc.mode == Mode::OpenMultiple;
    if (multi && range && _anchor >= 0) {
        std::fill(_sel.begin(), _sel.end(), 0);
        for (int k = std::min(_anchor, i); k <= std::max(_anchor, i); ++k)
            _sel[size_t(k)] = 1;
    } else if (multi && toggle) {
        _sel[size_t(i)] ^= 1;
        _anchor = i;
    } else {
        std::fill(_sel.begin(), _sel.end(), 0);
        _sel[size_t(i)] = 1;
        _anchor         = i;
    }
    _current = i;
    // Save: picking a file takes its name.
    if (_name && !_entries[size_t(i)].isDir) {
        _name->setText(_entries[size_t(i)].name);
        _confirmName.clear();
    }
    _list->reveal(i);
    _list->update();
    refreshOk();
}

void FileBrowser::activate(int i) {
    if (i < 0 || size_t(i) >= _entries.size())
        return;
    if (_entries[size_t(i)].isDir) {
        setDir(pathOf(size_t(i)));
        return;
    }
    select(i);
    accept();
}

void FileBrowser::goUp() {
    const std::string up = parentOf(_dir);
    if (up.empty())
        return;
    const std::string was(file::baseName(_dir));
    if (setDir(up))
        if (const int i = find(was); i >= 0) {
            _current = _anchor = i;
            _sel[size_t(i)]    = 1;
            _list->reveal(i);
            refreshOk();
        }
}

void FileBrowser::setShowHidden(bool on) {
    _hidden = on;
    _eye->setIcon(on ? gfx::Icon::Eye : gfx::Icon::EyeOff);
    _eye->setChecked(on);
    relist();
}

void FileBrowser::setFilter(size_t i) {
    if (i >= _desc.filters.size())
        return;
    _filter = i;
    if (_filterBtn)
        _filterBtn->setLabel(_desc.filters[i].name);
    relist();
}

void FileBrowser::setStatus(std::string s, bool error) {
    _status->setText(std::move(s));
    _status->setColor(error ? C::Danger : C::TextMuted);
    _status->setVisible(!_status->text().empty());
}

void FileBrowser::refreshOk() {
    bool can = false;
    switch (_desc.mode) {
    case Mode::Open:
    case Mode::OpenMultiple:
        for (size_t i = 0; i < _entries.size() && !can; ++i)
            can = _sel[i] != 0;
        break;
    case Mode::Save:
        can = _name && !_name->text().empty();
        break;
    case Mode::PickFolder:
        can = true; // the selected folder, else the one shown
        break;
    }
    _ok->setEnabled(can);
    if (_desc.mode == Mode::Save)
        _ok->setLabel(_confirmName.empty() ? tr("Save") : tr("Replace"));
}

bool FileBrowser::accept() {
    std::vector<std::string> out;
    switch (_desc.mode) {
    case Mode::Open:
    case Mode::OpenMultiple: {
        int folder = -1, files = 0;
        for (size_t i = 0; i < _entries.size(); ++i) {
            if (!_sel[i])
                continue;
            if (_entries[i].isDir)
                folder = int(i);
            else
                ++files;
        }
        if (!files && folder >= 0) { // OK on a folder opens it
            setDir(pathOf(size_t(folder)));
            return false;
        }
        for (size_t i = 0; i < _entries.size(); ++i)
            if (_sel[i] && !_entries[i].isDir)
                out.push_back(pathOf(i));
        break;
    }
    case Mode::Save: {
        std::string name(_name ? _name->text() : std::string());
        while (!name.empty() && (name.back() == ' ' || name.back() == '/'))
            name.pop_back();
        if (name.empty())
            return false;
        std::string target = file::resolve(_dir, name);
        if (file::isDir(target)) { // a typed folder name: go there
            setDir(target);
            if (_name)
                _name->setText(_desc.suggestedName);
            return false;
        }
        // No extension typed: the chosen filter's, when it names exactly one.
        if (file::extension(target).empty() && !_desc.filters.empty()) {
            const auto &pats = _desc.filters[_filter].patterns;
            if (pats.size() == 1 && pats[0].size() > 2 && pats[0].rfind("*.", 0) == 0 &&
                pats[0].find_first_of("*?", 2) == std::string::npos)
                target += pats[0].substr(1);
        }
        if (!file::isDir(std::string(file::dirName(target)))) {
            setStatus(tr("That folder doesn\xE2\x80\x99t exist."), true);
            return false;
        }
        if (file::exists(target) && _confirmName != target) {
            _confirmName = target;
            setStatus(
                i18n::arg(
                    tr("\xE2\x80\x9C%1\xE2\x80\x9D already exists. Replace it?"),
                    file::baseName(target)
                ),
                true
            );
            refreshOk();
            return false;
        }
        out.push_back(std::move(target));
        break;
    }
    case Mode::PickFolder: {
        int folder = -1;
        for (size_t i = 0; i < _entries.size() && folder < 0; ++i)
            if (_sel[i] && _entries[i].isDir)
                folder = int(i);
        out.push_back(folder >= 0 ? pathOf(size_t(folder)) : _dir);
        break;
    }
    }
    if (out.empty())
        return false;
    finish(std::move(out));
    return true;
}

void FileBrowser::cancel() {
    finish({});
}

void FileBrowser::finish(std::vector<std::string> paths) {
    if (_answered)
        return;
    _answered = true;
    Done done = std::move(_done);
    close(); // deferred destruction
    if (done)
        done(std::move(paths));
}

bool FileBrowser::keyDown(const Event &e) {
    using plat::Key;
    const uint32_t mods  = e.mods & kModMask;
    const bool     prim  = mods == plat::primaryMod();
    const bool     shift = mods == plat::ModShift;
    const int      n     = int(_entries.size());
    auto           move  = [&](int to) {
        if (n == 0)
            return;
        to = std::clamp(to, 0, n - 1);
        select(to, false, shift && _desc.mode == Mode::OpenMultiple);
    };
    switch (e.key) {
    case Key::Escape:
        cancel();
        return true;
    case Key::H:
        if (prim) {
            setShowHidden(!_hidden);
            return true;
        }
        break;
    case Key::A:
        if (prim && _desc.mode == Mode::OpenMultiple) {
            for (int i = 0; i < n; ++i)
                _sel[size_t(i)] = !_entries[size_t(i)].isDir;
            _list->update();
            refreshOk();
            return true;
        }
        break;
    case Key::Up:
        if (mods == plat::ModAlt) {
            goUp();
            return true;
        }
        move(_current < 0 ? n - 1 : _current - 1);
        return true;
    case Key::Down:
        move(_current < 0 ? 0 : _current + 1);
        return true;
    case Key::Home:
        move(0);
        return true;
    case Key::End:
        move(n - 1);
        return true;
    case Key::PageUp:
        move(_current - _list->rowsPerPage());
        return true;
    case Key::PageDown:
        move(_current + _list->rowsPerPage());
        return true;
    case Key::Backspace:
        goUp();
        return true;
    case Key::Enter:
    case Key::KpEnter:
        if (_current >= 0 && _entries[size_t(_current)].isDir && _sel[size_t(_current)])
            activate(_current);
        else
            accept();
        return true;
    default:
        break;
    }
    // A letter or digit jumps to the next entry starting with it.
    if (const char c = typeAheadChar(e.key, true); c && !(mods & ~plat::ModShift)) {
        const int i = typeAheadMatch(_current, n, c, &_entries, [](const void *entries, int k) {
            return std::string_view(
                (*static_cast<const std::vector<file::DirEntry> *>(entries))[size_t(k)].name
            );
        });
        if (i >= 0)
            select(i);
        return true;
    }
    return false;
}

bool FileBrowser::onEvent(Event &e) {
    // Keys the name field did not take arrive here too; only the list's.
    if (e.type == EventType::KeyDown && !(_name && _name->focused()) && keyDown(e))
        return true;
    if (e.type == EventType::KeyDown && e.key == plat::Key::Escape) {
        cancel();
        return true;
    }
    // It stands in for the OS's modal file dialog, a window of its own: no
    // key reaches the app's window shortcuts behind it (Tab still moves
    // focus inside).
    if (e.type == EventType::PointerDown ||
        (e.type == EventType::KeyDown && e.key != plat::Key::Tab))
        return true; // the scrim swallows presses; nothing behind reacts
    return Popup::onEvent(e);
}

} // namespace ui
