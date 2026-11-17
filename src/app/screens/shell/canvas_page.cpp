#include "screens/shell/canvas_page.h"

#include "app/screens/common/canvas_doc.h"
#include "base/i18n.h"
#include "base/str.h"
#include "prim/hash.h"
#include "gfx/icons_generated.h"
#include "ui/controls.h"

#include <algorithm>
#include <cstdlib>

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
    _body->onSubmit = [] { return false; }; // Enter is a new line
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
    setBody({});
    _menuBtn->setVisible(!_fileId.empty());
    if (_fileId.empty()) {
        _body->focus(); // a blank page: the first save creates the canvas
        return;
    }
    loadContent();
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
    }
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
