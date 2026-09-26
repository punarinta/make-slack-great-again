// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "cc_vt.h"

#include <QChar>
#include <QRegularExpression>
#include <algorithm>

namespace claude_code {

namespace {

// Terminal cell width: 0 for combining marks and joiners, 2 for East Asian
// wide characters and emoji, 1 otherwise — close enough to wcwidth for
// finding a row's text; only a relative move after a miscounted character
// could land a column off, and Claude Code positions by absolute column.
int cellWidth(char32_t c) {
    if (c == 0x200B || c == 0x200C || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F))
        return 0;
    const auto cat = QChar::category(c);
    if (cat == QChar::Mark_NonSpacing || cat == QChar::Mark_Enclosing)
        return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF && c != 0x303F) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x1FA70 && c <= 0x1FAFF) ||
        (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

std::vector<int> parseParams(const QByteArray &s) {
    std::vector<int> out;
    int              v = -1; // -1 = left out: the default applies
    for (const char ch : s) {
        if (ch >= '0' && ch <= '9') {
            v = (v < 0 ? 0 : v) * 10 + (ch - '0');
            v = std::min(v, 99999);
        } else if (ch == ';' || ch == ':') {
            out.push_back(v);
            v = -1;
        }
    }
    out.push_back(v);
    return out;
}

} // namespace

VtScreen::VtScreen(int rows, int cols)
    : _rows(std::max(rows, 1)), _cols(std::max(cols, 1)), _cells(_rows, std::vector<Cell>(_cols)),
      _bottom(_rows - 1) {}

int VtScreen::param(const std::vector<int> &p, size_t i, int def) const {
    return i < p.size() && p[i] > 0 ? p[i] : def;
}

void VtScreen::clampCursor() {
    _cx          = std::clamp(_cx, 0, _cols - 1);
    _cy          = std::clamp(_cy, 0, _rows - 1);
    _wrapPending = false;
}

void VtScreen::eraseCells(int r, int from, int to) {
    if (r < 0 || r >= _rows)
        return;
    for (int c = std::max(from, 0); c < std::min(to, _cols); ++c)
        _cells[r][c] = Cell{};
}

void VtScreen::scrollUp(int top, int bottom, int n) {
    for (int i = 0; i < n; ++i) {
        _cells.erase(_cells.begin() + top);
        _cells.insert(_cells.begin() + bottom, std::vector<Cell>(_cols));
    }
}

void VtScreen::scrollDown(int top, int bottom, int n) {
    for (int i = 0; i < n; ++i) {
        _cells.erase(_cells.begin() + bottom);
        _cells.insert(_cells.begin() + top, std::vector<Cell>(_cols));
    }
}

void VtScreen::lineFeed() {
    _wrapPending = false;
    if (_cy == _bottom)
        scrollUp(_top, _bottom, 1);
    else if (_cy < _rows - 1)
        ++_cy;
}

void VtScreen::reverseIndex() {
    if (_cy == _top)
        scrollDown(_top, _bottom, 1);
    else if (_cy > 0)
        --_cy;
}

void VtScreen::print(char32_t c) {
    const int w = cellWidth(c);
    if (w == 0)
        return;
    if (_wrapPending) {
        _cx = 0;
        lineFeed();
    }
    if (w == 2 && _cx == _cols - 1) { // doesn't fit: wraps whole
        _cx = 0;
        lineFeed();
    }
    _cells[_cy][_cx] = Cell{c, false};
    if (w == 2)
        _cells[_cy][_cx + 1] = Cell{U' ', true};
    _cx += w;
    if (_cx >= _cols) {
        _cx          = _cols - 1;
        _wrapPending = true;
    }
}

void VtScreen::csi(char final, const QByteArray &params, char prefix) {
    const auto p = parseParams(params);
    if (prefix == '?') {
        if (final != 'h' && final != 'l')
            return;
        const bool on = final == 'h';
        for (const int mode : p) {
            if (mode == 25) {
                _cursorVisible = on;
            } else if (mode == 1049 || mode == 1047 || mode == 47) {
                // Alternate screen: a clean one to draw on, either way.
                for (int r = 0; r < _rows; ++r)
                    eraseCells(r, 0, _cols);
                if (mode == 1049 && on) {
                    _savedX = _cx;
                    _savedY = _cy;
                }
            }
        }
        return;
    }
    if (prefix != 0)
        return; // queries (>q, =c…) and the like: nothing on screen
    switch (final) {
    case 'A':
        _cy = std::max(_cy - param(p, 0, 1), 0);
        break;
    case 'B':
        _cy = std::min(_cy + param(p, 0, 1), _rows - 1);
        break;
    case 'C':
        _cx = std::min(_cx + param(p, 0, 1), _cols - 1);
        break;
    case 'D':
        _cx = std::max(_cx - param(p, 0, 1), 0);
        break;
    case 'E':
        _cy = std::min(_cy + param(p, 0, 1), _rows - 1);
        _cx = 0;
        break;
    case 'F':
        _cy = std::max(_cy - param(p, 0, 1), 0);
        _cx = 0;
        break;
    case 'G':
    case '`':
        _cx = param(p, 0, 1) - 1;
        break;
    case 'd':
        _cy = param(p, 0, 1) - 1;
        break;
    case 'H':
    case 'f':
        _cy = param(p, 0, 1) - 1;
        _cx = param(p, 1, 1) - 1;
        break;
    case 'J': {
        const int mode = std::max(p[0], 0);
        if (mode == 0) {
            eraseCells(_cy, _cx, _cols);
            for (int r = _cy + 1; r < _rows; ++r)
                eraseCells(r, 0, _cols);
        } else if (mode == 1) {
            for (int r = 0; r < _cy; ++r)
                eraseCells(r, 0, _cols);
            eraseCells(_cy, 0, _cx + 1);
        } else {
            for (int r = 0; r < _rows; ++r)
                eraseCells(r, 0, _cols);
        }
        break;
    }
    case 'K': {
        const int mode = std::max(p[0], 0);
        if (mode == 0)
            eraseCells(_cy, _cx, _cols);
        else if (mode == 1)
            eraseCells(_cy, 0, _cx + 1);
        else
            eraseCells(_cy, 0, _cols);
        break;
    }
    case 'X':
        eraseCells(_cy, _cx, _cx + param(p, 0, 1));
        break;
    case 'P': {
        auto     &line = _cells[_cy];
        const int n    = std::min(param(p, 0, 1), _cols - _cx);
        line.erase(line.begin() + _cx, line.begin() + _cx + n);
        line.insert(line.end(), n, Cell{});
        break;
    }
    case '@': {
        auto     &line = _cells[_cy];
        const int n    = std::min(param(p, 0, 1), _cols - _cx);
        line.insert(line.begin() + _cx, n, Cell{});
        line.resize(_cols);
        break;
    }
    case 'L':
        if (_cy >= _top && _cy <= _bottom)
            scrollDown(_cy, _bottom, std::min(param(p, 0, 1), _bottom - _cy + 1));
        break;
    case 'M':
        if (_cy >= _top && _cy <= _bottom)
            scrollUp(_cy, _bottom, std::min(param(p, 0, 1), _bottom - _cy + 1));
        break;
    case 'S':
        scrollUp(_top, _bottom, std::min(param(p, 0, 1), _bottom - _top + 1));
        break;
    case 'T':
        scrollDown(_top, _bottom, std::min(param(p, 0, 1), _bottom - _top + 1));
        break;
    case 'r': {
        const int top    = param(p, 0, 1) - 1;
        const int bottom = param(p, 1, _rows) - 1;
        if (top < bottom && bottom < _rows) {
            _top    = top;
            _bottom = bottom;
        } else {
            _top    = 0;
            _bottom = _rows - 1;
        }
        _cx = _cy = 0;
        break;
    }
    case 's':
        _savedX = _cx;
        _savedY = _cy;
        break;
    case 'u':
        _cx = _savedX;
        _cy = _savedY;
        break;
    default:
        break; // colours (m) and the rest: nothing to keep
    }
    clampCursor();
}

void VtScreen::feed(const QByteArray &bytes) {
    for (const char byte : bytes) {
        const auto b = static_cast<unsigned char>(byte);
        switch (_state) {
        case State::Ground:
            if (_utf8Need > 0) {
                if ((b & 0xC0) == 0x80) {
                    _utf8.append(byte);
                    if (--_utf8Need == 0) {
                        const QString s = QString::fromUtf8(_utf8);
                        for (const char32_t c : s.toUcs4())
                            print(c);
                        _utf8.clear();
                    }
                    continue;
                }
                _utf8.clear(); // broken sequence: dropped, this byte read afresh
                _utf8Need = 0;
            }
            if (b == 0x1B) {
                _state = State::Esc;
            } else if (b == '\r') {
                _cx          = 0;
                _wrapPending = false;
            } else if (b == '\n' || b == 0x0B || b == 0x0C) {
                lineFeed();
            } else if (b == '\b') {
                _cx          = std::max(_cx - 1, 0);
                _wrapPending = false;
            } else if (b == '\t') {
                _cx = std::min((_cx / 8 + 1) * 8, _cols - 1);
            } else if (b >= 0xC0 && b < 0xF8) {
                _utf8     = QByteArray(1, byte);
                _utf8Need = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
            } else if (b >= 0x20 && b < 0x7F) {
                print(b);
            }
            break;
        case State::Esc:
            _state = State::Ground;
            if (b == '[') {
                _seq.clear();
                _state = State::Csi;
            } else if (b == ']') {
                _state = State::Osc;
            } else if (b == 'P' || b == '_' || b == '^' || b == 'X') {
                _state = State::Str; // DCS / APC / PM / SOS: skipped to ST
            } else if (b == '(' || b == ')' || b == '*' || b == '+' || b == '#') {
                _state = State::EscCharset;
            } else if (b == '7') {
                _savedX = _cx;
                _savedY = _cy;
            } else if (b == '8') {
                _cx = _savedX;
                _cy = _savedY;
                clampCursor();
            } else if (b == 'D') {
                lineFeed();
            } else if (b == 'E') {
                _cx = 0;
                lineFeed();
            } else if (b == 'M') {
                reverseIndex();
            } else if (b == 'c') {
                *this = VtScreen(_rows, _cols);
            }
            break;
        case State::EscCharset:
            _state = State::Ground;
            break;
        case State::Csi:
            if (b >= 0x40 && b <= 0x7E) {
                char prefix = 0;
                if (!_seq.isEmpty() &&
                    (_seq[0] == '?' || _seq[0] == '>' || _seq[0] == '=' || _seq[0] == '<'))
                    prefix = _seq[0];
                // An intermediate byte (a space, "$"…) makes it another command.
                const bool plain = std::none_of(_seq.begin(), _seq.end(), [](char ch) {
                    return ch >= 0x20 && ch <= 0x2F;
                });
                if (plain)
                    csi(static_cast<char>(b), prefix ? _seq.mid(1) : _seq, prefix);
                _state = State::Ground;
            } else if (_seq.size() < 64) {
                _seq.append(byte);
            }
            break;
        case State::Osc:
            if (b == 0x07)
                _state = State::Ground;
            else if (b == 0x1B)
                _state = State::OscEsc;
            break;
        case State::OscEsc:
            _state = b == '\\' ? State::Ground : State::Osc;
            break;
        case State::Str:
            if (b == 0x1B)
                _state = State::StrEsc;
            break;
        case State::StrEsc:
            _state = b == '\\' ? State::Ground : State::Str;
            break;
        }
    }
}

QString VtScreen::row(int r) const {
    if (r < 0 || r >= _rows)
        return {};
    QString s;
    for (const Cell &c : _cells[r])
        if (!c.wide)
            s.append(QString::fromUcs4(&c.ch, 1));
    while (s.endsWith(QLatin1Char(' ')))
        s.chop(1);
    return s;
}

namespace {

bool isRule(const QString &row) {
    // "──────…" — the prompt box's edges; the top one can carry the session's
    // name ("──── fix the build ─").
    return row.startsWith(QStringLiteral("──"));
}

const QString kPromptMark = QStringLiteral("❯");

} // namespace

std::optional<PromptBox> findPromptBox(const VtScreen &screen) {
    int bottom = -1;
    for (int r = screen.rows() - 1; r >= 0; --r)
        if (isRule(screen.row(r))) {
            bottom = r;
            break;
        }
    int top = -1;
    for (int r = bottom - 1; r >= 0; --r)
        if (isRule(screen.row(r))) {
            top = r;
            break;
        }
    if (top < 0 || bottom - top < 2)
        return std::nullopt;
    const QString first = screen.row(top + 1);
    if (!first.startsWith(kPromptMark))
        return std::nullopt;
    PromptBox box;
    box.top = top + 1;
    for (int r = top + 1; r < bottom; ++r) {
        QString line = screen.row(r);
        if (r == top + 1)
            line = line.mid(kPromptMark.size());
        if (line.startsWith(QLatin1Char(' ')))
            line = line.mid(1);
        box.lines << line;
    }
    box.empty = std::all_of(box.lines.begin(), box.lines.end(), [](const QString &l) {
        return l.trimmed().isEmpty();
    });
    return box;
}

bool readyForInput(const VtScreen &screen) {
    const auto box = findPromptBox(screen);
    return box && box->empty && box->lines.size() == 1 && screen.cursorVisible() &&
           screen.cursorRow() == box->top;
}

std::optional<PermissionQuestion> findPermissionQuestion(const VtScreen &screen) {
    int rule = -1;
    for (int r = screen.rows() - 1; r >= 0; --r)
        if (isRule(screen.row(r))) {
            rule = r;
            break;
        }
    if (rule < 0)
        return std::nullopt;
    static const QRegularExpression kOption(QStringLiteral("^\\s*(❯)?\\s*(\\d+)\\.\\s+(\\S.*)$"));
    PermissionQuestion              q;
    QStringList                     text;
    for (int r = rule + 1; r < screen.rows(); ++r) {
        const QString row = screen.row(r);
        const auto    m   = kOption.match(row);
        if (!m.hasMatch()) {
            if (!q.options.empty())
                break; // past the options: "Esc to cancel · Tab to amend"
            if (!row.trimmed().isEmpty())
                text << row.trimmed();
            continue;
        }
        const int number = m.captured(2).toInt();
        if (number != int(q.options.size()) + 1)
            return std::nullopt; // a numbered list, but not one of options
        q.options.push_back({number, m.captured(3).trimmed()});
        if (m.capturedLength(1) > 0) {
            if (q.selected)
                return std::nullopt;
            q.selected = number;
        }
    }
    if (q.options.size() < 2 || !q.selected)
        return std::nullopt;
    q.text = text.join(QLatin1Char(' '));
    return q;
}

} // namespace claude_code
