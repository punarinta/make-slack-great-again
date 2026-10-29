#include "screens/shell/shell_dialogs.h"

#include "app/identity.h"
#include "app/mrkdwn/markdown.h"
#include "app/mrkdwn/mrkdwn.h"
#include "app/model/jobs.h"
#include "app/screens/common/downloads.h"
#include "app/screens/common/file_dialogs.h"
#include "app/screens/common/message_text.h"
#include "app/screens/common/remote_images.h"
#include "app/screens/common/avatar_initial.h"
#include "base/file.h"
#include "base/i18n.h"
#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "screens/shell/composer.h"
#include "screens/shell/shell_text.h"
#include "ui/controls.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/emoji_picker.h"
#include "app/screens/messages/message_dialogs.h"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

using namespace ui;
using model::ConvKind;
using model::ConvRef;
using model::kNoConv;
using model::kNoUser;
using model::UserRef;
using V = Button::Kind;
using i18n::tr;

namespace shell {

namespace {

// ── The conversation picker ─────────────────────────────────────────────────

struct Target {
    ConvRef     conv   = kNoConv;
    UserRef     person = kNoUser; // someone without a DM yet
    std::string label;
    std::string role; // an agent workspace's teammate: a new session with it
};

class ConvSelector;

// The drop-down under the field: literal white, a divider.strong border,
// radius 4; rows padded 6/12, hover surface.highlight, the current row
// accent.subtleBg with replyLink text; at most 6 rows (200 px) tall.
class PickerList final : public Popup {
public:
    PickerList(ConvSelector &owner, std::vector<Target> items)
        : _owner(owner), _items(std::move(items)) {
        setModal(false);
        setCard(false);
        setPaintOutset(0);
    }
    SizeF measureContent(float, float) override {
        const float n = float(std::min<size_t>(_items.size(), 6));
        return {_w, std::min(200.f, rowH() * n + 4)};
    }
    void  setWidth(float w) { _w = w; }
    float rowH() const {
        return std::ceil(ui::pxFont(15, text::Weight::Regular, 0).size * 1.4f) + 12;
    }
    void paint(gfx::Painter &p) override {
        const RectF b = bounds();
        p.fillRoundRect(b, 4, 0xffffffffU);
        p.save();
        p.clipRoundRect(b, 4);
        const float k = windowScale();
        for (size_t i = 0; i < _items.size(); ++i) {
            const float y = 2 + rowH() * float(i) - _scroll;
            if (y + rowH() < 0 || y > b.h)
                continue;
            const bool cur = int(i) == _current;
            if (cur)
                p.fillRect({1, y, b.w - 2, rowH()}, ui::color(C::AccentSubtle));
            else if (int(i) == _hover)
                p.fillRect({1, y, b.w - 2, rowH()}, ui::color(C::FormHighlight));
            // Shaped once per row (again for another width, scale or colour),
            // not on every paint (each hover move repaints).
            const Color fg = cur ? ui::color(C::ReplyLink) : 0xff1d1c1dU;
            if (_lines.size() != _items.size() || _linesW != b.w || _linesScale != k) {
                _lines.clear();
                _lines.resize(_items.size());
                _lineFg.assign(_items.size(), 0);
                _linesW     = b.w;
                _linesScale = k;
            }
            if (!_lines[i] || _lineFg[i] != fg) {
                _lines[i] = oneLineLayout(
                    _items[i].label, ui::pxFont(15, text::Weight::Regular, fg), b.w - 24, k
                );
                _lineFg[i] = fg;
            }
            const text::Layout *l = _lines[i].get();
            l->paint(p, snapPx({12, y + std::floor((rowH() - l->height()) / 2)}));
        }
        p.restore();
        p.strokeRoundRect(b, 4, 1, ui::color(C::FormDividerStrong));
    }
    bool    onEvent(Event &e) override;
    uint8_t cursorAt(PointF p) const override {
        return rowAt(p.y) >= 0 ? uint8_t(plat::Cursor::Hand) : View::cursorAt(p);
    }
    const std::vector<Target> &items() const { return _items; }
    int                        current() const { return _current; }

private:
    int rowAt(float y) const {
        const int i = int((y - 2 + _scroll) / rowH());
        return i >= 0 && i < int(_items.size()) ? i : -1;
    }
    ConvSelector                              &_owner;
    std::vector<Target>                        _items;
    std::vector<std::unique_ptr<text::Layout>> _lines; // by item, shaped as last painted
    std::vector<Color>                         _lineFg;
    float                                      _w = 300, _scroll = 0;
    float                                      _linesW = -1, _linesScale = 0;
    int                                        _hover = -1, _current = -1;
};

class ConvSelector final : public View {
public:
    explicit ConvSelector(screens::Context &ctx) : _ctx(ctx) {
        style().row().height(36).padding(8, 0, 8, 0).spacing(4).items(Align::Center).noShrink();
        _edit = add<TextEdit>();
        _edit->style().flex(1);
        _edit->setMaxLines(1);
        _edit->setPlaceholder(tr("Search channels and people\xE2\x80\xA6"));
        _edit->onChange = [this] { refresh(); };
        _edit->onSubmit = [this] {
            if (_list && !_list->items().empty())
                pick(_list->items()[size_t(std::max(0, _list->current()))]);
            return true;
        };
        _chip = add<Clickable>();
        _chip->setVisible(false);
        _chip->setLook({C::None, C::None, C::None, C::None, 0});
        _chip->style().row().spacing(4).items(Align::Center);
        _chip->setCursor(plat::Cursor::IBeam); // only its × is clickable
        _chipLabel = _chip->add<Label>();
        auto *x    = _chip->add<Clickable>();
        x->style().size(18, 18);
        x->setLook({C::None, C::None, C::None, C::None, 0});
        _x         = x;
        x->onClick = [this] { clear(); };
        setCursor(plat::Cursor::IBeam);
    }
    ~ConvSelector() override { closeList(); }

    std::function<void(const Target *)> onPick;
    const Target                       *target() const { return _picked ? &_target : nullptr; }
    TextEdit                           &edit() { return *_edit; }

    void paint(gfx::Painter &p) override {
        const RectF b = bounds();
        p.fillRoundRect(b, 4, 0xffffffffU); // literal white, also in dark mode
        p.strokeRoundRect(b, 4, 1, ui::color(_edit->focused() ? C::Accent : C::FormDividerStrong));
        if (_chip->visible()) { // the chip: accent.subtleBg, radius 12
            const RectF c = _chip->frame(), l = _chipLabel->frame();
            p.fillRoundRect({c.x + l.x, c.y + l.y, l.w, l.h}, 12, ui::color(C::AccentSubtle));
        }
        View::paint(p);
    }
    void paintOver(gfx::Painter &p) override {
        if (!_chip->visible())
            return;
        // The chip: accent.subtleBg, replyLink bold text, radius 12, padding 2/8,
        // then the flat "×" (#666, #333 on hover).
        const RectF c  = _chip->frame();
        const RectF x  = _x->frame();
        const Color xc = _x->hovered() ? 0xff333333U : 0xff666666U;
        const float cx = c.x + x.x + 9, cy = c.y + x.y + 9;
        p.drawLine({cx - 4, cy - 4}, {cx + 4, cy + 4}, 1.3f, xc);
        p.drawLine({cx + 4, cy - 4}, {cx - 4, cy + 4}, 1.3f, xc);
    }
    bool onEvent(Event &e) override {
        if (e.type == EventType::PointerDown) {
            _edit->focus();
            return true;
        }
        if (e.type == EventType::FocusOut)
            _ctx.app.platform().post([this] {
                if (!_edit->focused())
                    closeList();
            });
        return false;
    }
    void pick(const Target &t) {
        _target = t;
        _picked = true;
        closeList();
        setStyledText(
            _chipLabel, t.label, ui::pxFont(15, text::Weight::Bold, ui::color(C::ReplyLink))
        );
        _chipLabel->style().padding(8, 2);
        _chip->setVisible(true);
        _edit->setVisible(false);
        _edit->clear();
        if (onPick)
            onPick(&_target);
    }
    void clear() {
        _picked = false;
        _chip->setVisible(false);
        _edit->setVisible(true);
        _edit->focus();
        if (onPick)
            onPick(nullptr);
    }
    void closeList() {
        if (PickerList *l = _list) {
            _list = nullptr;
            l->close();
        }
    }

private:
    void refresh() {
        closeList();
        std::string q(str::trim(_edit->text()));
        if (q.empty() || !window())
            return;
        const bool chans = q[0] == '#', people = q[0] == '@';
        if (chans || people)
            q.erase(0, 1);
        const std::string   fq = utf8::foldCase(q); // folded once, matched per row
        const model::Store &st = _ctx.store;
        std::vector<Target> out;
        auto                match = [&](std::string_view s) {
            return fq.empty() || utf8::containsFoldedNeedle(s, fq);
        };
        std::vector<bool> hasDm(st.userCount(), false); // people with a DM
        for (ConvRef c = 0; c < st.conversationCount(); ++c) {
            const auto &cv = st.conversation(c);
            if (cv.kind == ConvKind::Dm && cv.dmUser < hasDm.size())
                hasDm[cv.dmUser] = true;
            if (!cv.member)
                continue;
            const bool direct = cv.isDirect();
            if ((chans && direct) || (people && !direct))
                continue;
            const std::string name = st.displayName(c);
            if (match(name))
                out.push_back({c, kNoUser, direct ? name : "#" + name});
        }
        // An agent workspace's teammates:
        // writing to one starts a session with it.
        if (!chans && _ctx.backend.capabilities().agentSessions)
            for (const model::Backend::AgentRole &r : _ctx.backend.agentRoles())
                if (!r.id.empty() && match(r.name))
                    out.push_back({kNoConv, kNoUser, r.name, r.id});
        if (!chans) { // people without a DM (up to 50): prefix matches first
            struct Person {
                bool        prefix;
                std::string folded; // the label, folded: the order
                Target      target;
            };
            std::vector<Person> ppl;
            for (UserRef u = 0; u < st.userCount(); ++u) {
                const model::User &us = st.user(u);
                if (us.bot || us.placeholder || u == st.me || hasDm[u])
                    continue;
                const std::string_view label = us.label();
                if (!match(label))
                    continue;
                std::string folded = utf8::foldCase(label);
                const bool  prefix = str::startsWith(folded, fq);
                ppl.push_back({prefix, std::move(folded), {kNoConv, u, std::string(label)}});
            }
            const auto top = ppl.begin() + long(std::min<size_t>(ppl.size(), 50));
            std::partial_sort(ppl.begin(), top, ppl.end(), [](const Person &a, const Person &b) {
                return a.prefix != b.prefix ? a.prefix : a.folded < b.folded;
            });
            for (auto it = ppl.begin(); it != top; ++it)
                out.push_back(std::move(it->target));
        }
        if (out.empty())
            return;
        auto l = std::make_unique<PickerList>(*this, std::move(out));
        l->setWidth(width());
        l->setAnchor(windowRect(), Popup::Place::Below);
        _list = l.get();
        window()->showPopup(std::move(l));
    }

    screens::Context &_ctx;
    TextEdit         *_edit;
    Clickable        *_chip, *_x;
    Label            *_chipLabel;
    PickerList       *_list = nullptr;
    Target            _target;
    bool              _picked = false;
};

bool PickerList::onEvent(Event &e) {
    switch (e.type) {
    case EventType::PointerMove: {
        const int i = rowAt(e.pos.y);
        if (i != _hover) {
            _hover = i;
            update();
        }
        return true;
    }
    case EventType::PointerDown:
        if (const int i = rowAt(e.pos.y); i >= 0) {
            const Target t = _items[size_t(i)];
            _owner.pick(t); // closes this list (deferred)
        }
        return true;
    case EventType::Scroll: {
        const float max = std::max(0.f, rowH() * float(_items.size()) + 4 - height());
        _scroll         = std::clamp(_scroll - e.dy * (e.precise ? 1.f : 30.f), 0.f, max);
        update();
        return true;
    }
    default:
        return false;
    }
}

// ── Forward ─────────────────────────────────────────────────────────────────

// What a forward's files become: `paths` to
// upload again, in file order; `links` that go along as text instead (a
// canvas, a file with nothing to fetch, or every file when the target can't
// take uploads); `temps` the downloaded copies among the paths, removed once
// the send is over.
struct ForwardFiles {
    std::vector<std::string> paths, links, temps;
};

// Local files go as they are; the service's are downloaded through the
// backend first (off the UI thread). done(files, error) once, later; a
// non-empty error = a download failed, nothing should be sent (and the
// copies are gone).
void fetchForwardedFiles(
    screens::Context                                    &ctx,
    const std::vector<model::File>                      &files,
    bool                                                 upload,
    std::function<void(ForwardFiles, std::string error)> done
) {
    struct Batch {
        std::vector<std::string>                             paths; // by file index
        ForwardFiles                                         out;
        int                                                  pending = 1;
        bool                                                 failed  = false;
        std::function<void(ForwardFiles, std::string error)> done;
    };
    auto batch  = std::make_shared<Batch>();
    batch->done = std::move(done);
    batch->paths.resize(files.size());
    auto finish = [batch] {
        if (batch->failed || --batch->pending > 0)
            return;
        for (std::string &p : batch->paths)
            if (!p.empty())
                batch->out.paths.push_back(std::move(p));
        batch->done(std::move(batch->out), {});
    };
    auto fail = [batch](const std::string &err) {
        if (std::exchange(batch->failed, true))
            return;
        for (const std::string &t : batch->out.temps) // those still coming go as they land
            screens::removeTempDownload(t);
        batch->done({}, err.empty() ? std::string("download failed") : err);
    };
    // Held at 1 until every download has started: one that answers at once
    // can't finish the batch early.
    for (size_t i = 0; i < files.size() && !batch->failed; ++i) {
        const model::File &f      = files[i];
        const std::string &src    = f.source();
        const bool         remote = screens::RemoteImages::isRemote(src);
        if (src.empty() || !upload || f.mime == "application/vnd.slack-docs") { // a canvas
            if (!src.empty())
                batch->out.links.push_back(remote ? src : file::toFileUrl(src));
            continue;
        }
        if (!remote && file::exists(src) && !file::isDir(src)) { // already on disk
            batch->paths[i] = src;
            continue;
        }
        const std::string to = screens::tempDownloadPath(ctx.app.platform(), f.name);
        if (to.empty()) {
            fail("no temporary folder");
            break;
        }
        batch->out.temps.push_back(to);
        ++batch->pending;
        screens::fetchFile(
            ctx.app.platform(),
            ctx.backend,
            src,
            to,
            [batch, finish, fail, i, to](bool ok, const std::string &err) {
                if (batch->failed) {
                    screens::removeTempDownload(to);
                    return;
                }
                if (!ok)
                    return fail(err);
                batch->paths[i] = to;
                finish();
            }
        );
    }
    finish();
}

// A Context over another workspace's Store and Backend (the forward
// dialog's picker and composer work in the workspace the message goes to):
// the rest is the window's.
std::unique_ptr<screens::Context>
contextFor(const screens::Context &base, model::Store &store, model::Backend &backend) {
    auto c = std::unique_ptr<screens::Context>(
        new screens::Context{base.app, store, backend, base.images, {}, {}, {}, {}, {}}
    );
    c->openUrl        = base.openUrl;
    c->profileHover   = base.profileHover;
    c->window         = base.window;
    c->linkPreviews   = base.linkPreviews;
    c->remote         = base.remote;
    c->ai             = base.ai;
    c->openAiSettings = base.openAiSettings;
    return c;
}

class ForwardDialog final : public Dialog {
public:
    ForwardDialog(
        screens::Context                        &ctx,
        ConvRef                                  conv,
        model::Ts                                ts,
        std::string                              onlyFile,
        std::function<void(const std::string &)> onError,
        std::vector<ForwardWorkspace>            workspaces,
        PrefillTeammate                          prefillTeammate
    )
        : Dialog(onlyFile.empty() ? tr("Forward this message") : tr("Forward this file")),
          _conv(conv), _ts(ts), _onlyFile(std::move(onlyFile)), _onError(std::move(onError)),
          _workspaces(std::move(workspaces)), _prefillTeammate(std::move(prefillTeammate)) {
        // The message's workspace: the open one. Its own backend reads its
        // files (not the screens' proxy, which follows the next switch).
        model::Backend *srcBackend = &ctx.backend;
        int             at         = -1;
        for (size_t i = 0; i < _workspaces.size(); ++i)
            if (_workspaces[i].store == &ctx.store()) {
                srcBackend = _workspaces[i].backend;
                at         = int(i);
            }
        if (at < 0) { // the demo, tests: the open workspace alone
            _workspaces = {ForwardWorkspace{{}, {}, &ctx.store(), &ctx.backend, {}}};
            at          = 0;
        }
        _src = contextFor(ctx, ctx.store(), *srcBackend);
        // A picker above the selector when there is a choice of
        // workspace, starting on the message's.
        if (_workspaces.size() > 1) {
            std::vector<std::string> names;
            for (const ForwardWorkspace &w : _workspaces)
                names.push_back(w.name);
            _wsPicker           = content()->add<Dropdown>(std::move(names), at);
            _wsPicker->onChange = [this](int i) { setTarget(i); };
        }
        setTarget(at);
        if (const model::Message *m = _src->store().findMessage(conv, ts)) {
#ifdef MSGA_HAVE_MESSAGES
            if (_onlyFile.empty()) {
                screens::addMessagePreview(*_src, content(), *m, 140, true);
            } else {
                model::Message only = m->clone();
                only.text.clear();
                auto &files = only.extras().files;
                std::erase_if(files, [this](const model::File &f) { return f.path != _onlyFile; });
                screens::addMessagePreview(*_src, content(), only, 140, true);
            }
#endif
        }
        auto *copy = makeButton(tr("Copy link"), V::Secondary);
        _fwd       = makeButton(tr("Forward"), V::Primary);
        _fwd->setEnabled(false);
        addButtonRow(_fwd, makeButton(tr("Cancel"), V::Secondary), copy);
        copy->onClick = [this] { copyLink(); };
        _fwd->onClick = [this] { go(); };
    }
    // The views go first: they point into the Contexts below.
    ~ForwardDialog() override { clearChildren(); }
    void focusPicker() { _picker->edit().focus(); }
    // The picker's drop-down is a popup of its own: it closes with the
    // dialog, not later from the dialog's destructor (which runs while the
    // window disposes of closed popups).
    void willClose() override {
        if (_picker)
            _picker->closeList();
        Dialog::willClose();
    }

private:
    // The picker and the composer work in the
    // workspace `i` (a pick from the other one is cleared; the comment stays).
    void setTarget(int i) {
        if (i < 0 || size_t(i) >= _workspaces.size() || (i == _target && _picker))
            return;
        _target              = i;
        const std::string cm = _composer ? _composer->mrkdwn() : std::string();
        const int         at = _wsPicker ? 1 : 0;
        const bool        re = _picker != nullptr; // a workspace picked: its chats next
        if (_picker) {
            content()->remove(_picker);
            content()->remove(_composer);
            _picker   = nullptr;
            _composer = nullptr;
        }
        const ForwardWorkspace &w = _workspaces[size_t(i)];
        _tctx = w.store == &_src->store() ? nullptr : contextFor(*_src, *w.store, *w.backend);
        screens::Context &tc = _tctx ? *_tctx : *_src;
        _picker =
            static_cast<ConvSelector *>(content()->adopt(std::make_unique<ConvSelector>(tc), at));
        _composer = static_cast<Composer *>(
            content()->adopt(std::make_unique<Composer>(tc, _drafts), at + 1)
        );
        _composer->style().padding(0);
        _composer->style().maxH = 120;
        _composer->setPlaceholder(tr("Add a message, if you'd like."));
        _composer->setVoiceInput(false); // no voice input when forwarding
        if (!cm.empty())
            loadMrkdwn(_composer->edit(), tc.store(), cm);
        _composer->onSendRequest = [this] {
            if (_fwd->enabled())
                go();
            return true;
        };
        _picker->onPick = [this](const Target *t) { _fwd->setEnabled(t != nullptr); };
        if (_fwd)
            _fwd->setEnabled(false);
        if (re)
            _picker->edit().focus();
    }
    void copyLink() {
        const model::Store   &st = _src->store();
        const model::Message *m  = st.findMessage(_conv, _ts);
        if (!m)
            return;
        // As the message menu's "Copy link" (the message's permalink) and
        // "Copy link to file" (the file's page, else its original).
        std::string url;
        if (_onlyFile.empty()) {
            url = st.permalink(_conv, _ts, m->isReply() ? m->threadTs : 0);
        } else {
            url = screens::fileUrl(_onlyFile);
            for (const model::File &f : m->files())
                if (f.path == _onlyFile)
                    url = f.permalink.empty() ? screens::fileUrl(f.source()) : f.permalink;
        }
        if (!url.empty())
            _src->app.platform().setClipboardText(std::move(url));
    }
    void go() {
        const Target         *t = _picker->target();
        const model::Message *m = _src->store().findMessage(_conv, _ts);
        if (!t || !m)
            return;
        const ForwardWorkspace &to   = _workspaces[size_t(_target)];
        // No attribution header: the text is re-posted as mine — verbatim in
        // its own workspace, else as portable mrkdwn (mentions and channels
        // become the words they read as here) — after
        // the comment on its own line.
        const bool              same = to.store == &_src->store();
        const std::string       comment(str::trim(mrkdwn::convertOutgoing(_composer->mrkdwn())));
        const std::string       fwd     = !_onlyFile.empty() ? std::string()
                                          : same ? m->text
                                                 : portableMrkdwn(_src->store(), m->text);
        const std::string       full    = comment.empty() ? fwd
                                          : fwd.empty()   ? comment
                                                          : str::concat({comment, "\n", fwd});
        // Into the pick — a DM may be opened first — once the files are
        // here, unless that workspace went meanwhile; downloaded copies go
        // when the send is over.
        auto                    deliver = [to,
                                           conv    = t->conv,
                                           person  = t->person,
                                           role    = t->role,
                                           ws      = to.store->workspaceId,
                                           prefill = _prefillTeammate,
                                           onError = _onError](std::string text, ForwardFiles ff) {
            auto cleanup = [temps = ff.temps] {
                for (const std::string &t : temps)
                    screens::removeTempDownload(t);
            };
            const auto there = [to, ws] {
                return (!to.alive || to.alive()) && to.store->workspaceId == ws;
            };
            if (!there() || (text.empty() && ff.paths.empty()))
                return cleanup();
            if (conv == kNoConv && !role.empty()) {
                // The teammate's page with it all in
                // the composer, left to send (the copies stay for that send).
                if (prefill)
                    return prefill(to.key, role, std::move(text), std::move(ff.paths));
                cleanup();
                if (onError)
                    onError(tr("Couldn't open the chat."));
                return;
            }
            auto send = [to, there, text = std::move(text), paths = std::move(ff.paths), cleanup](
                            ConvRef c
                        ) {
                if (c == kNoConv || !there())
                    return cleanup();
                if (paths.empty()) {
                    to.backend->send(c, text, 0, nullptr);
                    return cleanup();
                }
                to.backend->sendWithFiles(c, text, 0, paths, [cleanup](bool, const std::string &) {
                    cleanup();
                });
            };
            if (conv != kNoConv) {
                send(conv);
            } else {
                to.backend->openDm(person, send);
            }
        };
        std::vector<model::File> files;
        for (const model::File &f : m->files())
            if (_onlyFile.empty() || f.path == _onlyFile)
                files.push_back(f);
        if (files.empty()) {
            deliver(full, {});
            accept();
            return;
        }
        // The files go along as uploads of their own bytes, fetched through
        // the workspace they live in; a target that can't take uploads gets
        // links.
        const int job = model::jobs().begin(
            files.size() == 1
                ? i18n::arg(tr("Forwarding %1"), files.front().name)
                : i18n::trn("Forwarding %n files", "Forwarding %n files", int64_t(files.size()))
        );
        fetchForwardedFiles(
            *_src,
            files,
            to.backend->capabilities().fileUpload,
            [job, full, deliver, onError = _onError](ForwardFiles ff, std::string err) {
                model::jobs().end(job);
                if (!err.empty()) {
                    LOG_WARN("shell", "forward: file download failed: %s", err.c_str());
                    if (onError)
                        onError(tr("Couldn't forward the file."));
                    return;
                }
                std::string text = full;
                for (const std::string &link : ff.links)
                    text += (text.empty() ? "" : "\n") + link;
                deliver(std::move(text), std::move(ff));
            }
        );
        accept();
    }

    ConvRef                                  _conv;
    model::Ts                                _ts;
    std::string                              _onlyFile;
    std::function<void(const std::string &)> _onError;
    std::vector<ForwardWorkspace>            _workspaces;
    int                                      _target = -1;
    std::unique_ptr<screens::Context>        _src, _tctx; // the message's; the pick's (if other)
    PrefillTeammate                          _prefillTeammate;
    DraftStash                               _drafts;
    Dropdown                                *_wsPicker = nullptr;
    ConvSelector                            *_picker   = nullptr;
    Composer                                *_composer = nullptr;
    Button                                  *_fwd      = nullptr;
};

// ── Picture dialogs ─────────────────────────────────────────────────────────

// The pictures in `dir` whose names start with `prefix`, sorted (the names
// carry the time they were saved: the last is the newest).
std::vector<std::string> listPictures(const std::string &dir, const std::string &prefix) {
    std::vector<std::string>    out;
    std::vector<file::DirEntry> es;
    if (file::listDir(dir, &es))
        for (const file::DirEntry &e : es)
            if (!e.isDir && str::startsWith(e.name, prefix))
                out.push_back(file::join(dir, e.name));
    std::sort(out.begin(), out.end());
    return out;
}

std::string iconDir(plat::App &app) {
    // Pictures from earlier versions are named slack_<id>-…, ours <id>-….
    return file::join(identity::dataDir(app), "workspace_icons");
}

std::string iconPrefix(const std::string &workspaceId) {
    return str::asciiLower(workspaceId) + "-";
}

// The workspace icon folder, listed once and again after a change of ours
// (a save, a sign-out): the rail and every notification ask for it.
struct IconIndex {
    std::string              dir;
    std::vector<std::string> files;
    bool                     listed = false;
};

IconIndex &iconIndex() {
    static IconIndex index;
    return index;
}

const std::vector<std::string> &iconFiles(plat::App &app) {
    IconIndex        &x   = iconIndex();
    const std::string dir = iconDir(app);
    if (!x.listed || x.dir != dir) {
        x.files  = listPictures(dir, {});
        x.dir    = dir;
        x.listed = true;
    }
    return x.files;
}

// <stem>-<msecs>.img naming: a new file each time, so nothing caches the
// old picture under the same path.
std::string newPictureName(std::string_view prefix) {
    return str::concat({prefix, str::number(base::nowMicros() / 1000), ".img"});
}

// The dialogs' common frame: the preview (the subclass's, added first),
// "Choose image…" and "Use default", the hint, Save and Cancel; a dropped
// image file is picked like a chosen one. Save copies the picture into the
// data folder (the original may move or vanish) on a worker and removes the
// earlier ones; a copy that fails keeps the dialog open and says so.
class ImagePickDialog : public Dialog {
public:
    bool onEvent(Event &e) override {
        if (e.type == EventType::DropEnter || e.type == EventType::DropMove) {
            if (!screens::dragOffersFiles(e.raw))
                return Dialog::onEvent(e);
            e.dropAction = plat::DropAction::Copy;
            return true;
        }
        if (e.type == EventType::Drop) {
            const auto p = screens::droppedFiles(e.raw);
            if (!p.empty())
                load(p[0]);
            return true;
        }
        return Dialog::onEvent(e);
    }

protected:
    // Where Save puts the picture: `dir`, as <prefix><msecs>.img; the files
    // there with that prefix are the earlier pictures, removed but `keep`.
    struct Target {
        std::string dir, prefix, keep;
    };

    ImagePickDialog(screens::Context &ctx, const char *title, const char *chooseTitle, bool custom)
        : Dialog(title, 440), _ctx(ctx), _hasCustom(custom), _chooseTitle(chooseTitle) {
        content()->style().spacing(8);
    }
    // The controls under the preview; `extra` at the button row's start.
    void buildControls(const char *hint, View *extra) {
        auto *row = content()->add<View>();
        row->style().row().spacing(8).items(Align::Center);
        row->add<View>()->style().flex(1);
        auto *choose =
            row->add<Button>(tr("Choose image\xE2\x80\xA6"), V::Secondary, Button::Form::Normal);
        _reset = row->add<Button>(tr("Use default"), V::Ghost, Button::Form::Normal);
        row->add<View>()->style().flex(1);
        _hint = styledLabel(
            content(), hint, ui::pxFont(11, text::Weight::Regular, ui::color(C::FormTextMuted))
        );
        _hint->setAlign(text::LayoutOptions::Align::Center);
        _save = makeButton(tr("Save"), V::Primary);
        addButtonRow(_save, makeButton(tr("Cancel"), V::Secondary), extra);
        choose->onClick = [this] { choose_(); };
        _reset->onClick = [this] {
            _chosen.clear();
            _resetOn = true;
            reset();
            refresh();
        };
        _save->onClick = [this] { save(); };
        refresh();
    }
    // A picture to try (chosen or dropped); loaded(path) once it is one,
    // notAnImage() when it isn't.
    virtual void   load(const std::string &path) = 0;
    // "Use default": the preview without a picture of its own.
    virtual void   reset()                       = 0;
    virtual Target target() const                = 0;
    // Saved: the picture's new path ("" = the default).
    virtual void   saved(const std::string &now) = 0;

    void loaded(const std::string &path) {
        _chosen  = path;
        _resetOn = false;
        _dirty   = true;
        refresh();
    }
    void notAnImage() { setHint(tr("That file could not be read as an image."), C::FormTextMuted); }
    void refresh() {
        _reset->setVisible(_hasCustom || !_chosen.empty());
        _reset->setEnabled(!_resetOn);
        _save->setEnabled(_dirty && !_saving);
    }

    screens::Context    &_ctx;
    std::string          _chosen;
    bool                 _hasCustom, _resetOn = false, _dirty = false;
    std::shared_ptr<int> _alive = std::make_shared<int>(0); // guards the workers' answers

private:
    void setHint(const char *text, C color) {
        setStyledText(_hint, text, ui::pxFont(11, text::Weight::Regular, ui::color(color)));
    }
    void choose_() {
        plat::FileDialogDesc d;
        d.mode    = plat::FileDialogDesc::Mode::Open;
        d.title   = tr(_chooseTitle);
        d.filters = {
            {tr("Images"), {"*.png", "*.jpg", "*.jpeg", "*.webp", "*.gif", "*.bmp", "*.svg"}}
        };
        screens::fileDialog(_ctx, std::move(d), [this](std::vector<std::string> p) {
            if (!p.empty())
                load(p[0]);
        });
    }
    // The copy and the clean-up on a worker, then saved() and the dialog
    // closes, or it stays open and says the picture could not be saved.
    void save() {
        if (!_dirty || _saving)
            return;
        _saving                = true;
        const Target      t    = target();
        const std::string from = _resetOn ? std::string() : _chosen;
        const std::string dest =
            from.empty() ? std::string() : file::join(t.dir, newPictureName(t.prefix));
        auto ok = std::make_shared<bool>(false);
        refresh();
        model::runInBackground(
            _ctx.app.platform(),
            [t, from, dest, ok] {
                std::string bytes;
                if (!dest.empty() && (!file::makeDirs(t.dir) || !file::readAll(from, &bytes) ||
                                      !file::writeAtomic(dest, bytes)))
                    return;
                const std::string &now = dest.empty() ? t.keep : dest;
                for (const std::string &f : listPictures(t.dir, t.prefix)) // the earlier ones go
                    if (f != now)
                        file::remove(f);
                *ok = true;
            },
            [this, alive = std::weak_ptr<int>(_alive), t, dest, ok] {
                if (alive.expired())
                    return;
                _saving = false;
                if (!*ok) {
                    LOG_WARN("shell", "The picture could not be saved.");
                    setHint(tr("The icon could not be saved."), C::FormError);
                    refresh();
                    return;
                }
                saved(dest.empty() ? t.keep : dest);
                accept();
            }
        );
    }

    const char *_chooseTitle;
    Button     *_reset = nullptr, *_save = nullptr;
    Label      *_hint   = nullptr;
    bool        _saving = false; // a save on its worker
};

// ── Workspace icon ──────────────────────────────────────────────────────────

// The rail's workspace bubble at 96 px, radius 24: the picture, else the
// first letter on the workspace colour (HSL(hash·37 mod 360, 65 %, 42 %)).
class IconPreview final : public View {
public:
    IconPreview(screens::Context &ctx) : _ctx(ctx) {
        style().size(96, 96).alignSelf(Align::Center);
    }
    void set(std::shared_ptr<const gfx::Bitmap> b) {
        _bmp = std::move(b);
        update();
    }
    void styleChanged() override {
        _letter.reset();
        View::styleChanged();
    }
    bool showsBitmap(const gfx::Bitmap *b) const override { return _bmp.get() == b; }
    void paint(gfx::Painter &p) override {
        const RectF r{0, 0, 96, 96};
        if (_bmp && !_bmp->empty()) {
            p.save();
            p.clipRoundRect(r, 24);
            p.drawBitmap(_bmp->view(), r, gfx::Sampling::Smooth);
            p.restore();
            return;
        }
        uint32_t h = 0;
        for (char c : _ctx.store().workspaceId)
            h = h * 31 + uint8_t(c);
        const std::string &n      = _ctx.store().workspaceName;
        const std::string  letter = n.empty() ? std::string("?") : screens::avatarInitial(n);
        if (letter != _shown) { // another workspace, a rename
            _shown = letter;
            _letter.reset();
        }
        screens::paintInitial(
            p,
            *this,
            r,
            24,
            screens::hsl(float((h * 37) % 360), 0.65f, 0.42f),
            letter,
            _letter,
            std::round(96.f * 17 / 40)
        );
    }

private:
    screens::Context                  &_ctx;
    std::shared_ptr<const gfx::Bitmap> _bmp;
    std::string                        _shown;
    std::unique_ptr<text::Layout>      _letter;
};

constexpr const char *kIconHint =
    N_("Only you see this icon. The picture is cropped to a square. You can also drop an image "
       "file onto this window.");

class WorkspaceIconDialog final : public ImagePickDialog {
public:
    WorkspaceIconDialog(
        screens::Context                        &ctx,
        Avatars                                 &avatars,
        std::string                              workspaceId,
        std::string                              defaultIcon,
        std::string                              current,
        std::function<void(const std::string &)> done
    )
        : ImagePickDialog(
              ctx,
              tr("Workspace icon"),
              N_("Choose workspace icon"),
              !customWorkspaceIconPath(ctx.app.platform(), workspaceId).empty()
          ),
          _avatars(avatars), _id(std::move(workspaceId)), _defaultIcon(std::move(defaultIcon)),
          _done(std::move(done)) {
        _preview = content()->add<IconPreview>(ctx);
        _preview->set(avatars.get(current, 192));
        buildControls(tr(kIconHint), new View());
    }

private:
    void load(const std::string &path) override {
        // Decoded on the worker: the answer comes once it is.
        std::weak_ptr<int> alive = _alive;
        _avatars.whenReady(path, 192, [this, alive, path](Avatars::Picture bmp) {
            if (alive.expired())
                return;
            if (!bmp)
                return notAnImage();
            _preview->set(std::move(bmp));
            loaded(path);
        });
    }
    void reset() override {
        _dirty = _hasCustom;
        _preview->set(_avatars.get(_defaultIcon, 192));
    }
    Target target() const override { return {iconDir(_ctx.app.platform()), iconPrefix(_id), {}}; }
    void   saved(const std::string &now) override {
        iconIndex().listed = false; // listed again on the next look
        if (_done)
            _done(now);
    }

    Avatars                                 &_avatars;
    std::string                              _id, _defaultIcon; // the workspace's
    IconPreview                             *_preview = nullptr;
    std::function<void(const std::string &)> _done;
};

// ── Tray icon ───────────────────────────────────────────────────────────────

// The preview: a dark panel stand-in (most trays are dark,
// and the built-in icon is white), the icon at 56 px inside it, the unread
// dot over its corner at the tray's proportions (36 of 128).
class TrayPreview final : public View {
public:
    TrayPreview() { style().size(96, 96).alignSelf(Align::Center); }
    void set(std::shared_ptr<const gfx::Bitmap> b) {
        _bmp = std::move(b);
        update();
    }
    void paint(gfx::Painter &p) override {
        p.fillRoundRect({0, 0, 96, 96}, 16, ui::color(C::Sidebar));
        const RectF icon{20, 20, 56, 56};
        if (_bmp)
            p.drawBitmap(_bmp->view(), icon, gfx::Sampling::Smooth);
        else
            gfx::drawIcon(p, gfx::Icon::Tray, icon, 0xffffffffU);
        const float d = 56.f * 36 / kTrayIconSize;
        p.fillCircle({icon.x + icon.w - d / 2, icon.y + icon.h - d / 2}, d / 2, 0xffcd2553U);
    }

private:
    std::shared_ptr<const gfx::Bitmap> _bmp;
};

class TrayIconDialog final : public ImagePickDialog {
public:
    TrayIconDialog(screens::Context &ctx, std::string current, bool monochrome, TrayIconDone done)
        : ImagePickDialog(ctx, tr("Tray icon"), N_("Choose tray icon"), !current.empty()),
          _current(std::move(current)), _done(std::move(done)) {
        _preview = content()->add<TrayPreview>();
        _mono    = new CheckBox(tr("Convert to monochrome"), monochrome);
        buildControls(
            tr("The picture is fitted into a square, and the unread dot is drawn over its corner. "
               "You can also drop an image file onto this window."),
            _mono
        );
        _mono->onChange = [this](bool) {
            _dirty = true; // an option alone changed
            refreshPreview();
            refresh();
        };
        if (_hasCustom)
            decode(_current, [this](std::shared_ptr<const gfx::Bitmap> bmp) {
                _picture = std::move(bmp);
                refreshPreview();
            });
        refreshPreview();
    }

private:
    // Read, decoded and fitted into the square on a worker; done(null) when
    // it isn't a picture. Only the latest call answers.
    void
    decode(const std::string &path, std::function<void(std::shared_ptr<const gfx::Bitmap>)> done) {
        const unsigned gen = ++_loading;
        auto           out = std::make_shared<std::shared_ptr<const gfx::Bitmap>>();
        model::runInBackground(
            _ctx.app.platform(),
            [out, path] {
                std::string bytes;
                gfx::Bitmap bmp;
                if (file::readAll(path, &bytes) && decodeTrayPicture(bytes, &bmp))
                    *out = std::make_shared<gfx::Bitmap>(trayPicture(bmp, false));
            },
            [this, alive = std::weak_ptr<int>(_alive), gen, out, done = std::move(done)] {
                if (!alive.expired() && gen == _loading)
                    done(std::move(*out));
            }
        );
    }
    void refreshPreview() {
        // The fitted picture is kept; the silhouette, a 128 px pass, is made here.
        std::shared_ptr<gfx::Bitmap> b;
        if (_picture) {
            b = std::make_shared<gfx::Bitmap>(*_picture);
            if (_mono->checked())
                trayMonochrome(b.get());
        }
        _preview->set(std::move(b));
    }
    void load(const std::string &path) override {
        decode(path, [this, path](std::shared_ptr<const gfx::Bitmap> bmp) {
            if (!bmp)
                return notAnImage();
            _picture = std::move(bmp);
            refreshPreview();
            loaded(path);
        });
    }
    void reset() override {
        ++_loading; // a picture still decoding is not wanted now
        _picture.reset();
        _dirty = true;
        refreshPreview();
    }
    // The data folder (app/identity.h), where earlier versions' tray_icon.png
    // is too; ours have names of their own, so a rollback finds its picture.
    Target target() const override {
        return {
            identity::dataDir(_ctx.app.platform()),
            "tray_icon-",
            _resetOn ? std::string() : _current
        };
    }
    void saved(const std::string &now) override {
        if (_done)
            _done(now, _mono->checked());
    }

    std::string                        _current;
    std::shared_ptr<const gfx::Bitmap> _picture; // decoded and fitted, before monochrome
    TrayPreview                       *_preview = nullptr;
    CheckBox                          *_mono    = nullptr;
    TrayIconDone                       _done;
    unsigned                           _loading = 0; // the latest decode()'s number
};

} // namespace

Popup *showForwardDialog(
    screens::Context                        &ctx,
    Window                                  &w,
    ConvRef                                  conv,
    model::Ts                                ts,
    const std::string                       &onlyFile,
    std::function<void(const std::string &)> onError,
    std::vector<ForwardWorkspace>            workspaces,
    PrefillTeammate                          prefillTeammate
) {
    if (!ctx.store().findMessage(conv, ts))
        return nullptr;
    auto d = std::make_unique<ForwardDialog>(
        ctx,
        conv,
        ts,
        onlyFile,
        std::move(onError),
        std::move(workspaces),
        std::move(prefillTeammate)
    );
    auto *raw = d.get();
    w.showPopup(std::move(d));
    raw->focusPicker();
    return raw;
}

std::string portableMrkdwn(const model::Store &source, std::string_view in) {
    std::string out;
    out.reserve(in.size());
    size_t pos = 0;
    while (pos < in.size()) {
        const size_t open  = in.find('<', pos);
        const size_t close = open == std::string_view::npos ? open : in.find('>', open);
        if (close == std::string_view::npos) {
            out.append(in.substr(pos));
            break;
        }
        out.append(in.substr(pos, open - pos));
        pos                        = close + 1;
        const std::string_view tok = in.substr(open + 1, close - open - 1);
        const size_t           bar = tok.find('|');
        const std::string_view id =
            tok.substr(1, bar == std::string_view::npos ? tok.npos : bar - 1);
        const std::string label(
            bar == std::string_view::npos ? std::string_view() : tok.substr(bar + 1)
        );
        if (tok.empty() || (tok[0] != '@' && tok[0] != '#' && tok[0] != '!')) {
            out.append(in.substr(open, close - open + 1)); // a link: the same everywhere
            continue;
        }
        std::string words;
        if (tok[0] == '@') {
            const model::UserRef u = source.findUser(id);
            words                  = "@" + (u != model::kNoUser && !source.user(u).placeholder
                                                ? std::string(source.user(u).label())
                                            : !label.empty() ? label
                                                             : std::string(id));
        } else if (tok[0] == '#') {
            const model::ConvRef c = source.findConversation(id);
            words                  = "#" + (c != model::kNoConv ? source.displayName(c)
                                            : !label.empty()    ? label
                                                                : std::string(id));
        } else if (str::startsWith(id, "subteam^")) {
            words = !label.empty() ? label : "@" + std::string(id.substr(8));
            if (words[0] != '@')
                words.insert(0, 1, '@');
        } else if (str::startsWith(id, "date^")) {
            words = label;
        } else { // here, channel, everyone: plain words, no broadcast
            words = "@" + std::string(id.substr(0, id.find('^')));
        }
        // The words go out as text: mrkdwn's escapes keep them from turning
        // into tokens again on the other side.
        out += mrkdwn::escapeEntities(mrkdwn::decodeEntities(words));
    }
    return out;
}

bool decodeTrayPicture(std::string_view bytes, gfx::Bitmap *out) {
    // SVG first, rendered straight at the stored size (a 24 px icon would
    // otherwise be upscaled blurry).
    float w = 0, h = 0;
    if (gfx::svgSize(bytes, &w, &h) && w > 0 && h > 0) {
        const float k = float(kTrayIconSize) / std::max(w, h);
        return gfx::renderSvg(
            bytes, std::max(1, int(std::lround(w * k))), std::max(1, int(std::lround(h * k))), out
        );
    }
    return gfx::decodeImage(bytes, out) && !out->empty();
}

gfx::Bitmap trayPicture(const gfx::Bitmap &src, bool monochrome) {
    // Fitted into the square, aspect kept and centred: a logo must not lose
    // its edges to a crop.
    constexpr int n = kTrayIconSize;
    gfx::Bitmap   out(n, n);
    if (src.empty())
        return out;
    const float k  = std::min(float(n) / src.width(), float(n) / src.height());
    const int   w  = std::max(1, int(std::lround(src.width() * k)));
    const int   h  = std::max(1, int(std::lround(src.height() * k)));
    gfx::Bitmap sc = gfx::resize(src.view(), w, h);
    const int   x0 = (n - w) / 2, y0 = (n - h) / 2;
    for (int y = 0; y < h; ++y)
        std::copy_n(sc.pixels() + size_t(y) * w, w, out.pixels() + size_t(y + y0) * n + x0);
    if (monochrome)
        trayMonochrome(&out);
    return out;
}

void trayMonochrome(gfx::Bitmap *bmp) {
    // A white silhouette. The shape comes from the alpha
    // channel when the picture has transparency inside its visible area (a
    // logo cut out of its background); an opaque one (a logo on a solid
    // backdrop, a photo) keys on each pixel's colour distance from the
    // backdrop sampled at the corners, so dark-on-light and light-on-dark
    // logos both come out as their mark.
    const int  W = bmp->width(), H = bmp->height();
    uint32_t  *px    = bmp->pixels();
    const auto alpha = [](uint32_t c) { return int(c >> 24); };
    int        x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (alpha(px[y * W + x]) > 0) {
                x0 = std::min(x0, x), y0 = std::min(y0, y);
                x1 = std::max(x1, x), y1 = std::max(y1, y);
            }
    if (x1 < 0)
        return; // fully transparent: nothing to shape
    bool translucent = false;
    for (int y = y0; y <= y1 && !translucent; ++y)
        for (int x = x0; x <= x1; ++x)
            if (alpha(px[y * W + x]) < 250) {
                translucent = true;
                break;
            }
    const auto ch = [](uint32_t c, int shift) { return int((c >> shift) & 0xff); };
    int        br = 0, bg = 0, bb = 0, maxDist = 1;
    const auto dist = [&](uint32_t c) {
        return std::max(
            std::max(std::abs(ch(c, 16) - br), std::abs(ch(c, 8) - bg)), std::abs(ch(c, 0) - bb)
        );
    };
    if (!translucent) {
        for (const uint32_t c :
             {px[y0 * W + x0], px[y0 * W + x1], px[y1 * W + x0], px[y1 * W + x1]})
            br += ch(c, 16), bg += ch(c, 8), bb += ch(c, 0);
        br /= 4, bg /= 4, bb /= 4;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                maxDist = std::max(maxDist, dist(px[y * W + x]));
    }
    // A flat picture (one colour, give or take compression noise) has no
    // mark to key out: the whole square is the silhouette.
    constexpr int kFlatContrast = 24;
    const bool    flat          = !translucent && maxDist < kFlatContrast;
    for (int i = 0; i < W * H; ++i) {
        const int a0 = alpha(px[i]);
        const int a =
            translucent || flat || a0 == 0 ? a0 : std::min(255, dist(px[i]) * 2 * 255 / maxDist);
        px[i] = uint32_t(a) << 24 | uint32_t(a) << 16 | uint32_t(a) << 8 |
                uint32_t(a); // premultiplied white
    }
}

Popup *showTrayIconDialog(
    screens::Context &ctx, Window &w, const std::string &current, bool monochrome, TrayIconDone done
) {
    auto  d   = std::make_unique<TrayIconDialog>(ctx, current, monochrome, std::move(done));
    auto *raw = d.get();
    w.showPopup(std::move(d));
    return raw;
}

std::string customWorkspaceIconPath(plat::App &app, const std::string &workspaceId) {
    const std::string               prefix = iconPrefix(workspaceId);
    const std::vector<std::string> &files  = iconFiles(app);
    for (auto it = files.rbegin(); it != files.rend(); ++it) // the newest first
        if (str::startsWith(file::baseName(*it), prefix))
            return *it;
    return {};
}

std::string
workspaceIcon(plat::App &app, const std::string &workspaceId, const std::string &serverIcon) {
    std::string custom = customWorkspaceIconPath(app, workspaceId);
    return custom.empty() ? serverIcon : custom;
}

void removeCustomWorkspaceIcon(plat::App &app, const std::string &workspaceId) {
    const std::string custom = customWorkspaceIconPath(app, workspaceId);
    if (custom.empty())
        return;
    file::remove(custom);
    iconIndex().listed = false;
}

Popup *showRenameDialog(screens::Context &ctx, Window &w, ConvRef c) {
    if (c >= ctx.store().conversationCount())
        return nullptr;
    const model::Conversation &cv    = ctx.store().conversation(c);
    const bool                 agent = ctx.backend.isAgentSession(c);
    auto d = std::make_unique<Dialog>(agent ? tr("Rename session") : tr("Name conversation"));
    styledLabel(d->content(), tr("Name"), pxFont(15, text::Weight::Bold, color(C::FormText)));
    auto *field = d->content()->add<TextField>(
        // Empty: the placeholder is the name it falls back to (derivedName).
        cv.localName.empty() ? std::string(ctx.store().displayName(c)) : std::string(),
        TextField::Size::Normal
    );
    field->setMaxLength(80); // Slack's channel-name cap
    field->setText(cv.localName);
    field->edit().selectAll();
    styledLabel(
        d->content(),
        agent ? tr("Only msga shows this name; Claude Code keeps its own. Leave it empty to use "
                   "Claude Code's name again.")
              : tr("Only you see this name. Leave it empty to show the members' names again."),
        pxFont(11, text::Weight::Regular, color(C::FormTextMuted))
    );
    auto *save = Dialog::makeButton(tr("Save"), V::Primary);
    d->addButtonRow(save, Dialog::makeButton(tr("Cancel"), V::Secondary));
    Dialog *raw   = d.get();
    // Read the field before accept() closes the dialog.
    save->onClick = [raw, field, &ctx, c] {
        std::string name(str::trim(field->text()));
        raw->accept();
        ctx.backend.setLocalName(c, std::move(name));
    };
    field->onReturn = save->onClick;
    w.showPopup(std::move(d));
    field->edit().focus();
    return raw;
}

Popup *showWorkspaceIconDialog(
    screens::Context                        &ctx,
    Window                                  &w,
    Avatars                                 &avatars,
    std::function<void(const std::string &)> done
) {
    return showWorkspaceIconDialog(
        ctx, w, avatars, ctx.store().workspaceId, ctx.store().workspaceIcon, std::move(done)
    );
}

Popup *showWorkspaceIconDialog(
    screens::Context                        &ctx,
    Window                                  &w,
    Avatars                                 &avatars,
    const std::string                       &workspaceId,
    const std::string                       &defaultIcon,
    std::function<void(const std::string &)> done
) {
    const std::string cur = workspaceIcon(ctx.app.platform(), workspaceId, defaultIcon);
    auto              d   = std::make_unique<WorkspaceIconDialog>(
        ctx, avatars, workspaceId, defaultIcon, cur, std::move(done)
    );
    auto *raw = d.get();
    w.showPopup(std::move(d));
    return raw;
}

Popup *showSectionDialog(screens::Context &ctx, Window &w, const std::string &id, ConvRef first) {
    const model::Store::Section *sec = id.empty() ? nullptr : ctx.store().findSection(id);
    if (!id.empty() && !sec)
        return nullptr;
    auto d = std::make_unique<Dialog>(sec ? tr("Rename section") : tr("Create a section"));
    styledLabel(d->content(), tr("Name"), pxFont(15, text::Weight::Bold, color(C::FormText)));
    // The emoji button and the name in one bordered box, as in "Set a status".
    auto *box = d->content()->add<View>();
    box->style().row().items(Align::Center).padding(8, 4).spacing(8).height(40);
    box->setBackground(C::FormBg, 6);
    box->setBorder(C::FieldBorder);
    auto *emojiBtn = box->add<Clickable>();
    emojiBtn->style().size(30, 30).stack().items(Align::Center).noShrink();
    emojiBtn->setLook({C::None, C::FormSunken, C::FormSunken, C::None, 6});
    auto *smiley = emojiBtn->add<IconView>(gfx::Icon::Smile, 20, C::FormTextFaint);
    auto *glyph  = emojiBtn->add<Label>("", Font::Title);
    auto *field  = box->add<TextEdit>();
    field->style().flex(1);
    field->setMaxLines(1);
    field->setFont(Font::Field);
    field->setPlaceholder(tr("e.g. Projects"));
    field->onChange = [field] {
        const std::string &t = field->text();
        if (const size_t n = utf8::prefixBytes(t, 80); n < t.size()) // Slack's cap
            field->setText(t.substr(0, n));
    };
    auto emoji = std::make_shared<std::string>();
    auto show  = [&ctx, emoji, smiley, glyph](std::string name) {
        *emoji              = std::move(name);
        const std::string g = emoji->empty() ? std::string() : ctx.store().emojiFor(*emoji).unicode;
        smiley->setVisible(g.empty());
        glyph->setVisible(!g.empty());
        glyph->setText(g);
    };
    show(sec ? sec->emoji : std::string());
    if (sec)
        field->setText(sec->name);
#ifdef MSGA_HAVE_MESSAGES
    emojiBtn->onClick = [&ctx, emojiBtn, field, show] {
        if (Window *win = emojiBtn->window())
            screens::EmojiPicker::show(
                *win, emojiBtn->windowRect(), ctx, [field, show](const std::string &n) {
                    show(n);
                    field->focus();
                }
            );
    };
#endif
    styledLabel(
        d->content(),
        tr("Only you see your sections."),
        pxFont(11, text::Weight::Regular, color(C::FormTextMuted))
    );
    auto *save = Dialog::makeButton(sec ? tr("Save") : tr("Create"), V::Primary);
    d->addButtonRow(save, Dialog::makeButton(tr("Cancel"), V::Secondary));
    Dialog *raw   = d.get();
    // Read the field before accept() closes the dialog.
    save->onClick = [raw, field, emoji, &ctx, id, first] {
        std::string name(str::trim(field->text()));
        if (name.empty()) {
            field->focus();
            return;
        }
        std::string e = *emoji;
        raw->accept();
        if (id.empty())
            ctx.backend.createSection(std::move(name), std::move(e), first);
        else
            ctx.backend.updateSection(id, std::move(name), std::move(e));
    };
    field->onSubmit = [save] { return save->onClick(), true; };
    w.showPopup(std::move(d));
    field->focus();
    return raw;
}

Popup *showDeleteSectionDialog(screens::Context &ctx, Window &w, const std::string &id) {
    const model::Store::Section *sec = ctx.store().findSection(id);
    if (!sec)
        return nullptr;
    auto d = Dialog::confirm(
        tr("Delete section"),
        i18n::arg(
            tr("Delete \"%1\"? Its conversations go back to where they were listed before."),
            sec->name
        ),
        tr("Delete"),
        V::Danger,
        themed(C::FormText)
    );
    Dialog *raw   = d.get();
    d->onAccepted = [&ctx, id] { ctx.backend.deleteSection(id); };
    w.showPopup(std::move(d));
    return raw;
}

} // namespace shell
