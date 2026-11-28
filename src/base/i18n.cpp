#include "base/i18n.h"

#include "base/str.h"
#include "prim/bytes.h"

#include <cstring>
#include <memory>

namespace i18n {

namespace {

int englishPlural(int64_t n) {
    return n == 1 ? 0 : 1;
}

// Few languages ever: a fixed array, no container.
constexpr int   kMaxLanguages          = 16;
const Language *g_langs[kMaxLanguages] = {};
int             g_langCount            = 0;

// The active table (English: none). The blob owns the strings tr() returns,
// so it lives until the next switch; switching happens at startup only.
struct Active {
    const Language             *lang = nullptr;
    std::string                 blob;
    uint32_t                    count = 0;
    std::unique_ptr<uint32_t[]> offsets; // count * formCount string starts
};
Active g_active;

using prim::le32;

// Validates and indexes a table; false leaves `a` untouched.
bool parse(const Language &l, std::string blob, Active &a) {
    if (blob.size() < 4 || l.formCount < 1)
        return false;
    const uint32_t count = le32(blob.data());
    const size_t   base  = 4 + size_t(count) * 4;
    if (base > blob.size() || blob.back() != '\0')
        return false;
    const size_t total = size_t(count) * l.formCount;
    auto         offs  = std::make_unique<uint32_t[]>(total);
    size_t       at    = base;
    for (size_t i = 0; i < total; ++i) {
        if (at >= blob.size())
            return false;
        offs[i] = uint32_t(at);
        at += std::strlen(blob.data() + at) + 1;
    }
    a.lang    = &l;
    a.blob    = std::move(blob);
    a.count   = count;
    a.offsets = std::move(offs);
    return true;
}

// The translated form f of msgid, or null.
const char *lookup(const char *msgid, int f) {
    if (!g_active.count)
        return nullptr;
    const uint32_t h  = hash(msgid);
    const char    *hs = g_active.blob.data() + 4;
    size_t         lo = 0, hi = g_active.count;
    while (lo < hi) {
        const size_t   mid = (lo + hi) / 2;
        const uint32_t v   = le32(hs + mid * 4);
        if (v == h) {
            const char *s =
                g_active.blob.data() + g_active.offsets[mid * g_active.lang->formCount + f];
            return *s ? s : nullptr;
        }
        if (v < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    return nullptr;
}

} // namespace

uint32_t hash(std::string_view s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s)
        h = (h ^ c) * 16777619u;
    return h;
}

void registerLanguage(const Language *lang) {
    for (int i = 0; i < g_langCount; ++i) {
        if (std::strcmp(g_langs[i]->code, lang->code) == 0) {
            g_langs[i] = lang; // re-registration replaces
            return;
        }
    }
    if (g_langCount < kMaxLanguages)
        g_langs[g_langCount++] = lang;
}

bool setLanguage(std::string_view code) {
    if (code == "en") {
        g_active = Active{};
        return true;
    }
    for (int i = 0; i < g_langCount; ++i) {
        if (code == g_langs[i]->code) {
            Active a;
            if (!parse(*g_langs[i], g_langs[i]->load(), a))
                return false;
            g_active = std::move(a);
            return true;
        }
    }
    return false;
}

void setPreferredLanguage(const std::vector<std::string> &tags) {
    for (const std::string &t : tags) {
        // "ja-JP", "ja_JP.UTF-8", "zh-Hant-TW": the primary subtag, lowercased.
        std::string code;
        for (char c : t) {
            if (c == '-' || c == '_' || c == '.' || c == '@')
                break;
            code += str::asciiLower(c);
        }
        if (code == "c" || code == "posix")
            code = "en";
        if (!code.empty() && setLanguage(code))
            return;
    }
    setLanguage("en");
}

const char *currentCode() {
    return g_active.lang ? g_active.lang->code : "en";
}

const char *tr(const char *msgid) {
    const char *t = lookup(msgid, 0);
    return t ? t : msgid;
}

std::string trn(const char *singular, const char *plural, int64_t n) {
    const char *text = nullptr;
    if (g_active.count) {
        int f = g_active.lang->plural(n);
        if (f < 0 || f >= g_active.lang->formCount)
            f = 0;
        text = lookup(singular, f);
    }
    if (!text)
        text = englishPlural(n) == 0 ? singular : plural;

    std::string       out;
    const std::string num = str::number(n);
    for (const char *p = text; *p; ++p) {
        if (p[0] == '%' && p[1] == 'n') {
            out += num;
            ++p;
        } else {
            out += *p;
        }
    }
    return out;
}

std::string
arg(std::string_view fmt, std::string_view a1, std::string_view a2, std::string_view a3) {
    std::string out;
    out.reserve(fmt.size() + a1.size() + a2.size() + a3.size());
    for (size_t i = 0; i < fmt.size(); ++i) {
        const char d = i + 1 < fmt.size() ? fmt[i + 1] : 0;
        if (fmt[i] == '%' && d >= '1' && d <= '3') {
            out += d == '1' ? a1 : d == '2' ? a2 : a3;
            ++i;
        } else {
            out += fmt[i];
        }
    }
    return out;
}

} // namespace i18n
