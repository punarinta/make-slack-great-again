#include "base/json.h"

#include "base/file.h"
#include "base/str.h"
#include "base/utf8.h"

#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

// ── Value ───────────────────────────────────────────────────────────────────

Type Value::type() const {
    return _doc ? _doc->_nodes[_i].type : Type::Missing;
}

bool Value::boolean(bool def) const {
    return type() == Type::Bool ? _doc->_nodes[_i].v.i != 0 : def;
}

int64_t Value::integer(int64_t def) const {
    switch (type()) {
    case Type::Int:
        return _doc->_nodes[_i].v.i;
    case Type::Double: {
        const double d = _doc->_nodes[_i].v.d;
        // Slack sends some counters as 3.0; accept them, but not 3.5 or 1e300.
        if (d >= -9.2e18 && d <= 9.2e18 && d == std::floor(d))
            return int64_t(d);
        return def;
    }
    default:
        return def;
    }
}

double Value::number(double def) const {
    switch (type()) {
    case Type::Int:
        return double(_doc->_nodes[_i].v.i);
    case Type::Double:
        return _doc->_nodes[_i].v.d;
    default:
        return def;
    }
}

std::string_view Value::str(std::string_view def) const {
    if (type() != Type::String)
        return def;
    const auto &n = _doc->_nodes[_i];
    return std::string_view(_doc->_buf.data() + n.v.s.off, n.v.s.len);
}

size_t Value::size() const {
    const Type t = type();
    return (t == Type::Array || t == Type::Object) ? _doc->_nodes[_i].v.count : 0;
}

Value Value::operator[](std::string_view key) const {
    if (type() != Type::Object)
        return {};
    const auto    &nodes = _doc->_nodes;
    const char    *buf   = _doc->_buf.data();
    const uint32_t end   = nodes[_i].end;
    for (uint32_t c = _i + 1; c < end; c = nodes[c].end)
        if (nodes[c].keyLen == key.size() &&
            std::memcmp(buf + nodes[c].keyOff, key.data(), key.size()) == 0)
            return Value(_doc, c);
    return {};
}

Value Value::operator[](size_t index) const {
    if (type() != Type::Array || index >= _doc->_nodes[_i].v.count)
        return {};
    const auto &nodes = _doc->_nodes;
    uint32_t    c     = _i + 1;
    while (index--)
        c = nodes[c].end;
    return Value(_doc, c);
}

std::string_view Value::key() const {
    if (!_doc)
        return {};
    const auto &n = _doc->_nodes[_i];
    return std::string_view(_doc->_buf.data() + n.keyOff, n.keyLen);
}

Value::Iterator &Value::Iterator::operator++() {
    _i = _doc->_nodes[_i].end;
    return *this;
}

Value::Iterator Value::begin() const {
    const Type t = type();
    if (t != Type::Array && t != Type::Object)
        return Iterator(_doc, 0);
    return Iterator(_doc, _i + 1);
}

Value::Iterator Value::end() const {
    const Type t = type();
    if (t != Type::Array && t != Type::Object)
        return Iterator(_doc, 0);
    return Iterator(_doc, _doc->_nodes[_i].end);
}

// ── Parser ──────────────────────────────────────────────────────────────────

class Parser {
public:
    Parser(Document &d) : _d(d), _s(d._buf.data()), _n(d._buf.size()) {}

    bool run(std::string *error) {
        skipWs();
        if (!parseValue(0, 0, 0))
            return report(error);
        skipWs();
        if (_p != _n) {
            fail("trailing characters after the document");
            return report(error);
        }
        return true;
    }

private:
    Document   &_d;
    char       *_s;
    size_t      _n;
    size_t      _p     = 0;
    const char *_why   = nullptr;
    size_t      _errAt = 0;

    bool fail(const char *why) {
        if (!_why) {
            _why   = why;
            _errAt = _p;
        }
        return false;
    }

    bool report(std::string *error) {
        if (error) {
            int line = 1, col = 1;
            for (size_t i = 0; i < _errAt && i < _n; ++i) {
                if (_s[i] == '\n')
                    ++line, col = 1;
                else
                    ++col;
            }
            char buf[96];
            std::snprintf(
                buf, sizeof buf, "line %d, column %d: %s", line, col, _why ? _why : "invalid JSON"
            );
            *error = buf;
        }
        return false;
    }

    void skipWs() {
        while (_p < _n && (_s[_p] == ' ' || _s[_p] == '\n' || _s[_p] == '\r' || _s[_p] == '\t'))
            ++_p;
    }

    uint32_t push(Type t, uint32_t keyOff, uint32_t keyLen) {
        Document::Node node;
        node.type   = t;
        node.keyOff = keyOff;
        node.keyLen = keyLen;
        _d._nodes.push_back(node);
        return uint32_t(_d._nodes.size() - 1);
    }

    bool literal(const char *word, size_t len) {
        if (_n - _p < len || std::memcmp(_s + _p, word, len) != 0)
            return fail("invalid literal");
        _p += len;
        return true;
    }

    bool hex4(uint32_t &out) {
        if (_n - _p < 4)
            return fail("truncated \\u escape");
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const int h = str::hexDigit(_s[_p + k]);
            if (h < 0)
                return fail("invalid \\u escape");
            out = (out << 4) | uint32_t(h);
        }
        _p += 4;
        return true;
    }

    // Parses a string starting at the opening quote, unescaping in place: the
    // write cursor never passes the read cursor (every escape is at least as
    // long as what it decodes to).
    bool parseString(uint32_t &off, uint32_t &len) {
        ++_p; // opening quote
        const size_t start = _p;
        size_t       w     = _p;
        while (true) {
            if (_p >= _n)
                return fail("unterminated string");
            const unsigned char c = uint8_t(_s[_p]);
            if (c == '"') {
                ++_p;
                break;
            }
            if (c < 0x20)
                return fail("control character in string");
            if (c != '\\') {
                // A run of plain bytes: moved only once an escape has shrunk
                // the string (before that, w == run and nothing moves).
                const size_t run = _p;
                while (++_p < _n) {
                    const unsigned char b = uint8_t(_s[_p]);
                    if (b == '"' || b == '\\' || b < 0x20)
                        break;
                }
                if (w != run)
                    std::memmove(_s + w, _s + run, _p - run);
                w += _p - run;
                continue;
            }
            if (++_p >= _n)
                return fail("unterminated string");
            const char e = _s[_p++];
            switch (e) {
            case '"':
                _s[w++] = '"';
                break;
            case '\\':
                _s[w++] = '\\';
                break;
            case '/':
                _s[w++] = '/';
                break;
            case 'b':
                _s[w++] = '\b';
                break;
            case 'f':
                _s[w++] = '\f';
                break;
            case 'n':
                _s[w++] = '\n';
                break;
            case 'r':
                _s[w++] = '\r';
                break;
            case 't':
                _s[w++] = '\t';
                break;
            case 'u': {
                uint32_t cp;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // A high surrogate needs its low half; a lone one (seen in
                    // truncated emoji from some bots) becomes U+FFFD.
                    uint32_t lo = 0;
                    if (_n - _p >= 6 && _s[_p] == '\\' && _s[_p + 1] == 'u') {
                        const size_t save = _p;
                        _p += 2;
                        if (!hex4(lo))
                            return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else {
                            cp = utf8::kReplacement;
                            _p = save;
                        }
                    } else {
                        cp = utf8::kReplacement;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = utf8::kReplacement;
                }
                w += utf8::encode(_s + w, cp);
                break;
            }
            default:
                --_p;
                return fail("invalid escape");
            }
        }
        off = uint32_t(start);
        len = uint32_t(w - start);
        return true;
    }

    bool parseNumber(uint32_t node) {
        const size_t start = _p;
        bool         isInt = true;
        if (_s[_p] == '-')
            ++_p;
        if (_p >= _n)
            return fail("invalid number");
        if (_s[_p] == '0') {
            ++_p;
            if (_p < _n && _s[_p] >= '0' && _s[_p] <= '9')
                return fail("leading zero in number");
        } else if (_s[_p] >= '1' && _s[_p] <= '9') {
            while (_p < _n && _s[_p] >= '0' && _s[_p] <= '9')
                ++_p;
        } else {
            return fail("invalid number");
        }
        if (_p < _n && _s[_p] == '.') {
            isInt = false;
            ++_p;
            if (_p >= _n || _s[_p] < '0' || _s[_p] > '9')
                return fail("digit expected after decimal point");
            while (_p < _n && _s[_p] >= '0' && _s[_p] <= '9')
                ++_p;
        }
        if (_p < _n && (_s[_p] == 'e' || _s[_p] == 'E')) {
            isInt = false;
            ++_p;
            if (_p < _n && (_s[_p] == '+' || _s[_p] == '-'))
                ++_p;
            if (_p >= _n || _s[_p] < '0' || _s[_p] > '9')
                return fail("digit expected in exponent");
            while (_p < _n && _s[_p] >= '0' && _s[_p] <= '9')
                ++_p;
        }
        auto &n = _d._nodes[node];
        if (isInt) {
            int64_t v = 0;
            auto    r = std::from_chars(_s + start, _s + _p, v);
            if (r.ec == std::errc()) {
                n.type = Type::Int;
                n.v.i  = v;
                return true;
            }
            // Out of int64 range: keep it as a double, like JavaScript would.
        }
        // strtod, not std::from_chars: libstdc++'s floating-point charconv is
        // ~160 KB in a static binary. The grammar is checked above, and msga
        // never calls setlocale, so the decimal point is always '.'.
        // The number ends at _p, which stays readable (the buffer's own NUL
        // at the very end): end it there for strtod, then put the byte back.
        const char after = _s[_p];
        _s[_p]           = '\0';
        errno            = 0;
        double d         = std::strtod(_s + start, nullptr);
        _s[_p]           = after;
        if (errno == ERANGE)
            d = 0; // out of range: 0, as std::from_chars left it
        n.type = Type::Double;
        n.v.d  = d;
        return true;
    }

    bool parseValue(int depth, uint32_t keyOff, uint32_t keyLen) {
        if (_p >= _n)
            return fail("value expected");
        const char c = _s[_p];
        uint32_t   node;
        switch (c) {
        case '{':
        case '[': {
            if (depth >= Document::kMaxDepth)
                return fail("nesting too deep");
            const bool obj = c == '{';
            node           = push(obj ? Type::Object : Type::Array, keyOff, keyLen);
            ++_p;
            uint32_t count = 0;
            skipWs();
            if (_p < _n && _s[_p] == (obj ? '}' : ']')) {
                ++_p;
            } else {
                while (true) {
                    uint32_t ko = 0, kl = 0;
                    if (obj) {
                        if (_p >= _n || _s[_p] != '"')
                            return fail("member name expected");
                        if (!parseString(ko, kl))
                            return false;
                        skipWs();
                        if (_p >= _n || _s[_p] != ':')
                            return fail("':' expected");
                        ++_p;
                        skipWs();
                    }
                    if (!parseValue(depth + 1, ko, kl))
                        return false;
                    ++count;
                    skipWs();
                    if (_p < _n && _s[_p] == ',') {
                        ++_p;
                        skipWs();
                        continue;
                    }
                    if (_p < _n && _s[_p] == (obj ? '}' : ']')) {
                        ++_p;
                        break;
                    }
                    return fail(obj ? "',' or '}' expected" : "',' or ']' expected");
                }
            }
            _d._nodes[node].v.count = count;
            break;
        }
        case '"': {
            node = push(Type::String, keyOff, keyLen);
            uint32_t off, len;
            if (!parseString(off, len))
                return false;
            _d._nodes[node].v.s.off = off;
            _d._nodes[node].v.s.len = len;
            break;
        }
        case 't':
            node = push(Type::Bool, keyOff, keyLen);
            if (!literal("true", 4))
                return false;
            _d._nodes[node].v.i = 1;
            break;
        case 'f':
            node = push(Type::Bool, keyOff, keyLen);
            if (!literal("false", 5))
                return false;
            break;
        case 'n':
            node = push(Type::Null, keyOff, keyLen);
            if (!literal("null", 4))
                return false;
            break;
        default:
            if (c != '-' && (c < '0' || c > '9'))
                return fail("unexpected character");
            node = push(Type::Int, keyOff, keyLen);
            if (!parseNumber(node))
                return false;
            break;
        }
        _d._nodes[node].end = uint32_t(_d._nodes.size());
        return true;
    }
};

bool Document::parse(std::string text, std::string *error) {
    _buf = std::move(text);
    // clear() keeps the capacity: a Document re-parsed per line or response
    // grows once, not each time.
    _nodes.clear();
    // Slack payloads average one node per ~12 bytes, so this rarely regrows.
    // String-heavy text needs far fewer; the pages never touched stay
    // uncommitted, so the slack is address space, not memory (no
    // shrink_to_fit: that would copy every node a second time).
    _nodes.reserve(_buf.size() / 10 + 4);
    Parser p(*this);
    if (!p.run(error)) {
        _nodes.clear();
        return false;
    }
    return true;
}

bool Document::parseFile(std::string_view path, std::string *error) {
    if (error)
        error->clear();
    std::string text;
    if (!file::readAll(path, &text)) {
        _nodes.clear();
        return false;
    }
    return parse(std::move(text), error);
}

// ── Writer ──────────────────────────────────────────────────────────────────

void escapeString(std::string &out, std::string_view s) {
    const char *const kHex = str::kHexLower;
    out += '"';
    size_t run = 0; // start of the pending unescaped run
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = uint8_t(s[i]);
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;
        out.append(s.data() + run, i - run);
        run = i + 1;
        out += '\\';
        switch (c) {
        case '"':
            out += '"';
            break;
        case '\\':
            out += '\\';
            break;
        case '\n':
            out += 'n';
            break;
        case '\r':
            out += 'r';
            break;
        case '\t':
            out += 't';
            break;
        case '\b':
            out += 'b';
            break;
        case '\f':
            out += 'f';
            break;
        default:
            out += "u00";
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    out.append(s.data() + run, s.size() - run);
    out += '"';
}

void Writer::prefix() {
    if (_afterKey) {
        _afterKey = false;
        return;
    }
    if (_first.empty())
        return;
    if (_first.back() == 0)
        _out += ',';
    _first.back() = 0;
    if (_pretty) {
        _out += '\n';
        _out.append(_first.size() * 2, ' ');
    }
}

void Writer::open(char c) {
    prefix();
    _out += c;
    _first.push_back(1);
}

void Writer::close(char c) {
    const bool empty = _first.empty() || _first.back() != 0;
    if (!_first.empty())
        _first.pop_back();
    if (_pretty && !empty) {
        _out += '\n';
        _out.append(_first.size() * 2, ' ');
    }
    _out += c;
}

Writer &Writer::beginObject() {
    open('{');
    return *this;
}
Writer &Writer::endObject() {
    close('}');
    return *this;
}
Writer &Writer::beginArray() {
    open('[');
    return *this;
}
Writer &Writer::endArray() {
    close(']');
    return *this;
}

Writer &Writer::key(std::string_view k) {
    prefix();
    escapeString(_out, k);
    _out += _pretty ? ": " : ":";
    _afterKey = true;
    return *this;
}

Writer &Writer::value(std::string_view s) {
    prefix();
    escapeString(_out, s);
    return *this;
}

Writer &Writer::value(int64_t n) {
    prefix();
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof buf, n);
    _out.append(buf, r.ptr);
    return *this;
}

Writer &Writer::value(double d) {
    if (!std::isfinite(d))
        return null();
    prefix();
    // The shortest of %.15g..%.17g that reads back as the same double (17
    // digits always do). snprintf, not std::to_chars: see parseNumber.
    char buf[32];
    int  len = 0;
    for (int prec = 15; prec <= 17; ++prec) {
        len = std::snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d)
            break;
    }
    _out.append(buf, size_t(len));
    // Keep it a double on re-parse: "3" would come back as an integer.
    if (!std::memchr(buf, '.', size_t(len)) && !std::memchr(buf, 'e', size_t(len)))
        _out += ".0";
    return *this;
}

Writer &Writer::value(bool b) {
    prefix();
    _out += b ? "true" : "false";
    return *this;
}

Writer &Writer::null() {
    prefix();
    _out += "null";
    return *this;
}

Writer &Writer::value(const Value &v) {
    switch (v.type()) {
    case Type::Missing:
    case Type::Null:
        return null();
    case Type::Bool:
        return value(v.boolean());
    case Type::Int:
        return value(v.integer());
    case Type::Double:
        return value(v.number());
    case Type::String:
        return value(v.str());
    case Type::Array:
        beginArray();
        for (Value c : v)
            value(c);
        return endArray();
    case Type::Object:
        beginObject();
        for (Value c : v) {
            key(c.key());
            value(c);
        }
        return endObject();
    }
    return *this;
}

std::string write(const Value &v, bool pretty) {
    Writer w(pretty);
    w.value(v);
    return w.take();
}

std::string owned(const Value &v) {
    return std::string(v.str());
}

} // namespace json
