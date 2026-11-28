// A compact JSON DOM (RFC 8259) and a writer.
//
// Document::parse takes ownership of the text and unescapes strings in place,
// so every string and key is a std::string_view into that one buffer — no
// per-string allocation. Nodes live in one flat vector (24 bytes each), in
// document order: a container's children follow it, and each node records
// where its subtree ends, which is how siblings are found. A Value is a
// {document, index} handle — cheap to copy, valid while the Document lives
// and is not re-parsed.
//
// Lookups on missing keys / wrong types never fail: they return an empty
// Value whose accessors yield the given default, so field mapping reads as
// `o["user"]["name"].str()` without guards.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace json {

enum class Type : uint8_t { Missing, Null, Bool, Int, Double, String, Array, Object };

class Document;

class Value {
public:
    Value() = default;

    Type type() const;
    bool exists() const { return _doc != nullptr; } // false: key/index not present
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isNumber() const { return type() == Type::Int || type() == Type::Double; }
    bool isInt() const { return type() == Type::Int; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    bool             boolean(bool def = false) const;
    // Int, or a Double with no fraction that fits; else def.
    int64_t          integer(int64_t def = 0) const;
    double           number(double def = 0) const; // Int or Double
    std::string_view str(std::string_view def = {}) const;

    // Element count of an array / member count of an object; 0 otherwise.
    size_t           size() const;
    Value            operator[](std::string_view key) const; // first member with that key
    Value            operator[](size_t index) const;
    Value            operator[](int index) const { return (*this)[size_t(index)]; }
    Value            operator[](const char *key) const { return (*this)[std::string_view(key)]; }
    bool             has(std::string_view key) const { return (*this)[key].exists(); }
    // The member name when this value was reached by iterating an object.
    std::string_view key() const;

    // Children of an array or object, in document order.
    class Iterator {
    public:
        Value     operator*() const { return Value(_doc, _i); }
        Iterator &operator++();
        bool      operator!=(const Iterator &o) const { return _i != o._i; }

    private:
        friend class Value;
        Iterator(const Document *d, uint32_t i) : _doc(d), _i(i) {}
        const Document *_doc;
        uint32_t        _i;
    };
    Iterator begin() const;
    Iterator end() const;

private:
    friend class Document;
    Value(const Document *d, uint32_t i) : _doc(d), _i(i) {}
    const Document *_doc = nullptr;
    uint32_t        _i   = 0;
};

class Document {
public:
    Document()                       = default;
    // Documents own their buffer; moving keeps Values valid only if they are
    // re-fetched from the new object (they point at the Document).
    Document(Document &&)            = default;
    Document &operator=(Document &&) = default;

    // Parses `text` (moved in and unescaped in place). On failure returns
    // false, leaves root() empty and sets *error to "line L, column C: why".
    bool parse(std::string text, std::string *error = nullptr);
    // Convenience copy for string literals / views.
    bool parse(std::string_view text, std::string *error) {
        return parse(std::string(text), error);
    }
    // Reads the file at `path` and parses it. On failure root() is empty and
    // *error is empty when the file can't be read, else the parse error.
    bool parseFile(std::string_view path, std::string *error = nullptr);

    Value root() const { return _nodes.empty() ? Value() : Value(this, 0); }

    // Nesting deeper than this is rejected (bounded recursion, no stack
    // overflow on hostile input).
    static constexpr int kMaxDepth = 256;

private:
    friend class Value;
    friend class Parser;
    struct Node {
        Type     type   = Type::Null;
        uint32_t end    = 0; // index one past this node's subtree
        uint32_t keyOff = 0, keyLen = 0;
        union {
            int64_t i;
            double  d;
            struct {
                uint32_t off, len;
            } s;
            uint32_t count;
        } v{};
    };
    std::string       _buf;
    std::vector<Node> _nodes;
};

// Appends `s` as a JSON string literal (quotes included) to out.
void escapeString(std::string &out, std::string_view s);

// A streaming writer. Commas, colons and (pretty) indentation are managed;
// the caller only pairs begin/end calls and puts key() before object values.
class Writer {
public:
    explicit Writer(bool pretty = false) : _pretty(pretty) {}

    Writer &beginObject();
    Writer &endObject();
    Writer &beginArray();
    Writer &endArray();
    Writer &key(std::string_view k);

    Writer &value(std::string_view s);
    Writer &value(const char *s) { return value(std::string_view(s)); }
    Writer &value(const std::string &s) { return value(std::string_view(s)); }
    Writer &value(int64_t n);
    Writer &value(int n) { return value(int64_t(n)); }
    Writer &value(double d); // NaN / infinity have no JSON form: written as null
    Writer &value(bool b);
    Writer &null();
    // Copies a parsed value (recursively).
    Writer &value(const Value &v);

    const std::string &str() const { return _out; }
    std::string        take() { return std::move(_out); }

private:
    void        prefix(); // separator + indentation before a new value
    void        open(char c);
    void        close(char c);
    std::string _out;
    std::string _first; // per open container: 1 = nothing written yet (a byte stack)
    bool        _pretty   = false;
    bool        _afterKey = false;
};

// Serialises a parsed value.
std::string write(const Value &v, bool pretty = false);

// ── Mapping onto structs ────────────────────────────────────────────────────
// v's string, copied ("" when it isn't one).
std::string owned(const Value &v);

// JSON field → struct member tables: one loop per struct instead of an
// inlined lookup and string assignment per field at every call site.
template <class T>
struct StrField {
    const char *key;
    std::string T::*field;
};
template <class T>
struct BoolField {
    const char *key;
    bool T::*field;
};
template <class T, size_t N>
void readStrings(const Value &o, T &obj, const StrField<T> (&fields)[N]) {
    for (const StrField<T> &f : fields)
        obj.*f.field = owned(o[f.key]);
}
template <class T, size_t N>
void readBools(const Value &o, T &obj, const BoolField<T> (&fields)[N]) {
    for (const BoolField<T> &f : fields)
        obj.*f.field = o[f.key].boolean();
}

} // namespace json
