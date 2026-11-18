#include "text/unicode.h"

#include <hb.h>

namespace text::uni {

namespace {

struct Range {
    uint32_t lo, hi;
};

template <size_t N>
bool inRanges(const Range (&t)[N], uint32_t cp) {
    size_t lo = 0, hi = N;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (cp < t[mid].lo)
            hi = mid;
        else if (cp > t[mid].hi)
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

// Extended_Pictographic, coarsened to blocks: the only consumers are grapheme
// joining after ZWJ and "is this an emoji-ish symbol", both of which tolerate
// a few extra symbols. The exact property is ~80 ranges we don't need.
constexpr Range kPict[] = {
    {0x00A9, 0x00A9},   {0x00AE, 0x00AE},   {0x203C, 0x203C},   {0x2049, 0x2049},
    {0x2122, 0x2122},   {0x2139, 0x2139},   {0x2194, 0x2199},   {0x21A9, 0x21AA},
    {0x231A, 0x231B},   {0x2328, 0x2328},   {0x23CF, 0x23CF},   {0x23E9, 0x23F3},
    {0x23F8, 0x23FA},   {0x24C2, 0x24C2},   {0x25AA, 0x25AB},   {0x25B6, 0x25B6},
    {0x25C0, 0x25C0},   {0x25FB, 0x25FE},   {0x2600, 0x27BF},   {0x2934, 0x2935},
    {0x2B05, 0x2B07},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},
    {0x3030, 0x3030},   {0x303D, 0x303D},   {0x3297, 0x3297},   {0x3299, 0x3299},
    {0x1F000, 0x1F0FF}, {0x1F10D, 0x1F10F}, {0x1F12F, 0x1F12F}, {0x1F16C, 0x1F171},
    {0x1F17E, 0x1F17F}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1AD, 0x1F1E5},
    {0x1F201, 0x1F20F}, {0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F}, {0x1F232, 0x1F23A},
    {0x1F23C, 0x1F23F}, {0x1F249, 0x1F3FA}, {0x1F400, 0x1F53D}, {0x1F546, 0x1F64F},
    {0x1F680, 0x1F6FF}, {0x1F774, 0x1F77F}, {0x1F7D5, 0x1F7FF}, {0x1F80C, 0x1F80F},
    {0x1F848, 0x1F84F}, {0x1F85A, 0x1F85F}, {0x1F888, 0x1F88F}, {0x1F8AE, 0x1F8FF},
    {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1FAFF}, {0x1FC00, 0x1FFFD},
};

// Emoji_Presentation=Yes: shown as colour emoji without a VS16.
constexpr Range kEmojiDefault[] = {
    {0x231A, 0x231B},   {0x23E9, 0x23EC},   {0x23F0, 0x23F0},   {0x23F3, 0x23F3},
    {0x25FD, 0x25FE},   {0x2614, 0x2615},   {0x2648, 0x2653},   {0x267F, 0x267F},
    {0x2693, 0x2693},   {0x26A1, 0x26A1},   {0x26AA, 0x26AB},   {0x26BD, 0x26BE},
    {0x26C4, 0x26C5},   {0x26CE, 0x26CE},   {0x26D4, 0x26D4},   {0x26EA, 0x26EA},
    {0x26F2, 0x26F3},   {0x26F5, 0x26F5},   {0x26FA, 0x26FA},   {0x26FD, 0x26FD},
    {0x2705, 0x2705},   {0x270A, 0x270B},   {0x2728, 0x2728},   {0x274C, 0x274C},
    {0x274E, 0x274E},   {0x2753, 0x2755},   {0x2757, 0x2757},   {0x2795, 0x2797},
    {0x27B0, 0x27B0},   {0x27BF, 0x27BF},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},   {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F1E6, 0x1F1FF}, {0x1F201, 0x1F201}, {0x1F21A, 0x1F21A},
    {0x1F22F, 0x1F22F}, {0x1F232, 0x1F236}, {0x1F238, 0x1F23A}, {0x1F250, 0x1F251},
    {0x1F300, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4},
    {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC},
    {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF},
};

// Where lines may break on both sides without spaces (UAX #14 ID class).
constexpr Range kIdeo[] = {
    {0x1100, 0x115F},
    {0x2E80, 0x2FFF},
    {0x3040, 0x30FF},
    {0x3100, 0x31FF},
    {0x3200, 0x4DBF},
    {0x4E00, 0x9FFF},
    {0xA000, 0xA4CF},
    {0xAC00, 0xD7AF},
    {0xF900, 0xFAFF},
    {0xFE30, 0xFE4F},
    {0xFF01, 0xFF60},
    {0xFFE0, 0xFFE6},
    {0x1F000, 0x1FAFF},
    {0x20000, 0x3FFFD},
};

// Closing punctuation, CJK small kana and iteration marks: never start a line
// with them (UAX #14 CL/CP/EX/IS/NS, the common members).
constexpr Range kNoBreakBefore[] = {
    {0x0021, 0x0021}, {0x0029, 0x0029}, {0x002C, 0x002C}, {0x002E, 0x002E}, {0x003A, 0x003B},
    {0x003F, 0x003F}, {0x005D, 0x005D}, {0x007D, 0x007D}, {0x00BB, 0x00BB}, {0x2019, 0x2019},
    {0x201D, 0x201D}, {0x2026, 0x2026}, {0x203A, 0x203A}, {0x3001, 0x3002}, {0x3005, 0x3005},
    {0x3009, 0x3009}, {0x300B, 0x300B}, {0x300D, 0x300D}, {0x300F, 0x300F}, {0x3011, 0x3011},
    {0x3015, 0x3015}, {0x3017, 0x3017}, {0x3019, 0x3019}, {0x301B, 0x301B}, {0x3041, 0x3041},
    {0x3043, 0x3043}, {0x3045, 0x3045}, {0x3047, 0x3047}, {0x3049, 0x3049}, {0x3063, 0x3063},
    {0x3083, 0x3083}, {0x3085, 0x3085}, {0x3087, 0x3087}, {0x308E, 0x308E}, {0x309D, 0x309E},
    {0x30A1, 0x30A1}, {0x30A3, 0x30A3}, {0x30A5, 0x30A5}, {0x30A7, 0x30A7}, {0x30A9, 0x30A9},
    {0x30C3, 0x30C3}, {0x30E3, 0x30E3}, {0x30E5, 0x30E5}, {0x30E7, 0x30E7}, {0x30EE, 0x30EE},
    {0x30F5, 0x30F6}, {0x30FB, 0x30FE}, {0xFF01, 0xFF01}, {0xFF09, 0xFF09}, {0xFF0C, 0xFF0C},
    {0xFF0E, 0xFF0E}, {0xFF1A, 0xFF1B}, {0xFF1F, 0xFF1F}, {0xFF3D, 0xFF3D}, {0xFF5D, 0xFF5D},
};

// Opening punctuation: never end a line with them (UAX #14 OP, common members).
constexpr Range kNoBreakAfter[] = {
    {0x0028, 0x0028}, {0x005B, 0x005B}, {0x007B, 0x007B}, {0x00AB, 0x00AB}, {0x2018, 0x2018},
    {0x201C, 0x201C}, {0x2039, 0x2039}, {0x3008, 0x3008}, {0x300A, 0x300A}, {0x300C, 0x300C},
    {0x300E, 0x300E}, {0x3010, 0x3010}, {0x3014, 0x3014}, {0x3016, 0x3016}, {0x3018, 0x3018},
    {0x301A, 0x301A}, {0xFF08, 0xFF08}, {0xFF3B, 0xFF3B}, {0xFF5B, 0xFF5B},
};

hb_unicode_funcs_t *ufuncs() {
    static hb_unicode_funcs_t *f = hb_unicode_funcs_get_default();
    return f;
}

enum Gcb : int8_t {
    GOther,
    GCR,
    GLF,
    GControl,
    GExtend,
    GZWJ,
    GRI,
    GSpacing,
    GL,
    GV,
    GT,
    GLV,
    GLVT,
    GPict
};

int gcbClass(uint32_t cp) {
    if (cp < 0x7f) {
        if (cp == '\r')
            return GCR;
        if (cp == '\n')
            return GLF;
        return cp < 0x20 ? GControl : GOther;
    }
    if (cp == 0x200D)
        return GZWJ;
    if (cp == 0x200C || (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F))
        return GExtend; // ZWNJ, skin tones, emoji tag sequences
    if (isRegional(cp))
        return GRI;
    if (cp >= 0x1100 && cp <= 0x11FF)
        return cp < 0x1160 ? GL : cp < 0x11A8 ? GV : GT;
    if (cp >= 0xAC00 && cp <= 0xD7A3)
        return (cp - 0xAC00) % 28 == 0 ? GLV : GLVT;
    switch (category(cp)) {
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
        return GExtend;
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
        return GSpacing;
    case HB_UNICODE_GENERAL_CATEGORY_CONTROL:
    case HB_UNICODE_GENERAL_CATEGORY_FORMAT:
    case HB_UNICODE_GENERAL_CATEGORY_LINE_SEPARATOR:
    case HB_UNICODE_GENERAL_CATEGORY_PARAGRAPH_SEPARATOR:
        return GControl;
    default:
        break;
    }
    return isExtPict(cp) ? GPict : GOther;
}

enum Bidi : uint8_t { BL, BR, BAL, BEN, BES, BET, BAN, BCS, BNSM, BBN, BB, BS, BWS, BON };

bool strongRtlBlock(uint32_t cp, bool *arabic) {
    *arabic = (cp >= 0x0600 && cp <= 0x07BF) || (cp >= 0x0860 && cp <= 0x08FF) ||
              (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF) ||
              (cp >= 0x1EC70 && cp <= 0x1EEFF);
    return *arabic || (cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFB4F) ||
           (cp >= 0x10800 && cp <= 0x10FFF) || (cp >= 0x1E800 && cp <= 0x1EFFF);
}

uint8_t bidiClass(uint32_t cp) {
    if (cp < 0x80) {
        if (cp >= '0' && cp <= '9')
            return BEN;
        if ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z')
            return BL;
        switch (cp) {
        case ' ':
            return BWS;
        case '\t':
        case 0x0B:
        case 0x1F:
            return BS;
        case '\n':
        case '\r':
        case 0x1C:
        case 0x1D:
        case 0x1E:
            return BB;
        case '+':
        case '-':
            return BES;
        case '#':
        case '$':
        case '%':
            return BET;
        case ',':
        case '.':
        case '/':
        case ':':
            return BCS;
        default:
            return cp < 0x20 || cp == 0x7F ? BBN : BON;
        }
    }
    switch (cp) {
    case 0x200E:
        return BL;
    case 0x200F:
        return BR;
    case 0x061C:
        return BAL;
    case 0x00A0:
        return BCS;
    case 0x00B0:
    case 0x00B1:
        return BET;
    default:
        break;
    }
    const int gc = category(cp);
    switch (gc) {
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
        return BNSM;
    case HB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER:
        return (cp >= 0x0660 && cp <= 0x0669) ? BAN : BEN;
    case HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR:
    case HB_UNICODE_GENERAL_CATEGORY_LINE_SEPARATOR:
        return BWS;
    case HB_UNICODE_GENERAL_CATEGORY_PARAGRAPH_SEPARATOR:
        return BB;
    case HB_UNICODE_GENERAL_CATEGORY_CONTROL:
        return cp == 0x85 ? BB : BBN;
    case HB_UNICODE_GENERAL_CATEGORY_FORMAT:
        return BBN;
    case HB_UNICODE_GENERAL_CATEGORY_CURRENCY_SYMBOL:
        return BET;
    default:
        break;
    }
    bool arabic;
    if (strongRtlBlock(cp, &arabic)) {
        if (cp == 0x060C || cp == 0x066B || cp == 0x066C)
            return cp == 0x060C ? BCS : BAN; // Arabic comma / decimal+thousands separators
        return arabic ? BAL : BR;
    }
    switch (gc) {
    case HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_LETTER_NUMBER:
    case HB_UNICODE_GENERAL_CATEGORY_PRIVATE_USE:
        return BL;
    default:
        return BON;
    }
}

bool isNeutral(uint8_t t) {
    return t == BB || t == BS || t == BWS || t == BON || t == BBN;
}

} // namespace

int category(uint32_t cp) {
    return hb_unicode_general_category(ufuncs(), cp);
}
uint32_t script(uint32_t cp) {
    return hb_unicode_script(ufuncs(), cp);
}

bool isExtPict(uint32_t cp) {
    return cp >= 0xA9 && inRanges(kPict, cp);
}
bool isEmojiDefault(uint32_t cp) {
    return cp >= 0x231A && inRanges(kEmojiDefault, cp);
}
bool isRegional(uint32_t cp) {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

bool isBreakSpace(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x2006) ||
           (cp >= 0x2008 && cp <= 0x200A) || cp == 0x205F || cp == 0x3000;
}

bool isNewline(uint32_t cp) {
    return cp == '\n' || cp == '\r' || cp == 0x0B || cp == 0x0C || cp == 0x85 || cp == 0x2028 ||
           cp == 0x2029;
}

bool isLetter(uint32_t cp) {
    switch (category(cp)) {
    case HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER:
        return true;
    default:
        return false;
    }
}

bool isSelectWordChar(uint32_t cp) {
    if (cp < 0x80)
        return cp == '_' || (cp >= '0' && cp <= '9') || ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z');
    switch (category(cp)) {
    case HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER:
    case HB_UNICODE_GENERAL_CATEGORY_LETTER_NUMBER:
    case HB_UNICODE_GENERAL_CATEGORY_OTHER_NUMBER:
        return true;
    default:
        return cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F); // keep joined clusters whole
    }
}

bool GraphemeScanner::next(uint32_t cp) {
    const int c = gcbClass(cp);
    bool      brk;
    const int p = _prev;
    if (p < 0)
        brk = true;
    else if (p == GCR && c == GLF)
        brk = false;
    else if (p == GCR || p == GLF || p == GControl || c == GCR || c == GLF || c == GControl)
        brk = true;
    else if (p == GL && (c == GL || c == GV || c == GLV || c == GLVT))
        brk = false;
    else if ((p == GLV || p == GV) && (c == GV || c == GT))
        brk = false;
    else if ((p == GLVT || p == GT) && c == GT)
        brk = false;
    else if (c == GExtend || c == GZWJ || c == GSpacing)
        brk = false;
    else if (p == GZWJ && c == GPict && _pictZwj)
        brk = false;
    else if (p == GRI && c == GRI && (_riCount & 1))
        brk = false;
    else
        brk = true;

    _pictZwj = c == GZWJ && _inPict;
    _inPict  = c == GPict || (c == GExtend && _inPict);
    _riCount = c == GRI ? _riCount + 1 : 0;
    _prev    = c;
    return brk;
}

bool isIdeographic(uint32_t cp) {
    return cp >= 0x1100 && (inRanges(kIdeo, cp) || isEmojiDefault(cp));
}

Break breakBetween(uint32_t a, uint32_t b) {
    if (isNewline(a))
        return (a == '\r' && b == '\n') ? Break::None : Break::Mandatory;
    if (isNewline(b))
        return Break::None;
    // Glue: NBSP, narrow NBSP, word joiner, BOM, figure space.
    auto glue = [](uint32_t c) {
        return c == 0xA0 || c == 0x202F || c == 0x2060 || c == 0xFEFF || c == 0x2007;
    };
    if (glue(a) || glue(b) || isBreakSpace(b))
        return Break::None;
    if (a == 0x200B || isBreakSpace(a))
        return Break::Allowed;
    if (inRanges(kNoBreakBefore, b) || inRanges(kNoBreakAfter, a))
        return Break::None;
    if (a == '-' || a == 0x2010 || a == 0x2012 || a == 0x2013) {
        const int gc = category(b);
        if (gc >= HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER &&
            gc <= HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER)
            return Break::Allowed;
    }
    if (isIdeographic(a) || isIdeographic(b))
        return Break::Allowed;
    return Break::None;
}

uint8_t resolveBidi(const uint32_t *cps, size_t n, uint8_t *levels, uint8_t *t) {
    bool    anyRtl = false;
    uint8_t para   = 0xff;
    for (size_t i = 0; i < n; ++i) {
        t[i] = bidiClass(cps[i]);
        if (t[i] == BR || t[i] == BAL || t[i] == BAN)
            anyRtl = true;
        if (para == 0xff && (t[i] == BL || t[i] == BR || t[i] == BAL))
            para = t[i] == BL ? 0 : 1;
    }
    if (para == 0xff)
        para = 0;
    if (!anyRtl && para == 0) { // the common case: all left-to-right
        for (size_t i = 0; i < n; ++i)
            levels[i] = 0;
        return 0;
    }
    const uint8_t sos = para ? BR : BL;
    // W1: NSM takes the type of the previous character.
    for (size_t i = 0; i < n; ++i)
        if (t[i] == BNSM)
            t[i] = i ? t[i - 1] : sos;
    // W2 + W3 + W7 need the last strong type; W2 first.
    uint8_t strong = sos;
    for (size_t i = 0; i < n; ++i) {
        if (t[i] == BL || t[i] == BR || t[i] == BAL)
            strong = t[i];
        else if (t[i] == BEN && strong == BAL)
            t[i] = BAN;
    }
    for (size_t i = 0; i < n; ++i)
        if (t[i] == BAL)
            t[i] = BR;
    // W4: a single separator between two numbers of the same kind.
    for (size_t i = 1; i + 1 < n; ++i) {
        if (t[i] == BES && t[i - 1] == BEN && t[i + 1] == BEN)
            t[i] = BEN;
        else if (t[i] == BCS && t[i - 1] == t[i + 1] && (t[i - 1] == BEN || t[i - 1] == BAN))
            t[i] = t[i - 1];
    }
    // W5: terminators next to European numbers become numbers.
    for (size_t i = 0; i < n; ++i) {
        if (t[i] != BET)
            continue;
        size_t j = i;
        while (j < n && t[j] == BET)
            ++j;
        const bool en = (i > 0 && t[i - 1] == BEN) || (j < n && t[j] == BEN);
        for (size_t k = i; k < j; ++k)
            t[k] = en ? BEN : BON; // W6 for the rest
        i = j - 1;
    }
    // W6: remaining separators are neutral. W7: EN after L is L.
    strong = sos;
    for (size_t i = 0; i < n; ++i) {
        if (t[i] == BES || t[i] == BCS)
            t[i] = BON;
        if (t[i] == BL || t[i] == BR)
            strong = t[i];
        else if (t[i] == BEN && strong == BL)
            t[i] = BL;
    }
    // N1/N2: neutrals between the same direction take it, else the paragraph's.
    for (size_t i = 0; i < n; ++i) {
        if (!isNeutral(t[i]))
            continue;
        size_t j = i;
        while (j < n && isNeutral(t[j]))
            ++j;
        auto dir = [](uint8_t x) -> uint8_t { return x == BL ? BL : BR; }; // EN/AN count as R
        const uint8_t before = i ? dir(t[i - 1]) : uint8_t(sos);
        const uint8_t after  = j < n ? dir(t[j]) : uint8_t(sos);
        const uint8_t res    = before == after ? before : sos;
        for (size_t k = i; k < j; ++k)
            t[k] = res;
        i = j - 1;
    }
    // I1/I2 on top of the paragraph level.
    for (size_t i = 0; i < n; ++i) {
        if (para == 0)
            levels[i] = t[i] == BR ? 1 : (t[i] == BAN || t[i] == BEN) ? 2 : 0;
        else
            levels[i] = t[i] == BR ? 1 : 2;
    }
    // L1: trailing whitespace and separators go back to the paragraph level.
    for (size_t i = n; i-- > 0;) {
        const uint8_t c = bidiClass(cps[i]);
        if (c == BWS || c == BS || c == BB || c == BBN)
            levels[i] = para;
        else
            break;
    }
    for (size_t i = 0; i < n; ++i)
        if (cps[i] == '\t')
            levels[i] = para;
    return para;
}

} // namespace text::uni
