// The canvas tab: a big borderless
// title line over the body, autosaved a few seconds after typing stops, in a
// column as wide as Slack's (1040 px); a floating "⋮" with "Copy link" and
// "Delete canvas" once the canvas exists. A conversation without one opens a
// blank page: the first save creates the canvas, then the tab names it.
//
// The asymmetry is Slack's: a canvas reads as the HTML its file serves and
// saves as canvas markdown. The body is a TextEdit holding the markdown's
// block syntax as text ("## ", "- ", "1. ", "> ", "| a | b |") with bold,
// italic, strike, code and links as formats. While it isn't being edited it
// shows as a formatted reading view built from that text (headings, lists,
// checklists, quotes, code, tables as a grid); a click there edits the text
// at that point, and leaving the editor (Escape, a click elsewhere) shows the
// view again. Saves are a section diff (screens/common/canvas_doc.h):
// only the sections that changed are written, the whole document when that
// can't be done safely. There is no co-editing.
#pragma once

#include "app/screens/common/canvas_doc.h"
#include "screens/common/context.h"
#include "ui/ui.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace shell {

class CanvasPage : public ui::View {
public:
    explicit CanvasPage(screens::Context &ctx);
    ~CanvasPage() override;

    // Shows conv's canvas (Conversation::canvasId; none: a blank page that
    // creates it on the first save). The same canvas again keeps the editor,
    // refetching only when nothing is unsaved.
    void open(model::ConvRef conv);
    // A canvas file shared in conv (the canvas viewer): the same page on
    // that file. title: the file's (the title line until the meta answers).
    void openFile(model::ConvRef conv, const std::string &fileId, const std::string &title);
    // After "Delete canvas" succeeded (the viewer closes itself).
    std::function<void()> onDeleted;
    // Saves pending edits now (tab or conversation switch, sign-out).
    void                  flushPendingSave();
    void                  clear();

    // The title as typed, for the tab (only while the canvas exists).
    std::function<void(const std::string &title)> onTitleChanged;
    std::function<void(const std::string &error)> onError;

    ui::TextEdit &title() const { return *_title; }
    ui::TextEdit &body() const { return *_body; }
    // The formatted view of the body shown while it isn't being edited.
    ui::View     &reader() const { return *_reader; }
    bool          editing() const { return _editing; }
    // Into the editor, the caret at `at` (a byte offset into body().text()),
    // as a click on the reading view does; out of it, as Escape does.
    void          edit(uint32_t at);
    void          stopEditing();
    ui::View     *menuButton() const { return _menuBtn; }
    ui::Label    *notice() const { return _notice; }
    void          showMenu();
    void          confirmDelete(); // the menu's "Delete canvas"

private:
    enum class ReadOnly : uint8_t { None, NotAddressable, NoAccess };
    void loadContent();
    void applyRemoteHtml(const std::string &html);
    void setBody(const std::string &editorHtml);
    void setReadOnly(ReadOnly cause);
    void armSave();
    void dropTimer();
    // Reading view or editor, from _editing, the body and read-only state;
    // the reading view rebuilt from the body when it shows.
    void refreshMode();
    void rebuildReader();

    screens::Context        &_ctx;
    ui::ScrollView          *_scroll  = nullptr;
    ui::Label               *_notice  = nullptr;
    ui::TextEdit            *_title   = nullptr;
    ui::TextEdit            *_body    = nullptr;
    ui::View                *_reader  = nullptr;
    bool                     _editing = false;
    std::vector<std::string> _readerLinks; // the reading view's link ids - 1
    ui::View                *_menuBtn = nullptr;
    model::ConvRef           _conv    = model::kNoConv;
    std::string              _fileId;        // "" = not created yet
    std::string              _permalink;     // "Copy link"; "" until the meta answered
    std::string              _lastHtml;      // the server's last answer (unchanged: no reset)
    std::string              _serverTitle;   // the file's title: names the title h1
    uint64_t                 _seq       = 0; // invalidates answers for an earlier open
    uint32_t                 _saveTimer = 0; // plat::TimerId
    ReadOnly                 _ro        = ReadOnly::None;
    bool                     _loading = false, _refetching = false, _saving = false;
    bool                     _bodyDirty = false, _titleDirty = false;
    std::shared_ptr<bool>    _alive;

    // baseChunks of _lastHtml as last cut, keyed by a hash of it and the titles.
    std::vector<screens::canvas::Chunk> _base;
    uint64_t                            _baseKey = 0;
    bool                                _baseOk  = false;
};

// The body's conversion back (canvas_page.cpp), exposed for tests; the
// other way is screens::canvas::editorHtml.
namespace canvas {
// The body back to canvas markdown: block prefixes as typed, formats as
// **b** _i_ ~~s~~ `c` [t](u), a blank line between paragraphs.
std::string markdown(const ui::TextEdit &body);
} // namespace canvas

} // namespace shell
