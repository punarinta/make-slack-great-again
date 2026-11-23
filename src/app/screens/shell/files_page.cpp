#include "screens/shell/files_page.h"

#include "app/screens/common/file_badge.h"
#include "app/screens/common/message_text.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "gfx/icons_generated.h"
#include "text/text.h"
#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/image_cache.h"
#include "app/screens/messages/message_list.h"
#endif

#include <cmath>

using namespace ui;
using i18n::tr;

namespace shell {

namespace {

constexpr float kTile = 40;

// The type square: the colour of the file's kind, its short type in white
// (or the code glyph).
class TypeTile final : public View {
public:
    explicit TypeTile(screens::FileBadge b) : _b(std::move(b)) {
        style().size(kTile, kTile).noShrink();
        setHitTransparent(true);
    }
    void paint(gfx::Painter &p) override {
        p.fillRoundRect({0, 0, kTile, kTile}, 6, _b.color);
        if (_b.code) {
            gfx::drawIcon(p, gfx::Icon::CodeFile, {10, 10, 20, 20}, 0xffffffffU);
            return;
        }
        if (!_l || _scale != windowScale()) {
            _scale = windowScale();
            _l     = text::layoutPlain(
                _b.label, pxFont(15 * 0.66f, text::Weight::Bold, 0xffffffffU), _scale
            );
        }
        _l->paint(
            p,
            snapPx({std::floor((kTile - _l->width()) / 2), std::floor((kTile - _l->height()) / 2)})
        );
    }

private:
    screens::FileBadge            _b;
    std::unique_ptr<text::Layout> _l;
    float                         _scale = 0;
};

} // namespace

class FilesPage::Row final : public Clickable {
public:
    Row(FilesPage &page, size_t index, const model::SharedFile &sf, std::string sub) {
        setRole(Role::Button);
        setLook({C::None, C::FormHighlight, C::FormHighlight, C::None, 8});
        style().row().padding(12, 8, 12, 8).spacing(12).items(Align::Center).noShrink();
        const model::File &f = sf.file;
#ifdef MSGA_HAVE_MESSAGES
        if (f.hasPreview()) {
            auto *pic = add<screens::CachedImage>(
                page._ctx.images,
                f.isImage() ? f.path : f.thumb,
                screens::ImageCache::Shape::Square,
                6
            );
            pic->style().size(kTile, kTile).noShrink();
            pic->setPlaceholder(C::FormHighlight);
            pic->setHitTransparent(true);
        } else
#endif
            add<TypeTile>(screens::fileBadge(f));
        auto *col = add<View>();
        col->style().flex(1).spacing(2);
        col->style().shrink = 1;
        col->setHitTransparent(true);
        const std::string &title = f.isCanvas() && !f.title.empty() ? f.title : f.name;
        _name                    = col->add<Label>(title, Font::BodyBold);
        _name->setMaxLines(1);
        _name->setHitTransparent(true);
        _sub = col->add<Label>(std::move(sub), Font::Small, C::TextMuted);
        _sub->setMaxLines(1);
        _sub->setHitTransparent(true);
        onClick = [&page, index] { page.activate(index); };
    }
    std::string accessibleName() const override { return _name->text(); }
    std::string text() const { return _name->text() + "\n" + _sub->text(); }

private:
    Label *_name = nullptr, *_sub = nullptr;
};

FilesPage::FilesPage(screens::Context &ctx) : _ctx(ctx) {
    style().stack();
    setBackground(C::Surface);
    _scroll = add<ScrollView>();
    _scroll->setBackground(C::Surface);
    View *content = _scroll->content();
    content->style().padding(12, 12, 12, 12).spacing(2);
    _list = content->add<View>();
    _list->style().spacing(2);
    _status = content->add<Label>("", Font::Body, C::TextMuted);
    _status->setAlign(text::LayoutOptions::Align::Center);
    _status->style().padding(24);
    _status->setVisible(false);
    _scroll->onScroll = [this] {
        if (_scroll->scrollOffset() + _scroll->height() >= _scroll->contentExtent() - 400)
            loadMore();
    };
}

FilesPage::~FilesPage() {
    *_alive = false;
}

void FilesPage::open(model::ConvRef conv) {
    if (conv != _conv) {
        clear();
        _conv = conv;
    }
    if (_conv == model::kNoConv)
        return;
    load(1);
}

void FilesPage::clear() {
    ++_seq;
    _list->clearChildren();
    _rows.clear();
    _files.clear();
    _conv     = model::kNoConv;
    _nextPage = 0;
    _loading  = false;
    setStatus({});
}

void FilesPage::load(int page) {
    _loading = true;
    if (_files.empty())
        setStatus(tr("Loading\xE2\x80\xA6"));
    const uint64_t       seq   = ++_seq;
    std::weak_ptr<bool>  alive = _alive;
    const model::ConvRef conv  = _conv;
    _ctx.backend().loadConversationFiles(
        conv, page, [this, alive, seq, page](model::Backend::FilesPage answer) {
            if (alive.expired() || seq != _seq)
                return;
            _loading = false;
            if (!answer.error.empty()) {
                // Keep what is shown; only an empty page says it failed.
                if (_files.empty())
                    setStatus(tr("Couldn't load the files."));
                return;
            }
            if (page <= 1) { // the first page again: the list starts over
                _list->clearChildren();
                _rows.clear();
                _files.clear();
            }
            const size_t from = _files.size();
            for (model::SharedFile &f : answer.files) {
                // A file can move to an earlier page while paging (a new
                // one shared): shown once.
                bool seen = false;
                for (const model::SharedFile &g : _files)
                    seen = seen || (!f.file.id.empty() && g.file.id == f.file.id);
                if (!seen)
                    _files.push_back(std::move(f));
            }
            _nextPage = answer.nextPage;
            addRows(from);
            setStatus(
                _files.empty() ? std::string(tr("No files have been shared here yet."))
                               : std::string()
            );
            loadMoreSoon(); // a short page may not fill the view
        }
    );
}

void FilesPage::addRows(size_t from) {
    for (size_t i = from; i < _files.size(); ++i)
        _rows.push_back(_list->add<Row>(*this, i, _files[i], subline(_files[i])));
}

void FilesPage::loadMore() {
    if (_loading || _nextPage <= 0 || _conv == model::kNoConv)
        return;
    load(_nextPage);
}

void FilesPage::loadMoreSoon() {
    if (_queued || _nextPage <= 0)
        return;
    _queued                   = true;
    std::weak_ptr<bool> alive = _alive;
    _ctx.app.platform().post([this, alive] {
        if (alive.expired())
            return;
        _queued = false;
        if (visible() && !_scroll->canScroll())
            loadMore();
    });
}

void FilesPage::layout() {
    View::layout();
    if (_nextPage > 0 && !_loading && !_scroll->canScroll())
        loadMoreSoon();
}

void FilesPage::setStatus(std::string text) {
    _statusText = std::move(text);
    _status->setText(_statusText);
    _status->setVisible(!_statusText.empty());
}

std::string FilesPage::subline(const model::SharedFile &f) const {
    const model::Store      &st = _ctx.store();
    std::vector<std::string> parts;
    if (f.user != model::kNoUser && f.user < st.userCount())
        if (const std::string_view who = st.user(f.user).label(); !who.empty())
            parts.emplace_back(who);
    if (f.created > 0)
        parts.push_back(base::formatDate(f.created, _ctx.backend().nowSecs()));
    if (!f.file.prettyType.empty())
        parts.push_back(f.file.prettyType);
    else if (f.file.isCanvas())
        parts.push_back(tr("Canvas"));
    if (const std::string sz = str::byteSize(f.file.size, str::ByteSize::File); !sz.empty())
        parts.push_back(sz);
    std::string out;
    for (const std::string &p : parts)
        out += out.empty() ? p : " \xC2\xB7 " + p;
    return out;
}

std::string FilesPage::rowText(size_t i) const {
    return i < _rows.size() ? _rows[i]->text() : std::string();
}

void FilesPage::activate(size_t i) {
    if (i >= _files.size())
        return;
    const model::File &f = _files[i].file;
    if (f.isCanvas()) {
        if (_ctx.openCanvas) {
            _ctx.openCanvas(_conv, f);
            return;
        }
    }
#ifdef MSGA_HAVE_MESSAGES
    else if (f.hasPreview() && window()) {
        screens::showFileViewer(_ctx, *window(), nullptr, 0, f);
        return;
    }
#endif
    const std::string &where = f.webUrl();
    if (_ctx.openUrl && !where.empty())
        _ctx.openUrl(screens::fileUrl(where));
}

} // namespace shell
