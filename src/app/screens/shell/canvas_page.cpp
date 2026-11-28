#include "screens/shell/canvas_page.h"

#include "app/screens/common/canvas_doc.h"
#include "base/i18n.h"
#include "base/str.h"
#include "prim/hash.h"
#include "gfx/icons_generated.h"
#include "ui/controls.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <utility>

using namespace ui;
using gfx::Icon;
using i18n::arg;
using i18n::tr;
using model::ConvRef;
using model::kNoConv;
using CanvasState = model::Backend::CanvasState;
using Change      = model::Backend::CanvasChange;

namespace shell {

namespace {

constexpr float kColumnMaxW  = 1040; // the editor column, Slack's measure
constexpr int   kSaveDelayMs = 2500; // autosave this long after typing stops

using EditAt = std::function<void(uint32_t at)>;

// A part of the reading view that edits the body at `at` when clicked
// (a list marker, a table's frame, the space between rows).
class ReaderBlock final : public View {
public:
    ReaderBlock(EditAt edit, uint32_t at) : _edit(std::move(edit)), _at(at) {
        setCursor(plat::Cursor::IBeam);
    }
    bool onEvent(Event &e) override {
        switch (e.type) {
        case EventType::PointerDown:
            _pressed = e.button == plat::Button::Left;
            return _pressed;
        case EventType::PointerUp:
            if (!std::exchange(_pressed, false))
                return false;
            if (bounds().contains(e.pos))
                _edit(_at);
            return true;
        case EventType::PointerCancel:
            _pressed = false;
            return false;
        default:
            return false;
        }
    }

private:
    EditAt   _edit;
    uint32_t _at;
    bool     _pressed = false;
};

// Text of the body from `from` on (as many bytes as it shows): a click on a
// link opens it, anywhere else edits the body at the offset under the pointer.
class ReaderText final : public Label {
public:
    ReaderText(EditAt edit, uint32_t from) : _edit(std::move(edit)), _from(from) {
        setCursor(plat::Cursor::IBeam);
    }
    bool onEvent(Event &e) override {
        if (Label::onEvent(e)) // a link
            return true;
        switch (e.type) {
        case EventType::PointerDown:
            _pressed = e.button == plat::Button::Left;
            return _pressed;
        case EventType::PointerUp:
            if (!std::exchange(_pressed, false))
                return false;
            if (bounds().contains(e.pos))
                _edit(_from + std::min<uint32_t>(offsetAt(e.pos), uint32_t(text().size())));
            return true;
        case EventType::PointerCancel:
            _pressed = false;
            return false;
        default:
            return false;
        }
    }

private:
    EditAt   _edit;
    uint32_t _from;
    bool     _pressed = false;
};

bool startsWith(std::string_view s, std::string_view p) {
    return s.substr(0, p.size()) == p;
}

// A markdown table's rule row ("| --- | :-: |").
bool tableRule(std::string_view line) {
    bool dash = false;
    for (const char c : line) {
        if (c == '-')
            dash = true;
        else if (c != '|' && c != ':' && c != ' ')
            return false;
    }
    return dash;
}

// The cells of a table row: [from, to) per cell, spaces trimmed, between the
// row's pipes.
std::vector<std::pair<uint32_t, uint32_t>>
tableCells(std::string_view text, uint32_t from, uint32_t to) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    uint32_t                                   start = from + 1; // past the leading '|'
    for (uint32_t i = start; i < to; ++i) {
        if (text[i] != '|')
            continue;
        uint32_t a = start, b = i;
        while (a < b && text[a] == ' ')
            ++a;
        while (b > a && text[b - 1] == ' ')
            --b;
        out.push_back({a, b});
        start = i + 1;
    }
    return out;
}

} // namespace

namespace canvas {

std::string markdown(const TextEdit &body) {
    return screens::canvas::markdown(body.text(), body.runs());
}

} // namespace canvas

// ── CanvasPage ──────────────────────────────────────────────────────────────

CanvasPage::CanvasPage(screens::Context &ctx) : _ctx(ctx), _alive(std::make_shared<bool>(true)) {
    style().stack();
    setBackground(C::Surface);
    _scroll   = add<ScrollView>();
    View *row = _scroll->content();
    row->style().row().justifyContent(Justify::Center);
    View *col = row->add<View>();
    col->style().column().flex(1).padding(24, 56, 24, 32).spacing(16);
    col->style().maxW = kColumnMaxW;

    _notice = col->add<Label>("", Font::Caption, C::FormWarning);
    _notice->setVisible(false);
    _title = col->add<TextEdit>();
    _title->setFont(Font::CanvasTitle);
    _title->setPlaceholder(tr("Your canvas title"));
    _title->setMaxLines(0);
    _title->setPlainPaste(true);
    _reader = col->add<ReaderBlock>([this](uint32_t at) { edit(at); }, 0);
    // The editor's own padding, so the text stays put between the two.
    _reader->style().column().spacing(6).flex(1).padding(8, 6);
    _reader->style().minH = 12 * 21; // the editor's 12 lines: room to click into
    _reader->setVisible(false);
    _body = col->add<TextEdit>();
    _body->setPlaceholder(tr("Go ahead, start writing!"));
    _body->setMinLines(12);
    _body->setMaxLines(0);
    _body->style().flex(1);

    // Enter in the title goes on to the body (a title is one line).
    _title->onSubmit = [this] {
        _body->focus();
        return true;
    };
    _title->onChange = [this] {
        if (_loading)
            return;
        _titleDirty = true;
        armSave();
        if (onTitleChanged && !_fileId.empty())
            onTitleChanged(std::string(str::trim(_title->text())));
    };
    _body->onSubmit      = [] { return false; }; // Enter is a new line
    // Leaving the editor shows the reading view again.
    _body->onFocusChange = [this](bool focused) {
        if (!focused && _editing) {
            _editing = false;
            refreshMode();
        }
    };
    _body->onKey = [this](const Event &e) {
        if (e.key != plat::Key::Escape)
            return false;
        stopEditing();
        return true;
    };
    _body->onChange = [this] {
        if (_loading)
            return;
        _bodyDirty = true;
        armSave();
    };
    _body->onSelectionChange = [this] {
        // The page scrolls, not the editor: keep the caret in sight.
        const RectF c = _body->caretRect();
        const RectF b = _body->windowRect(), v = _scroll->windowRect();
        const float top = b.y + c.y - v.y - 8, bottom = b.y + c.y + c.h - v.y + 8;
        if (top < 0)
            _scroll->scrollBy(top);
        else if (bottom > v.h)
            _scroll->scrollBy(bottom - v.h);
    };

    // The floating "⋮", top-right over the page.
    View *strip = add<View>();
    strip->style().row().height(16 + 34).padding(16, 16, 16, 0).justifyContent(Justify::End);
    strip->setHitTransparent(true);
    auto *menu = strip->add<IconButton>(Icon::EllipsisVertical, std::string());
    menu->style().size(34, 34);
    menu->setIconSize(17);
    menu->setBackground(C::Surface, 8);
    menu->setBorder(C::BorderStrong);
    menu->onClick = [this] { showMenu(); };
    _menuBtn      = menu;
    _menuBtn->setVisible(false); // meaningless until the canvas exists
}

CanvasPage::~CanvasPage() {
    *_alive = false;
    dropTimer();
}

void CanvasPage::dropTimer() {
    if (_saveTimer)
        _ctx.app.cancelTimer(_saveTimer);
    _saveTimer = 0;
}

void CanvasPage::armSave() {
    dropTimer();
    _saveTimer = _ctx.app.addTimer(kSaveDelayMs, false, [this] {
        _saveTimer = 0;
        flushPendingSave();
    });
}

void CanvasPage::open(ConvRef conv) {
    if (conv >= _ctx.store().conversationCount())
        return clear();
    const model::Conversation &c = _ctx.store().conversation(conv);
    openFile(conv, c.canvasId, c.canvasTitle == "Canvas" ? std::string() : c.canvasTitle);
}

void CanvasPage::openFile(ConvRef conv, const std::string &fileId, const std::string &title) {
    if (conv >= _ctx.store().conversationCount())
        return clear();
    // Same conversation, same canvas: keep the editor, refetching only
    // when nothing is unsaved (local edits win; they autosave shortly).
    if (_conv == conv && _fileId == fileId) {
        if (!_fileId.empty() && !_bodyDirty && !_titleDirty && !_saving)
            loadContent();
        return;
    }
    flushPendingSave(); // the previous conversation's edits
    ++_seq;
    _conv        = conv;
    _fileId      = fileId;
    _permalink   = {};
    _lastHtml    = {};
    _serverTitle = _fileId.empty() ? std::string() : title;
    _refetching  = false;
    setReadOnly(ReadOnly::None);
    _loading = true;
    _title->setText(_serverTitle);
    _loading    = false;
    _titleDirty = false;
    // A blank page is written; a canvas is read until clicked into.
    _editing    = _fileId.empty();
    setBody({});
    _menuBtn->setVisible(!_fileId.empty());
    if (_fileId.empty()) {
        _body->focus(); // a blank page: the first save creates the canvas
        return;
    }
    loadContent();
}

void CanvasPage::edit(uint32_t at) {
    if (_ro != ReadOnly::None)
        return;
    _editing = true;
    refreshMode();
    const uint32_t end = uint32_t(_body->text().size());
    _body->focus();
    _body->setSelection(std::min(at, end), std::min(at, end));
}

void CanvasPage::stopEditing() {
    if (!_editing)
        return;
    _editing = false;
    if (Window *w = window(); w && w->focusView() == _body)
        w->setFocus(nullptr); // onFocusChange: the reading view
    refreshMode();
}

void CanvasPage::refreshMode() {
    // Nothing to read yet (a new canvas): the editor and its placeholder.
    const bool reading = !_editing && (!_body->empty() || _ro != ReadOnly::None);
    if (reading)
        rebuildReader();
    const bool switched = _reader->visible() != reading;
    _reader->setVisible(reading);
    _body->setVisible(!reading);
    if (!switched)
        return;
    // The page stays where it was: once the other view is laid out (its
    // height differs), scroll back to the same offset.
    const float y = _scroll->scrollOffset();
    if (y > 0)
        _ctx.app.addTimer(0, false, [this, alive = std::weak_ptr<bool>(_alive), y] {
            if (!alive.expired())
                _scroll->scrollTo(y);
        });
}

void CanvasPage::rebuildReader() {
    _reader->clearChildren();
    _readerLinks.clear();
    const std::string &t    = _body->text();
    const EditAt       edit = [this](uint32_t at) { this->edit(at); };

    // The body's formats per byte, as TextEdit keeps them: format bits, and
    // a link as (index into _readerLinks + 1) << 8.
    std::vector<uint16_t> fmt(t.size(), 0);
    for (const TextEdit::Run &r : _body->runs()) {
        uint16_t v = r.format;
        if (!r.link.empty()) {
            auto it = std::find(_readerLinks.begin(), _readerLinks.end(), r.link);
            if (it == _readerLinks.end())
                it = _readerLinks.insert(_readerLinks.end(), std::string(r.link));
            v = uint16_t(v | (uint16_t(it - _readerLinks.begin() + 1) << 8));
        }
        for (uint32_t i = r.start; i < r.end && i < t.size(); ++i)
            fmt[i] = v;
    }
    text::Style body = font(Font::Body);
    body.color       = themed(C::Text);
    // [a, b) of the body in `base`, its own formats on top.
    auto rich        = [&](uint32_t a, uint32_t b, const text::Style &base) {
        text::AttributedText out;
        for (uint32_t i = a; i < b;) {
            uint32_t j = i + 1;
            while (j < b && fmt[j] == fmt[i])
                ++j;
            const uint16_t f  = fmt[i];
            text::Style    st = base;
            if (f & TextEdit::Bold)
                st.weight = text::Weight::Bold;
            if (f & TextEdit::Italic)
                st.italic = true;
            if (f & TextEdit::Strike)
                st.strike = true;
            if (f & TextEdit::Code) {
                st.mono       = true;
                st.size       = std::max(10.f, base.size - 2);
                st.color      = themed(C::CodeText);
                st.background = themed(C::CodeBg);
            }
            if (f >> 8) {
                st.color     = themed(C::Link);
                st.underline = true;
                st.linkId    = f >> 8;
            }
            out.append(std::string_view(t).substr(i, j - i), st);
            i = j;
        }
        return out;
    };
    auto label = [&](View *in, uint32_t a, uint32_t b, const text::Style &base) {
        auto *l = in->add<ReaderText>(edit, a);
        l->setRichText(rich(a, b, base));
        l->onLink = [this](uint32_t id) {
            if (id && id <= _readerLinks.size()) {
                const std::string &url = _readerLinks[id - 1];
                if (_ctx.openUrl)
                    _ctx.openUrl(url);
                else
                    _ctx.app.platform().openUrl(url);
            }
        };
        return l;
    };

    // Quoted lines in a row: one bar down their side.
    View *quote = nullptr;
    auto  host  = [&](bool quoted) -> View  *{
        if (!quoted) {
            quote = nullptr;
            return _reader;
        }
        if (!quote) {
            View *row = _reader->add<View>();
            row->style().row().spacing(12);
            View *bar = row->add<View>();
            bar->style().width(3).noShrink();
            bar->setBackground(C::BorderStrong, 1.5f);
            quote = row->add<View>();
            quote->style().column().flex(1).spacing(6);
        }
        return quote;
    };

    // Table rows and code lines gather until their block ends.
    struct Row {
        uint32_t from, to;
    };
    std::vector<Row> table, code;
    bool             tableQuoted = false, inCode = false;
    auto             flushTable = [&] {
        if (table.empty())
            return;
        View *in   = host(tableQuoted);
        auto *grid = in->add<ReaderBlock>(edit, table.front().from);
        grid->style().column().margins(0, 4, 0, 4);
        grid->setBackground(C::Surface, 6);
        grid->setBorder(C::TableBorder);
        grid->setClipChildren(true);
        const bool header =
            table.size() > 1 &&
            tableRule(std::string_view(t).substr(table[1].from, table[1].to - table[1].from));
        size_t cols = 0;
        for (const Row &r : table)
            cols = std::max(cols, tableCells(t, r.from, r.to).size());
        bool first = true;
        for (size_t i = 0; i < table.size(); ++i) {
            if (header && i == 1)
                continue; // the rule row
            if (!first) {
                View *rule = grid->add<View>();
                rule->style().height(1);
                rule->setBackground(C::TableBorder);
            }
            View *row = grid->add<View>();
            row->style().row();
            if (header && i == 0)
                row->setBackground(C::TableHeaderBg);
            const auto cells = tableCells(t, table[i].from, table[i].to);
            for (size_t c = 0; c < cols; ++c) {
                View *cell = row->add<View>();
                cell->style().column().padding(12, 7).flex(1);
                if (c < cells.size()) {
                    text::Style st = body;
                    if (header && i == 0)
                        st.weight = text::Weight::Semibold;
                    label(cell, cells[c].first, cells[c].second, st);
                }
            }
            first = false;
        }
        table.clear();
    };
    auto flushCode = [&](bool quoted) {
        if (code.empty())
            return;
        auto *box = host(quoted)->add<ReaderBlock>(edit, code.front().from);
        box->style().column().padding(12, 10);
        box->setBackground(C::CodeBg, 6);
        text::Style mono = body;
        mono.mono        = true;
        mono.size        = std::max(10.f, body.size - 2);
        mono.color       = themed(C::CodeText);
        for (const Row &r : code)
            label(box, r.from, r.to, mono);
        code.clear();
    };

    for (uint32_t i = 0; i <= t.size();) {
        uint32_t e = uint32_t(t.find('\n', i));
        if (e == uint32_t(std::string::npos) || e > t.size())
            e = uint32_t(t.size());
        uint32_t p = i;
        while (startsWith(std::string_view(t).substr(p, e - p), "> "))
            p += 2;
        const bool             quoted = p > i;
        const std::string_view line   = std::string_view(t).substr(p, e - p);
        const uint32_t         next   = e + 1;

        if (startsWith(line, "```")) { // a fence opens or closes a code block
            flushTable();
            if (inCode)
                flushCode(quoted);
            inCode = !inCode;
        } else if (inCode) {
            code.push_back({p, e});
        } else if (startsWith(line, "|")) {
            if (table.empty())
                tableQuoted = quoted;
            table.push_back({p, e});
        } else {
            flushTable();
            View *in = host(quoted);
            if (line.empty()) {
                if (in->childCount())
                    in->add<View>()->style().height(4);
            } else if (
                size_t h = line.find_first_not_of('#');
                h >= 1 && h <= 6 && h < line.size() && line[h] == ' '
            ) {
                static constexpr float kScale[] = {1.6f, 1.35f, 1.15f, 1.05f, 1.f, 1.f};
                text::Style            st       = body;
                st.size *= kScale[h - 1];
                st.weight = text::Weight::Bold;
                label(in, p + uint32_t(h) + 1, e, st)->style().margins(0, 10, 0, 2);
            } else {
                // A list item: its indent (two spaces a level), then its marker.
                size_t s = line.find_first_not_of(' ');
                if (s == std::string_view::npos)
                    s = line.size();
                const std::string_view item = line.substr(s);
                std::string            marker;
                size_t                 skip    = 0;
                bool                   checked = false;
                if (startsWith(item, "- [ ] ") || startsWith(item, "- [x] ")) {
                    checked = item[3] == 'x';
                    marker  = checked ? "\xE2\x98\x91" : "\xE2\x98\x90"; // ☑ ☐
                    skip    = 6;
                } else if (startsWith(item, "- ") || startsWith(item, "* ")) {
                    marker = "\xE2\x80\xA2"; // •
                    skip   = 2;
                } else if (
                    const size_t d = item.find_first_not_of("0123456789");
                    d > 0 && d != std::string_view::npos && startsWith(item.substr(d), ". ")
                ) {
                    marker = std::string(item.substr(0, d + 1));
                    skip   = d + 2;
                }
                if (marker.empty()) {
                    label(in, p, e, body);
                } else {
                    const uint32_t from = p + uint32_t(s + skip);
                    auto          *row  = in->add<ReaderBlock>(edit, from);
                    row->style().row().padding(float(s / 2) * 22, 0, 0, 0);
                    auto *m = row->add<Label>(marker, Font::Body, C::TextMuted);
                    m->style().width(24).noShrink();
                    text::Style st = body;
                    if (checked) {
                        st.strike = true;
                        st.color  = themed(C::TextMuted);
                    }
                    label(row, from, e, st)->style().flex(1);
                }
            }
        }
        if (e >= t.size())
            break;
        i = next;
    }
    flushTable();
    flushCode(false);
}

void CanvasPage::loadContent() {
    if (_fileId.empty())
        return;
    const uint64_t seq = _seq;
    _refetching        = true;
    // The meta first: the file's title names the title h1 in the HTML.
    _ctx.backend().loadCanvasMeta(
        _fileId,
        [this,
         alive = std::weak_ptr<bool>(_alive),
         seq](std::string title, std::string link, CanvasState state) {
            if (alive.expired() || seq != _seq)
                return;
            if (state == CanvasState::Gone) {
                // Deleted elsewhere: the tab reverts to "Add canvas" (the
                // backend dropped it from the Store).
                _refetching = false;
                _fileId.clear();
                _lastHtml.clear();
                _serverTitle.clear();
                _loading = true;
                _title->clear();
                _loading = false;
                setBody({});
                _menuBtn->setVisible(false);
                return;
            }
            if (state == CanvasState::NoAccess) {
                // Never an empty, editable page whose autosave would replace
                // a canvas we can't see.
                _refetching = false;
                _lastHtml.clear();
                _serverTitle.clear();
                _loading = true;
                _title->clear();
                _loading = false;
                setBody({});
                _menuBtn->setVisible(false);
                setReadOnly(ReadOnly::NoAccess);
                return;
            }
            if (_ro == ReadOnly::NoAccess)
                setReadOnly(ReadOnly::None);
            _permalink = std::move(link);
            if (!title.empty())
                _serverTitle = std::move(title);
            _ctx.backend().loadCanvasContent(
                _fileId, [this, alive, seq](std::string html, std::string error) {
                    if (alive.expired() || seq != _seq)
                        return;
                    if (!error.empty()) {
                        _refetching = false; // the base is unknown
                        _lastHtml.clear();
                        return;
                    }
                    applyRemoteHtml(html);
                }
            );
        }
    );
}

void CanvasPage::applyRemoteHtml(const std::string &html) {
    _refetching = false;
    if (html == _lastHtml)
        return; // unchanged: don't reset the view
    _lastHtml = html;
    // Local edits win, while there is something local to protect.
    if ((_bodyDirty || _titleDirty || _saving) && !_body->empty())
        return;
    std::string       title;
    const std::string body = screens::canvas::editorHtml(
        html, {_serverTitle, std::string(str::trim(_title->text()))}, &title
    );
    if (!title.empty() && title != _title->text()) {
        _loading = true;
        _title->setText(title);
        _loading = false;
        if (onTitleChanged)
            onTitleChanged(title);
    }
    setBody(body);
}

void CanvasPage::setBody(const std::string &editorHtml) {
    // The same text again (a save coming back): leave the caret alone.
    std::string              text;
    std::vector<uint16_t>    fmt;
    std::vector<std::string> links;
    rich::fromHtml(editorHtml, &text, &fmt, &links);
    if (!editorHtml.empty() && text == _body->text()) {
        _bodyDirty = false;
        refreshMode();
        return;
    }
    const uint32_t caret = _body->caret();
    _loading             = true;
    _body->clear();
    if (!editorHtml.empty()) {
        _body->insertHtml(editorHtml);
        const uint32_t at = std::min<uint32_t>(caret, uint32_t(_body->text().size()));
        _body->setSelection(at, at);
    }
    _loading   = false;
    _bodyDirty = false;
    refreshMode();
}

void CanvasPage::flushPendingSave() {
    if (_conv == kNoConv || _ro != ReadOnly::None || (!_bodyDirty && !_titleDirty))
        return;
    if (_saving || _refetching) { // after the save / base refresh in flight
        armSave();
        return;
    }
    dropTimer();
    const std::string md = canvas::markdown(*_body);
    const std::string title(str::trim(_title->text()));
    if (_fileId.empty() && md.empty() && title.empty()) { // nothing to create a canvas from
        _bodyDirty = _titleDirty = false;
        return;
    }
    const uint64_t            seq   = _seq;
    const std::weak_ptr<bool> alive = _alive;
    _saving                         = true;

    if (_fileId.empty()) {
        // The body only; the title follows as a rename (it is a separate
        // field Slack renders as a leading h1 — "# title" would repeat it).
        _bodyDirty = _titleDirty = false;
        _ctx.backend().createChannelCanvas(
            _conv, md, [this, alive, seq, title](std::string fileId, std::string error) {
                if (alive.expired())
                    return;
                if (seq != _seq) {
                    _saving = false;
                    return;
                }
                if (fileId.empty()) {
                    _saving    = false;
                    _bodyDirty = true;
                    // Every failure shows the banner.
                    if (onError)
                        onError(arg(tr("Could not create canvas: %1"), error));
                    // It had a canvas we didn't know about (a free team's
                    // canvas tab): adopt it and save as an edit instead.
                    if (error.find("already_exists") != std::string::npos) {
                        _ctx.backend().loadChannelCanvas(_conv, [this, alive, seq](std::string id) {
                            if (alive.expired() || seq != _seq || id.empty())
                                return;
                            _fileId = std::move(id);
                            _menuBtn->setVisible(true);
                            flushPendingSave();
                        });
                        return;
                    }
                    armSave();
                    return;
                }
                _fileId = std::move(fileId);
                _menuBtn->setVisible(true);
                auto finish = [this, alive, seq](bool, const std::string &) {
                    if (alive.expired())
                        return;
                    _saving = false;
                    if (seq == _seq)
                        loadContent();
                };
                if (title.empty())
                    finish(true, {});
                else
                    _ctx.backend().editCanvas(_fileId, {{Change::Op::Rename, title}}, finish);
            }
        );
        return;
    }

    std::vector<Change> changes;
    if (_titleDirty && !title.empty())
        changes.push_back({Change::Op::Rename, title});
    // A section diff against the last served HTML: only the sections
    // that changed are written, so concurrent edits elsewhere survive. When
    // it can't be expressed safely, the whole document is replaced.
    if (_bodyDirty) {
        // The served HTML is cut once per answer (and titles), not per save.
        std::vector<std::string> titles{
            _serverTitle, title, std::string(str::trim(_title->text()))
        };
        uint64_t key = prim::fnv1a(_lastHtml) ^ _lastHtml.size();
        for (const std::string &t : titles)
            key = prim::fnv1a(t, prim::fnv1a("\x1f", key));
        if (key != _baseKey || _lastHtml.empty()) {
            _baseKey = key;
            _base.clear();
            _baseOk = !_lastHtml.empty() && screens::canvas::baseChunks(_lastHtml, titles, &_base);
        }
        std::vector<Change> ops;
        const bool          sections =
            !_lastHtml.empty() && _baseOk &&
            screens::canvas::diff(_base, screens::canvas::documentChunks(md), &ops);
        if (sections)
            changes.insert(changes.end(), ops.begin(), ops.end());
        else // Slack rejects an empty document; a lone space clears the page.
            changes.push_back({Change::Op::ReplaceAll, md.empty() ? std::string(" ") : md});
    }
    _bodyDirty = _titleDirty = false;
    if (changes.empty()) {
        _saving = false;
        return;
    }
    _ctx.backend().editCanvas(
        _fileId, std::move(changes), [this, alive, seq](bool ok, const std::string &error) {
            if (alive.expired())
                return;
            _saving = false;
            if (seq != _seq)
                return;
            if (ok) {
                loadContent();
                return;
            }
            // Every failure shows the banner.
            if (onError)
                onError(arg(tr("Canvas edit failed: %1"), error));
            if (error.find("canvas_not_found") != std::string::npos) {
                // Not addressable as a canvas (a free team's tab made in
                // Slack's own editor): retrying can never succeed.
                setReadOnly(ReadOnly::NotAddressable);
                return;
            }
            // The edits stay and the next save retries — as a whole document:
            // a failed sequence can leave the canvas partly updated.
            _bodyDirty = true;
            _lastHtml.clear();
            armSave();
        }
    );
}

void CanvasPage::setReadOnly(ReadOnly cause) {
    _ro                 = cause;
    const bool readOnly = cause != ReadOnly::None;
    _notice->setText(
        cause == ReadOnly::NotAddressable
            ? std::string(
                  tr("This canvas was created with Slack's built-in editor and is not "
                     "editable through the Slack API — it is read-only here.")
              )
        : cause == ReadOnly::NoAccess ? std::string(tr("You don't have access to this canvas."))
                                      : std::string()
    );
    _notice->setVisible(readOnly);
    _title->setEnabled(!readOnly);
    _body->setEnabled(!readOnly);
    if (readOnly) {
        dropTimer();
        _bodyDirty = _titleDirty = false;
        _editing                 = false;
    }
    refreshMode();
}

void CanvasPage::showMenu() {
    Window *w = window();
    if (_fileId.empty() || !w)
        return;
    std::vector<MenuItem> items(3);
    items[0].id     = 1;
    items[0].label  = tr("Copy link");
    items[0].icon   = uint16_t(Icon::Link);
    items[1]        = MenuItem::separatorItem();
    items[2].id     = 2;
    items[2].label  = tr("Delete canvas");
    items[2].danger = true;
    const RectF r   = _menuBtn->windowRect();
    Menu::popupAt(*w, {r.x + r.w, r.y + r.h + 4}, std::move(items), [this](int id) {
        if (id == 1 && !_permalink.empty())
            _ctx.app.platform().setClipboardText(_permalink);
        else if (id == 2)
            confirmDelete();
    });
}

void CanvasPage::confirmDelete() {
    Window *w = window();
    if (!w)
        return;
    auto d = Dialog::confirm(
        tr("Delete canvas"),
        tr("The canvas will be deleted for everyone in the conversation.\nThis action cannot be "
           "undone."),
        tr("Delete canvas"),
        Button::Kind::Danger,
        themed(C::TextMuted)
    );
    d->onAccepted = [this, alive = std::weak_ptr<bool>(_alive)] {
        if (alive.expired() || _fileId.empty())
            return;
        // Pending edits would re-create content on a dead canvas.
        dropTimer();
        _bodyDirty = _titleDirty = false;
        _ctx.backend().deleteCanvas(_fileId, [this, alive](bool ok, const std::string &error) {
            if (alive.expired())
                return;
            if (!ok) {
                if (onError)
                    onError(arg(tr("Canvas deletion failed: %1"), error));
                return;
            }
            const ConvRef conv = _conv;
            clear();
            _conv = conv; // still this conversation's (blank) page
            if (onDeleted)
                onDeleted();
        });
    };
    w->showPopup(std::move(d));
}

void CanvasPage::clear() {
    ++_seq;
    dropTimer();
    _conv = kNoConv;
    _fileId.clear();
    _permalink.clear();
    _lastHtml.clear();
    _serverTitle.clear();
    _refetching = _saving = false;
    setReadOnly(ReadOnly::None);
    _loading = true;
    _title->clear();
    _loading    = false;
    _titleDirty = false;
    setBody({});
    _menuBtn->setVisible(false);
}

} // namespace shell
