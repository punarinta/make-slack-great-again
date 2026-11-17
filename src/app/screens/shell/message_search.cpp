#include "screens/shell/message_search.h"

#include "app/screens/common/message_text.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"
#include "base/utf8.h"
#include "gfx/icons_generated.h"
#include "screens/shell/nav_chrome.h"

#include <algorithm>
#include <cmath>

using namespace ui;
using gfx::Icon;
using i18n::tr;
using model::ConvRef;
using model::Ts;

namespace shell {

namespace {

constexpr double kFadeMs       = 350; // the dimming's and the card's fades
constexpr float  kOverlayAlpha = 70;  // surface.overlay
constexpr size_t kPreviewChars = 120;

// The search icon: the 16 px glyph in a 20 px box, a hover tooltip.
class SearchIcon final : public View {
public:
    SearchIcon() { style().size(20, 20).noShrink(); }
    std::string tooltip() const override { return tr("Search messages"); }
    void        paint(gfx::Painter &p) override {
        gfx::drawIcon(p, Icon::Search, {snapPx(2), snapPx(2), 16, 16}, color(C::FormIcon));
    }
};

// The search header: surface.raised under a divider.def bottom rule.
class SearchHeader final : public View {
public:
    void paint(gfx::Painter &p) override {
        p.fillRect(bounds(), color(C::FormBg));
        p.fillRect({0, height() - 1, width(), 1}, color(C::FormDivider));
    }
};

// A result row: 8/12 padding, a divider.subtle
// rule under it, surface.highlight on hover, surface.highlightStrong when
// selected from the keyboard.
class ResultRow final : public Clickable {
public:
    ResultRow() {
        setLook({C::None, C::FormHighlight, C::FormHighlight, C::FormHighlightStrong, 0});
        style().padding(12, 8);
    }
    void paint(gfx::Painter &p) override {
        Clickable::paint(p);
        p.fillRect({0, height() - 1, width(), 1}, color(C::FormHighlight));
    }
};

class StatusRow final : public View {
public:
    explicit StatusRow(std::string text) {
        style().padding(12, 8);
        add<Label>(std::move(text), Font::Body, C::FormTextFaint)->setMaxLines(1);
    }
    void paint(gfx::Painter &p) override {
        p.fillRect({0, height() - 1, width(), 1}, color(C::FormHighlight));
    }
};

} // namespace

// The panel: header and results fade in as a unit,
// over the dimming that MessageSearch paints.
class MessageSearch::Card final : public View {
public:
    float opacity = 0;
    void  paint(gfx::Painter &p) override { p.setOpacity(opacity); } // the children too
};

std::string searchConvLabel(const model::Store &store, ConvRef conv) {
    const std::string name = conv < store.conversationCount() ? store.displayName(conv) : "";
    if (name.empty())
        return tr("Unknown channel");
    return store.conversation(conv).isDirect() ? name : str::concat({"#", name});
}

std::string searchPreview(const model::Store &store, std::string_view text) {
    // The rendered text with mentions resolved (the
    // parser's label is kept for anyone unknown).
    std::string out = screens::plainText(store, text);
    // The first 120 characters, newlines as spaces.
    out.resize(utf8::prefixBytes(out, kPreviewChars));
    std::replace(out.begin(), out.end(), '\n', ' ');
    return out;
}

MessageSearch::MessageSearch(screens::Context &ctx) : _ctx(ctx) {
    setVisible(false);
    _card        = add<Card>();
    // Header row: search icon + input + close button (sp.lg, sp.md margins).
    auto *header = _card->add<SearchHeader>();
    header->style().row().padding(12, 8, 8, 8).spacing(8).items(Align::Center).noShrink();
    header->add<SearchIcon>();
    _field = header->add<TextEdit>();
    _field->style().flex(1).padding(0, 4);
    _field->setMaxLines(1);
    _field->setFont(Font::Field);
    _field->setPlaceholder(tr("Search messages\xE2\x80\xA6"));
    _field->onKey     = [this](const Event &e) { return e.type == EventType::KeyDown && key(e); };
    auto *closeBtn    = header->add<GlyphButton>(Icon::X, 24, 14, C::FormIcon, tr("Close search"));
    closeBtn->onClick = [this] { close(); };
    _list             = _card->add<ScrollView>();
    _list->setBackground(C::FormBg);
    _list->setVisible(false); // shown only once a search is run
    // The dimmed area below the results card.
    add<View>()->style().flex(1);
    // A late-resolved user can now be named in a result's preview.
    _observer = _ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
        if (ch.kind == model::ChangeKind::Users && !_results.empty() && visible())
            renameSoon();
    });
}

MessageSearch::~MessageSearch() {
    _ctx.store.unobserve(_observer);
}

void MessageSearch::toggle() {
    if (visible())
        hideNow();
    else
        show();
}

void MessageSearch::show() {
    setVisible(true);
    animateTo(true);
    _field->focus();
    _field->selectAll();
}

void MessageSearch::hideNow() {
    if (!visible())
        return;
    _start   = -1;
    _closing = false;
    _alpha = _opacity = _card->opacity = 0;
    finishHide();
}

void MessageSearch::close() {
    if (shown())
        animateTo(false);
}

void MessageSearch::finishHide() {
    const bool hadFocus = window() && isAncestorOf(window()->focusView());
    setVisible(false);
    if (hadFocus && onHidden)
        onHidden();
}

void MessageSearch::animateTo(bool open) {
    _closing     = !open;
    _alphaFrom   = _alpha;
    _opacityFrom = _opacity;
    _start       = -1;
    startTicking();
}

bool MessageSearch::tick(double nowMs) {
    if (!visible())
        return false;
    if (_start < 0)
        _start = nowMs;
    const double t       = std::min(1.0, (nowMs - _start) / kFadeMs);
    const float  e       = _closing ? float(t * t * t)                        // InCubic
                                    : float(1 - (1 - t) * (1 - t) * (1 - t)); // OutCubic
    const float  alphaTo = _closing ? 0 : kOverlayAlpha, opacityTo = _closing ? 0 : 1;
    _alpha         = _alphaFrom + (alphaTo - _alphaFrom) * e;
    _opacity       = _opacityFrom + (opacityTo - _opacityFrom) * e;
    _card->opacity = _opacity;
    update();
    if (t < 1)
        return true;
    if (_closing) {
        _closing = false;
        finishHide();
    }
    return false;
}

void MessageSearch::reset() {
    hideNow();
    _field->clear();
    _results.clear();
    _rows.clear();
    _heads.clear();
    _previews.clear();
    _list->content()->clearChildren();
    _list->setVisible(false);
    _statusText.clear();
    _sel = -1;
    ++_generation; // a search still running belongs to the old workspace
}

void MessageSearch::paint(gfx::Painter &p) {
    p.fillRect(bounds(), gfx::Color(uint32_t(std::lround(_alpha)) << 24));
}

bool MessageSearch::onEvent(Event &e) {
    // The overlay covers the message area: nothing under it is reachable.
    return e.type == EventType::PointerDown || e.type == EventType::Scroll;
}

bool MessageSearch::key(const Event &e) {
    switch (e.key) {
    case plat::Key::Escape:
        close();
        return true;
    case plat::Key::Up:
        navigateBy(-1);
        return true;
    case plat::Key::Down:
        navigateBy(1);
        return true;
    case plat::Key::Enter:
    case plat::Key::KpEnter:
        if (_sel >= 0)
            activate(_sel);
        else
            runSearch(std::string(str::trim(_field->text())));
        return true;
    default:
        return false;
    }
}

void MessageSearch::runSearch(const std::string &query) {
    if (query.empty())
        return;
    _results.clear();
    _sel = -1;
    _list->setVisible(true);
    _rows.clear();
    _heads.clear();
    _previews.clear();
    _list->content()->clearChildren();
    addStatus(tr("Searching\xE2\x80\xA6"));
    const uint32_t     gen   = ++_generation;
    std::weak_ptr<int> alive = _alive;
    _ctx.backend().search(query, [this, alive, gen](std::vector<model::Backend::SearchHit> hits) {
        if (alive.expired() || gen != _generation)
            return;
        _results = std::move(hits);
        populate();
    });
}

void MessageSearch::addStatus(std::string text) {
    _statusText = text;
    _list->content()->add<StatusRow>(std::move(text));
    _card->invalidateLayout(); // the list is a layout boundary: the card follows it
}

std::string MessageSearch::headText(const model::Backend::SearchHit &r) const {
    return str::concat(
        {searchConvLabel(_ctx.store, r.conv), "  ", base::formatDateTime(model::tsSecs(r.ts))}
    );
}

void MessageSearch::populate() {
    _rows.clear();
    _heads.clear();
    _previews.clear();
    _list->content()->clearChildren();
    _sel = -1;
    _statusText.clear();
    if (_results.empty()) {
        addStatus(tr("No results found."));
        return;
    }
    const model::Store &store = _ctx.store;
    for (size_t i = 0; i < _results.size(); ++i) {
        const model::Backend::SearchHit &r   = _results[i];
        auto                            *row = _list->content()->add<ResultRow>();
        _heads.push_back(row->add<Label>(headText(r), Font::Body, C::FormText));
        _heads.back()->setMaxLines(1);
        _previews.push_back(row->add<Label>(searchPreview(store, r.text), Font::Body, C::FormText));
        _previews.back()->setMaxLines(1);
        row->setTooltip(mrkdwn::parse(r.text).text);
        row->onClick = [this, i] { activate(int(i)); };
        _rows.push_back(row);
    }
    _card->invalidateLayout();
}

void MessageSearch::renameSoon() {
    if (_renameQueued)
        return;
    _renameQueued            = true;
    std::weak_ptr<int> alive = _alive;
    _ctx.app.platform().post([this, alive] {
        if (alive.expired())
            return;
        _renameQueued = false;
        rename();
    });
}

void MessageSearch::rename() {
    const model::Store &store = _ctx.store;
    for (size_t i = 0; i < _rows.size() && i < _results.size(); ++i) {
        _heads[i]->setText(headText(_results[i]));
        _previews[i]->setText(searchPreview(store, _results[i].text));
    }
}

void MessageSearch::navigateBy(int delta) {
    if (_rows.empty())
        return;
    const int n = int(_rows.size());
    _sel        = _sel < 0 ? (delta > 0 ? 0 : n - 1) : std::clamp(_sel + delta, 0, n - 1);
    for (int i = 0; i < n; ++i)
        _rows[size_t(i)]->setChecked(i == _sel);
    _list->ensureVisible(_rows[size_t(_sel)]);
}

void MessageSearch::activate(int index) {
    if (index < 0 || size_t(index) >= _results.size() || size_t(index) >= _rows.size())
        return;
    const model::Backend::SearchHit hit = _results[size_t(index)];
    if (onResult)
        onResult(hit.conv, hit.ts);
    close();
}

} // namespace shell
