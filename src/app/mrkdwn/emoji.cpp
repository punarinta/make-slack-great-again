#include "app/mrkdwn/emoji.h"

#include "base/i18n.h"
#include "base/utf8.h"

#include <cstring>

namespace emoji {

namespace {

#include "app/mrkdwn/emoji_categories.inc"
#include "app/mrkdwn/emoji_table.inc"

constexpr int kRestartCount = (kEmojiCount + kEmojiRestart - 1) / kEmojiRestart;

// Walks the front-coded entries: `name` holds the full current name.
struct Cursor {
    uint32_t pos = 0; // offset of the next entry in kEmojiData
    char     name[256];
    uint32_t nameLen  = 0;
    uint32_t valueOff = 0, valueLen = 0;

    void next() {
        const unsigned shared = kEmojiData[pos];
        const unsigned suffix = kEmojiData[pos + 1];
        std::memcpy(name + shared, kEmojiData + pos + 2, suffix);
        nameLen  = shared + suffix;
        valueLen = kEmojiData[pos + 2 + suffix];
        valueOff = pos + 3 + suffix;
        pos      = valueOff + valueLen;
    }
    std::string_view view() const { return std::string_view(name, nameLen); }
};

// Inverse of gen_emoji_table.py's encode_cp.
std::string decodeValue(uint32_t off, uint32_t len) {
    std::string          out;
    const unsigned char *p = kEmojiData + off, *e = p + len;
    while (p < e) {
        const unsigned b = *p++;
        uint32_t       cp;
        if (b < 0x10) {
            cp = 0x1F000 + ((b << 8) | *p++);
        } else if (b == 0x10) {
            cp = 0x200D;
        } else if (b == 0x11) {
            cp = 0xFE0F;
        } else if (b <= 0x16) {
            cp = 0x1F3FB + (b - 0x12);
        } else if (b == 0x17) {
            cp = 0x20E3;
        } else if (b < 0x80) {
            cp = b;
        } else if (b < 0xA0) {
            cp = 0x1F1E6 + (b - 0x80);
        } else if (b < 0xF0) {
            cp = 0x2000 + (((b - 0xA0) << 8) | *p++);
        } else {
            cp = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
            p += 3;
        }
        utf8::append(out, cp);
    }
    return out;
}

std::string_view restartName(int r) {
    const uint32_t pos = kEmojiRestarts[r];
    return std::string_view(
        reinterpret_cast<const char *>(kEmojiData + pos + 2), kEmojiData[pos + 1]
    );
}

// Index of the last restart whose name is <= key (0 if none).
int restartFor(std::string_view key) {
    int lo = 0, hi = kRestartCount; // invariant: answer in [lo, hi)
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (restartName(mid) <= key)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

bool findValue(std::string_view name, uint32_t *off, uint32_t *len) {
    if (name.empty() || name.size() > 255)
        return false;
    Cursor c;
    c.pos = kEmojiRestarts[restartFor(name)];
    for (int k = 0; k < kEmojiRestart && c.pos < sizeof kEmojiData; ++k) {
        c.next();
        const std::string_view n = c.view();
        if (n == name) {
            *off = c.valueOff;
            *len = c.valueLen;
            return true;
        }
        if (n > name)
            return false;
    }
    return false;
}

std::string lookupPlain(std::string_view name) {
    uint32_t off, len;
    return findValue(name, &off, &len) ? decodeValue(off, len) : std::string();
}

} // namespace

std::string applySkinTone(std::string_view u, int tone) {
    if (tone < 2 || tone > 6 || u.empty())
        return std::string(u);
    size_t i = 0;
    utf8::decode(u, i); // first code point
    std::string out(u.substr(0, i));
    utf8::append(out, 0x1F3FB + uint32_t(tone - 2));
    size_t rest = i;
    if (u.compare(rest, 3, "\xEF\xB8\x8F") == 0) // VS16 is replaced by the modifier
        rest += 3;
    out.append(u.substr(rest));
    return out;
}

std::string toUnicode(std::string_view name) {
    // "base::mod1::mod2": Slack's modifier suffix (reactions, rich_text).
    const size_t sep = name.find("::");
    if (sep == std::string_view::npos || sep == 0)
        return lookupPlain(name);
    std::string out = lookupPlain(name.substr(0, sep));
    if (out.empty())
        return out;
    std::string_view mods = name.substr(sep + 2);
    while (!mods.empty()) {
        const size_t     next = mods.find("::");
        std::string_view mod  = mods.substr(0, next);
        if (mod.size() == 11 && mod.substr(0, 10) == "skin-tone-" && mod[10] >= '2' &&
            mod[10] <= '6')
            out = applySkinTone(out, mod[10] - '0');
        else if (!mod.empty())
            out += lookupPlain(mod); // any other modifier glyph: appended
        if (next == std::string_view::npos)
            break;
        mods.remove_prefix(next + 2);
    }
    return out;
}

bool isKnown(std::string_view name) {
    uint32_t off, len;
    return findValue(name, &off, &len);
}

std::string expandShortcodes(std::string_view text) {
    if (text.find(':') == std::string_view::npos)
        return std::string(text);
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != ':') {
            out += text[i++];
            continue;
        }
        size_t j = i + 1;
        while (j < text.size() && text[j] != ':' && text[j] != ' ' && text[j] != '\n')
            ++j;
        if (j < text.size() && text[j] == ':' && j > i + 1) {
            uint32_t off, len;
            if (findValue(text.substr(i + 1, j - i - 1), &off, &len)) {
                out += decodeValue(off, len);
                i = j + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

void complete(std::string_view prefix, size_t max, std::vector<std::string> &out) {
    if (max == 0)
        return;
    Cursor c;
    c.pos = kEmojiRestarts[restartFor(prefix)];
    for (int i = restartFor(prefix) * kEmojiRestart; i < kEmojiCount; ++i) {
        c.next();
        const std::string_view n = c.view();
        if (n.substr(0, prefix.size()) == prefix) {
            out.emplace_back(n);
            if (out.size() >= max)
                return;
        } else if (n > prefix) {
            return; // sorted: past every name with this prefix
        }
    }
}

void forEach(const std::function<bool(std::string_view, const std::string &)> &fn) {
    Cursor c;
    for (int i = 0; i < kEmojiCount; ++i) {
        c.next();
        if (!fn(c.view(), decodeValue(c.valueOff, c.valueLen)))
            return;
    }
}

void forEachName(const std::function<bool(std::string_view)> &fn) {
    Cursor c;
    for (int i = 0; i < kEmojiCount; ++i) {
        c.next();
        if (!fn(c.view()))
            return;
    }
}

int count() {
    return kEmojiCount;
}

// ── The picker's categories ─────────────────────────────────────────────────

namespace {

// Entry i of the sorted table: from its restart point, at most 15 steps.
Cursor cursorAt(int i) {
    Cursor c;
    c.pos = kEmojiRestarts[i / kEmojiRestart];
    for (int k = 0; k <= i % kEmojiRestart; ++k)
        c.next();
    return c;
}

int indexOf(std::string_view name) {
    if (name.empty() || name.size() > 255)
        return -1;
    const int r = restartFor(name);
    Cursor    c;
    c.pos = kEmojiRestarts[r];
    for (int k = 0; k < kEmojiRestart && r * kEmojiRestart + k < kEmojiCount; ++k) {
        c.next();
        if (c.view() == name)
            return r * kEmojiRestart + k;
        if (c.view() > name)
            return -1;
    }
    return -1;
}

} // namespace

int categoryCount() {
    return int(sizeof kEmojiCategories / sizeof kEmojiCategories[0]);
}

const char *categoryId(int cat) {
    return kEmojiCategories[cat].id;
}

const char *categoryLabel(int cat) {
    return i18n::tr(kEmojiCategories[cat].label);
}

void categoryEntries(int cat, std::vector<std::pair<std::string, std::string>> &out) {
    const EmojiCategory &c = kEmojiCategories[cat];
    out.reserve(out.size() + c.count);
    for (unsigned k = 0; k < c.count; ++k) {
        const Cursor cur = cursorAt(kEmojiCategoryNames[c.first + k]);
        out.emplace_back(std::string(cur.view()), decodeValue(cur.valueOff, cur.valueLen));
    }
}

bool supportsSkinTone(std::string_view name) {
    const int i = indexOf(name);
    return i >= 0 && (kEmojiSkinBits[i / 8] >> (i % 8) & 1);
}

} // namespace emoji
