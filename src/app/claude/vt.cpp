#include "app/claude/vt.h"

#include "base/str.h"
#include "base/utf8.h"

#include <algorithm>
#include <iterator>

namespace claude {

namespace {

// Combining marks (Unicode Mn / Me) in the scripts a terminal session shows:
// they take no cell of their own. Not the whole category — the Indic and
// South-East Asian blocks are only partly here — which costs at most a
// column on a row with an unlisted mark (Claude Code positions by absolute
// column, so the next move puts it right).
struct Range {
    char32_t from, to;
};
constexpr Range kMarks[] = {
    {0x0300, 0x036F},   {0x0483, 0x0489},   {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2},
    {0x05C4, 0x05C5},   {0x05C7, 0x05C7},   {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670},
    {0x06D6, 0x06DC},   {0x06DF, 0x06E4},   {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711},
    {0x0730, 0x074A},   {0x07A6, 0x07B0},   {0x07EB, 0x07F3}, {0x0816, 0x082D}, {0x0859, 0x085B},
    {0x08D3, 0x08E1},   {0x08E3, 0x0902},   {0x093A, 0x093A}, {0x093C, 0x093C}, {0x0941, 0x0948},
    {0x094D, 0x094D},   {0x0951, 0x0957},   {0x0962, 0x0963}, {0x0981, 0x0981}, {0x09BC, 0x09BC},
    {0x09C1, 0x09C4},   {0x09CD, 0x09CD},   {0x0A01, 0x0A02}, {0x0A3C, 0x0A3C}, {0x0A41, 0x0A51},
    {0x0A70, 0x0A71},   {0x0A81, 0x0A82},   {0x0ABC, 0x0ABC}, {0x0AC1, 0x0AC8}, {0x0ACD, 0x0ACD},
    {0x0B01, 0x0B01},   {0x0B3C, 0x0B3C},   {0x0B3F, 0x0B3F}, {0x0B41, 0x0B44}, {0x0B4D, 0x0B4D},
    {0x0BC0, 0x0BC0},   {0x0BCD, 0x0BCD},   {0x0C3E, 0x0C40}, {0x0C46, 0x0C56}, {0x0CBC, 0x0CBC},
    {0x0CCC, 0x0CCD},   {0x0D41, 0x0D44},   {0x0D4D, 0x0D4D}, {0x0DCA, 0x0DCA}, {0x0DD2, 0x0DD6},
    {0x0E31, 0x0E31},   {0x0E34, 0x0E3A},   {0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC},
    {0x0EC8, 0x0ECD},   {0x0F18, 0x0F19},   {0x0F35, 0x0F39}, {0x0F71, 0x0F84}, {0x102D, 0x1030},
    {0x1032, 0x1037},   {0x1039, 0x103A},   {0x135D, 0x135F}, {0x17B4, 0x17B5}, {0x17B7, 0x17BD},
    {0x17C6, 0x17D3},   {0x180B, 0x180D},   {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20F0},
    {0x2CEF, 0x2CF1},   {0x2DE0, 0x2DFF},   {0x302A, 0x302D}, {0x3099, 0x309A}, {0xA66F, 0xA672},
    {0xA674, 0xA67D},   {0xA69E, 0xA69F},   {0xFB1E, 0xFB1E}, {0xFE20, 0xFE2F}, {0x1D167, 0x1D169},
    {0x1D17B, 0x1D182}, {0xE0100, 0xE01EF},
};

bool isMark(char32_t c) {
    for (const Range &r : kMarks)
        if (c >= r.from && c <= r.to)
            return true;
    return false;
}

// Terminal cell width: 0 for combining marks and joiners, 2 for East Asian
// wide characters and emoji, 1 otherwise — close enough to wcwidth for
// finding a row's text; only a relative move after a miscounted character
// could land a column off, and Claude Code positions by absolute column.
int cellWidth(char32_t c) {
    if (c == 0x200B || c == 0x200C || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F))
        return 0;
    if (c >= 0x0300 && isMark(c))
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

// A CSI's parameters: a fixed few (a sequence is at most 64 bytes, see
// VtScreen::feed), so none is ever allocated for.
struct Params {
    int        v[32];
    int        n = 0;
    int        operator[](int i) const { return v[i]; }
    const int *begin() const { return v; }
    const int *end() const { return v + n; }
};

Params parseParams(std::string_view s) {
    Params out;
    int    v = -1; // -1 = left out: the default applies
    for (const char ch : s) {
        if (ch >= '0' && ch <= '9') {
            v = (v < 0 ? 0 : v) * 10 + (ch - '0');
            v = std::min(v, 99999);
        } else if (ch == ';' || ch == ':') {
            if (out.n < int(std::size(out.v)))
                out.v[out.n++] = v;
            v = -1;
        }
    }
    if (out.n < int(std::size(out.v)))
        out.v[out.n++] = v;
    return out;
}

int param(const Params &p, int i, int def) {
    return i < p.n && p[i] > 0 ? p[i] : def;
}

} // namespace

VtScreen::VtScreen(int rows, int cols)
    : _rows(std::max(rows, 1)), _cols(std::max(cols, 1)), _cells(_rows, std::vector<Cell>(_cols)),
      _bottom(_rows - 1) {}

void VtScreen::reset() {
    _cells.assign(size_t(_rows), std::vector<Cell>(size_t(_cols)));
    _cx = _cy = _savedX = _savedY = 0;
    _top                          = 0;
    _bottom                       = _rows - 1;
    _cursorVisible                = true;
    _wrapPending                  = false;
    _state                        = State::Ground;
    _seq.clear();
    _utf8.clear();
    _utf8Need = 0;
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

// The rows themselves move (their storage with them) and those scrolled off
// come back blank at the other end: nothing is allocated.
void VtScreen::scrollUp(int top, int bottom, int n) {
    n = std::min(n, bottom - top + 1);
    if (n <= 0)
        return;
    const auto first = _cells.begin() + top, last = _cells.begin() + bottom + 1;
    std::rotate(first, first + n, last);
    for (auto row = last - n; row != last; ++row)
        std::fill(row->begin(), row->end(), Cell{});
}

void VtScreen::scrollDown(int top, int bottom, int n) {
    n = std::min(n, bottom - top + 1);
    if (n <= 0)
        return;
    const auto first = _cells.begin() + top, last = _cells.begin() + bottom + 1;
    std::rotate(first, last - n, last);
    for (auto row = first; row != first + n; ++row)
        std::fill(row->begin(), row->end(), Cell{});
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

void VtScreen::csi(char final, std::string_view params, char prefix) {
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
        line.insert(line.end(), size_t(n), Cell{});
        break;
    }
    case '@': {
        auto     &line = _cells[_cy];
        const int n    = std::min(param(p, 0, 1), _cols - _cx);
        line.insert(line.begin() + _cx, size_t(n), Cell{});
        line.resize(size_t(_cols));
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

void VtScreen::feed(std::string_view bytes) {
    for (const char byte : bytes) {
        const auto b = static_cast<unsigned char>(byte);
        switch (_state) {
        case State::Ground:
            if (_utf8Need > 0) {
                if ((b & 0xC0) == 0x80) {
                    _utf8.push_back(byte);
                    if (--_utf8Need == 0) {
                        for (size_t i = 0; i < _utf8.size();)
                            print(utf8::decode(_utf8, i));
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
                _utf8.assign(1, byte);
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
                reset();
            }
            break;
        case State::EscCharset:
            _state = State::Ground;
            break;
        case State::Csi:
            if (b >= 0x40 && b <= 0x7E) {
                char prefix = 0;
                if (!_seq.empty() &&
                    (_seq[0] == '?' || _seq[0] == '>' || _seq[0] == '=' || _seq[0] == '<'))
                    prefix = _seq[0];
                // An intermediate byte (a space, "$"…) makes it another command.
                const bool plain = std::none_of(_seq.begin(), _seq.end(), [](char ch) {
                    return ch >= 0x20 && ch <= 0x2F;
                });
                if (plain)
                    csi(static_cast<char>(b),
                        prefix ? std::string_view(_seq).substr(1) : std::string_view(_seq),
                        prefix);
                _state = State::Ground;
            } else if (_seq.size() < 64) {
                _seq.push_back(byte);
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

std::string VtScreen::row(int r) const {
    if (r < 0 || r >= _rows)
        return {};
    std::string s;
    for (const Cell &c : _cells[size_t(r)])
        if (!c.wide)
            utf8::append(s, uint32_t(c.ch));
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

namespace {

bool isRule(std::string_view row) {
    // "──────…" — the prompt box's edges; the top one can carry the session's
    // name ("──── fix the build ─").
    return str::startsWith(row, "──");
}

constexpr std::string_view kPromptMark = "❯";

int lastRule(const VtScreen &screen, int below) {
    for (int r = below - 1; r >= 0; --r)
        if (isRule(screen.row(r)))
            return r;
    return -1;
}

// "  ❯ 2. Yes, and don't ask again" → the option, as the pattern
// ^\s*(❯)?\s*(\d+)\.\s+(\S.*)$ would read it.
bool parseOption(std::string_view row, int *number, std::string *label, bool *marked) {
    std::string_view s = str::trimSpace(row);
    *marked            = str::startsWith(s, kPromptMark);
    if (*marked)
        s = str::trimSpace(s.substr(kPromptMark.size()));
    size_t digits = 0;
    while (digits < s.size() && s[digits] >= '0' && s[digits] <= '9')
        ++digits;
    if (digits == 0 || digits > 6 || digits >= s.size() || s[digits] != '.')
        return false;
    const std::string_view rest = s.substr(digits + 1);
    if (rest.empty() || str::trimSpace(rest).size() == rest.size())
        return false; // no blank after the dot
    const std::string_view text = str::trimSpace(rest);
    if (text.empty())
        return false;
    int n = 0;
    for (size_t i = 0; i < digits; ++i)
        n = n * 10 + (s[i] - '0');
    *number = n;
    label->assign(text);
    return true;
}

} // namespace

std::optional<PromptBox> findPromptBox(const VtScreen &screen) {
    const int bottom = lastRule(screen, screen.rows());
    const int top    = bottom < 0 ? -1 : lastRule(screen, bottom);
    if (top < 0 || bottom - top < 2)
        return std::nullopt;
    if (!str::startsWith(screen.row(top + 1), kPromptMark))
        return std::nullopt;
    PromptBox box;
    box.top = top + 1;
    for (int r = top + 1; r < bottom; ++r) {
        std::string line = screen.row(r);
        if (r == top + 1)
            line.erase(0, kPromptMark.size());
        if (!line.empty() && line[0] == ' ')
            line.erase(0, 1);
        box.lines.push_back(std::move(line));
    }
    box.empty = std::all_of(box.lines.begin(), box.lines.end(), [](const std::string &l) {
        return str::trimSpace(l).empty();
    });
    return box;
}

bool readyForInput(const VtScreen &screen) {
    const auto box = findPromptBox(screen);
    return box && box->empty && box->lines.size() == 1 && screen.cursorVisible() &&
           screen.cursorRow() == box->top;
}

namespace {

// The question drawn in rows [from, to): its text, then its numbered options.
std::optional<PermissionQuestion> questionIn(const VtScreen &screen, int from, int to) {
    PermissionQuestion q;
    std::string        text;
    for (int r = from; r < to; ++r) {
        const std::string row    = screen.row(r);
        int               number = 0;
        std::string       label;
        bool              marked = false;
        if (!parseOption(row, &number, &label, &marked)) {
            if (!q.options.empty())
                break; // past the options: "Esc to cancel · Tab to amend"
            const std::string_view t = str::trimSpace(row);
            if (!t.empty()) {
                if (!text.empty())
                    text += ' ';
                text += t;
            }
            continue;
        }
        if (number != int(q.options.size()) + 1)
            return std::nullopt; // a numbered list, but not one of options
        q.options.push_back({number, std::move(label)});
        if (marked) {
            if (q.selected)
                return std::nullopt;
            q.selected = number;
        }
    }
    if (q.options.size() < 2 || !q.selected)
        return std::nullopt;
    q.text = std::move(text);
    return q;
}

} // namespace

// Under the lowest rule that has one: 2.1.314 draws another rule (the
// session's name) below the question.
std::optional<PermissionQuestion> findPermissionQuestion(const VtScreen &screen) {
    for (int to = screen.rows(), rule = lastRule(screen, to); rule >= 0;
         to = rule, rule = lastRule(screen, rule))
        if (auto q = questionIn(screen, rule + 1, to))
            return q;
    return std::nullopt;
}

} // namespace claude
