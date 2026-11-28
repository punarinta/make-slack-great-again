// FileBrowser — the in-app file chooser, for systems without a native one
// (Linux with no FileChooser portal: plain X11 sessions, a static binary
// outside a portal-aware desktop). It takes plat's FileDialogDesc as is and
// answers like plat::App::showFileDialog: the chosen absolute paths, or none
// when cancelled. Linux only: Windows and macOS always have the OS dialog.
//
// A modal card over a dimmed window: a path bar (click a folder to go back
// up), the folder's entries (folders first; hidden files off, Ctrl+H or the
// eye button shows them), a name field in Save mode, the filter chooser,
// Cancel / OK. Keyboard: Up/Down/Home/End/PageUp/PageDown move, Enter opens
// a folder or accepts, Backspace or Alt+Up goes to the parent, a letter jumps
// to the next entry starting with it, Escape cancels; OpenMultiple adds
// Ctrl/Shift+click, Shift+arrows and Ctrl+A. Everything is drawn from theme
// tokens on the card's own background, so it reads in light and dark alike
// (issue #18: a fallback dialog that inherited a foreign palette was
// unreadable).
#pragma once

#include "base/file.h"
#include "ui/scroll.h"
#include "ui/widgets.h"

#include <functional>
#include <string>
#include <vector>

namespace ui {

class TextEdit;

class FileBrowser : public Popup {
public:
    using Done = std::function<void(std::vector<std::string> paths)>; // empty = cancelled

    FileBrowser(const plat::FileDialogDesc &d, Done done);
    ~FileBrowser() override; // answers "cancelled" if nothing was chosen
    // Shows it over w and focuses the list (Save: the name field).
    static FileBrowser *show(Window &w, const plat::FileDialogDesc &d, Done done);

    // ── State (keyboard handling and tests go through these) ─────────────────
    const std::string    &dir() const { return _dir; }
    // Lists `dir`; false (and an error line, the old listing kept) when it
    // cannot be read.
    bool                  setDir(std::string dir);
    size_t                entryCount() const { return _entries.size(); }
    const file::DirEntry &entry(size_t i) const { return _entries[i]; }
    int                   find(std::string_view name) const; // -1 if not listed
    int                   current() const { return _current; }
    bool                  selected(size_t i) const { return i < _sel.size() && _sel[i]; }
    // Click semantics: plain = select only i; toggle = Ctrl; range = Shift.
    void                  select(int i, bool toggle = false, bool range = false);
    // Double click / Enter on i: a folder opens, a file accepts.
    void                  activate(int i);
    void                  goUp();
    void                  setShowHidden(bool on);
    bool                  showHidden() const { return _hidden; }
    void                  setFilter(size_t i);
    size_t                filter() const { return _filter; }
    TextEdit             *nameField() const { return _name; }
    // OK: returns false when nothing can be chosen yet (no selection, empty
    // name) or it asked something first (Save over an existing file).
    bool                  accept();
    void                  cancel();

    SizeF measureContent(float availW, float availH) override;
    void  paint(gfx::Painter &p) override;
    bool  onEvent(Event &e) override;

    // Entry names matching a filter pattern ("*.png", "report-??.txt"),
    // ASCII case-insensitive. Public for tests.
    static bool globMatch(std::string_view pattern, std::string_view name);

private:
    class List;
    friend class List;

    void        relist();
    void        rebuildPath();
    void        refreshOk();
    void        setStatus(std::string s, bool error);
    void        finish(std::vector<std::string> paths);
    bool        keyDown(const Event &e);
    std::string pathOf(size_t i) const;

    plat::FileDialogDesc        _desc;
    Done                        _done;
    std::string                 _dir;
    std::vector<file::DirEntry> _all, _entries; // the folder; what is shown
    std::vector<uint8_t>        _sel;
    std::string                 _confirmName; // Save: overwrite asked for this name
    View                       *_card = nullptr, *_path = nullptr;
    List                       *_list   = nullptr;
    TextEdit                   *_name   = nullptr;
    Label                      *_status = nullptr;
    Button                     *_ok = nullptr, *_filterBtn = nullptr, *_eye = nullptr;
    size_t                      _filter  = 0;
    int                         _current = -1, _anchor = -1;
    bool                        _hidden = false, _answered = false;
};

} // namespace ui
