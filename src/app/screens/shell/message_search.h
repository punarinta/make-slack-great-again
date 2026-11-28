// Ctrl/Cmd+F and the header's search button: an overlay over the message area (list, typing line,
// composer) that dims it and puts a card at its top: the search icon, the
// "Search messages…" field and a close button, then the results once a
// search ran. Enter searches (Backend::search); ↑ / ↓ move through the
// results, Enter or a click opens one (onResult) and closes; Esc and the
// close button fade it out.
#pragma once

#include "screens/common/context.h"
#include "ui/ui.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shell {

// A result's two lines: "#channel" (a DM's name bare, "Unknown channel"
// without one), and the message text, mentions named, its first 120
// characters on one line.
std::string searchConvLabel(const model::Store &store, model::ConvRef conv);
std::string searchPreview(const model::Store &store, std::string_view mrkdwn);

class MessageSearch : public ui::View {
public:
    explicit MessageSearch(screens::Context &ctx);
    ~MessageSearch() override;

    // Shown → hidden at once, hidden → shown (fading in,
    // the field focused with its text selected).
    void toggle();
    void show();
    void hideNow();
    void close(); // fades out, then hides (Esc, the close button)
    // A workspace switch: the query and results go.
    void reset();
    bool shown() const { return visible() && !_closing; }

    // A result picked: its conversation and message. The search closes.
    std::function<void(model::ConvRef, model::Ts ts)> onResult;
    // The search hid (closed or toggled off) while its field had the focus.
    std::function<void()>                             onHidden;

    void runSearch(const std::string &query);
    void navigateBy(int delta);
    void activate(int index);

    ui::TextEdit                                 &field() const { return *_field; }
    const std::vector<model::Backend::SearchHit> &results() const { return _results; }
    int                                           selected() const { return _sel; }
    // What the list shows: the results' rows, or the one status row
    // ("Searching…", "No results found."); hidden before the first search.
    ui::View                                     *list() const { return _list; }
    std::string                                   statusText() const { return _statusText; }

    void paint(gfx::Painter &p) override;
    bool onEvent(ui::Event &e) override;
    bool tick(double nowMs) override;

private:
    class Card;
    void        populate();
    // The rows' texts again, in place (a user resolved: a mention's name);
    // the keyboard selection stays. Coalesced: once per burst of changes.
    void        renameSoon();
    void        rename();
    std::string headText(const model::Backend::SearchHit &r) const;
    void        addStatus(std::string text);
    bool        key(const ui::Event &e);
    void        animateTo(bool open);
    void        finishHide();

    screens::Context                      &_ctx;
    Card                                  *_card  = nullptr;
    ui::TextEdit                          *_field = nullptr;
    ui::ScrollView                        *_list  = nullptr;
    std::vector<ui::Clickable *>           _rows;
    std::vector<ui::Label *>               _heads, _previews; // per row: its two lines
    std::vector<model::Backend::SearchHit> _results;
    std::string                            _statusText;
    int                                    _sel        = -1;
    uint32_t                               _generation = 0; // the search a reply belongs to
    model::Store::ObserverId               _observer   = 0;
    std::shared_ptr<int>                   _alive      = std::make_shared<int>(0);
    // The fades (350 ms, OutCubic in, InCubic out): the dimming's
    // alpha and the card's opacity, from where they are towards the target.
    float                                  _alpha = 0, _opacity = 0;
    float                                  _alphaFrom = 0, _opacityFrom = 0;
    double                                 _start        = -1;
    bool                                   _closing      = false;
    bool                                   _renameQueued = false;
};

} // namespace shell
