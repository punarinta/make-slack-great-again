// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Just enough of a terminal to read Claude Code's own screen: a background
// session's terminal UI, relayed by `claude attach` (see cc_attach). It keeps
// the text on each row and where the cursor is — no colours, no scrollback.
//
// Claude Code 2.1.282 was seen using only: SGR colours (m), cursor moves
// (A B C D G H), erase (J K), private modes (?h / ?l: cursor visibility,
// alternate screen, mouse, focus, bracketed paste, synchronized output),
// window-title OSCs, charset selection (ESC ( B) and two queries (c, >q) it
// does fine without answers to. The usual rest (scroll regions, insert/delete,
// save/restore cursor) is here too so a newer version doesn't throw it off.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

namespace claude_code {

class VtScreen {
public:
    VtScreen(int rows, int cols);

    void feed(const QByteArray &bytes);

    int     rows() const { return _rows; }
    int     cols() const { return _cols; }
    // Row `r`'s text, trailing blanks dropped. A wide character's second cell
    // isn't in it.
    QString row(int r) const;
    int     cursorRow() const { return _cy; }
    int     cursorCol() const { return _cx; }
    bool    cursorVisible() const { return _cursorVisible; }

private:
    struct Cell {
        char32_t ch   = U' ';
        bool     wide = false; // the right half of a wide character before it
    };
    void print(char32_t c);
    void lineFeed();
    void reverseIndex();
    void scrollUp(int top, int bottom, int n);
    void scrollDown(int top, int bottom, int n);
    void eraseCells(int r, int from, int to);
    void csi(char final, const QByteArray &params, char prefix);
    void clampCursor();
    int  param(const std::vector<int> &p, size_t i, int def) const;

    enum class State { Ground, Esc, EscCharset, Csi, Osc, OscEsc, Str, StrEsc };

    int                            _rows, _cols;
    std::vector<std::vector<Cell>> _cells;
    int                            _cx = 0, _cy = 0;
    int                            _savedX = 0, _savedY = 0;
    int                            _top = 0, _bottom = 0; // scroll region, inclusive
    bool                           _cursorVisible = true;
    bool                           _wrapPending   = false;
    State                          _state         = State::Ground;
    QByteArray                     _seq;  // a CSI's parameters so far
    QByteArray                     _utf8; // an unfinished UTF-8 sequence
    int                            _utf8Need = 0;
};

// Claude Code's prompt box, as the screen shows it: the rows between the last
// two horizontal rules, the first starting "❯". Present only when the prompt
// has the keyboard — a permission question or a panel (/cost, /model…) takes
// the box's place.
struct PromptBox {
    int         top = 0; // the first row inside the rules
    QStringList lines;   // the input, "❯ " and indentation taken off
    bool        empty = true;
};
std::optional<PromptBox> findPromptBox(const VtScreen &screen);

// Whether Claude Code is waiting for the user's typing: the prompt box is
// shown, empty, with the cursor in it — so text sent now goes into the prompt
// and nowhere else (not into a permission question, which a stray Enter
// would answer, and not onto a draft someone left in it).
bool readyForInput(const VtScreen &screen);

// A permission question in the prompt box's place, as Claude Code 2.1.283
// draws it below its last horizontal rule:
//
//    Bash command
//      │ rm -rf build
//    Do you want to proceed?
//    ❯ 1. Yes
//      2. Yes, and don't ask again for rm commands in /src
//      3. No
//
// The options are the numbered rows, "❯" marking the one Enter would pick.
struct PermissionQuestion {
    struct Option {
        int     number = 0;
        QString label;
    };
    QString             text;         // the rows above the options, joined by spaces
    std::vector<Option> options;      // numbered 1, 2, 3… in order
    int                 selected = 0; // the number "❯" is on
};
std::optional<PermissionQuestion> findPermissionQuestion(const VtScreen &screen);

} // namespace claude_code
