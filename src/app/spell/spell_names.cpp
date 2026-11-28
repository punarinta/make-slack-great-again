// Language names for the Settings language grid: with a territory, the language's own name for
// itself, capitalised ("Español", "Русский"), regional where CLDR has a name of its own for the
// variant ("American English"); without one, its English name
// ("German"); then the code. A table rather than a locale
// library: the binary carries no CLDR, and a dictionary's code is all the
// OS gives on Linux.
#include "app/spell/spell.h"
#include "app/spell/spell_internal.h"

#include "base/str.h"

namespace spell {

namespace {

struct Name {
    const char *code, *name;
};

// ISO 639-1 (and the few 639-2/3 codes dictionaries use) → the English and
// the native name.
constexpr struct {
    const char *code, *english, *native;
} kLanguages[] = {
    {"af", "Afrikaans", "Afrikaans"},
    {"an", "Aragonese", "Aragonés"},
    {"ar", "Arabic", "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9"},
    {"as",
     "Assamese",
     "\xE0\xA6\x85\xE0\xA6\xB8\xE0\xA6\xAE\xE0\xA7\x80\xE0\xA6\xAF\xE0\xA6\xBC\xE0\xA6\xBE"},
    {"ast", "Asturian", "Asturianu"},
    {"az", "Azerbaijani", "Az\xC9\x99rbaycan"},
    {"be",
     "Belarusian",
     "\xD0\x91\xD0\xB5\xD0\xBB\xD0\xB0\xD1\x80\xD1\x83\xD1\x81\xD0\xBA\xD0\xB0\xD1\x8F"},
    {"bg", "Bulgarian", "\xD0\x91\xD1\x8A\xD0\xBB\xD0\xB3\xD0\xB0\xD1\x80\xD1\x81\xD0\xBA\xD0\xB8"},
    {"bn", "Bangla", "\xE0\xA6\xAC\xE0\xA6\xBE\xE0\xA6\x82\xE0\xA6\xB2\xE0\xA6\xBE"},
    {"bo",
     "Tibetan",
     "\xE0\xBD\x96\xE0\xBD\xBC\xE0\xBD\x91\xE0\xBC\x8B\xE0\xBD\xA6\xE0\xBE\x90\xE0\xBD\x91\xE0\xBC"
     "\x8B"},
    {"br", "Breton", "Brezhoneg"},
    {"bs", "Bosnian", "Bosanski"},
    {"ca", "Catalan", "Catal\xC3\xA0"},
    {"cs",
     "Czech",
     "\xC4\x8C"
     "e\xC5\xA1tina"},
    {"cy", "Welsh", "Cymraeg"},
    {"da", "Danish", "Dansk"},
    {"de", "German", "Deutsch"},
    {"el", "Greek", "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC"},
    {"en", "English", "English"},
    {"eo", "Esperanto", "Esperanto"},
    {"es", "Spanish", "Espa\xC3\xB1ol"},
    {"et", "Estonian", "Eesti"},
    {"eu", "Basque", "Euskara"},
    {"fa", "Persian", "\xD9\x81\xD8\xA7\xD8\xB1\xD8\xB3\xDB\x8C"},
    {"fi", "Finnish", "Suomi"},
    {"fo", "Faroese", "F\xC3\xB8royskt"},
    {"fr",
     "French",
     "Fran\xC3\xA7"
     "ais"},
    {"fy", "Western Frisian", "Frysk"},
    {"ga", "Irish", "Gaeilge"},
    {"gd", "Scottish Gaelic", "G\xC3\xA0idhlig"},
    {"gl", "Galician", "Galego"},
    {"gu",
     "Gujarati",
     "\xE0\xAA\x97\xE0\xAB\x81\xE0\xAA\x9C\xE0\xAA\xB0\xE0\xAA\xBE\xE0\xAA\xA4\xE0\xAB\x80"},
    {"gug",
     "Guarani",
     "Ava\xC3\xB1"
     "e\xE2\x80\x99\xE1\xBA\xBD"},
    {"he", "Hebrew", "\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA"},
    {"hi", "Hindi", "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80"},
    {"hr", "Croatian", "Hrvatski"},
    {"hu", "Hungarian", "Magyar"},
    {"hy", "Armenian", "\xD5\x80\xD5\xA1\xD5\xB5\xD5\xA5\xD6\x80\xD5\xA5\xD5\xB6"},
    {"id", "Indonesian", "Indonesia"},
    {"is", "Icelandic", "\xC3\x8Dslenska"},
    {"it", "Italian", "Italiano"},
    {"ja", "Japanese", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"},
    {"ka",
     "Georgian",
     "\xE1\x83\xA5\xE1\x83\x90\xE1\x83\xA0\xE1\x83\x97\xE1\x83\xA3\xE1\x83\x9A\xE1\x83\x98"},
    {"kk", "Kazakh", "\xD2\x9A\xD0\xB0\xD0\xB7\xD0\xB0\xD2\x9B \xD1\x82\xD1\x96\xD0\xBB\xD1\x96"},
    {"km", "Khmer", "\xE1\x9E\x81\xE1\x9F\x92\xE1\x9E\x98\xE1\x9F\x82\xE1\x9E\x9A"},
    {"kn", "Kannada", "\xE0\xB2\x95\xE0\xB2\xA8\xE0\xB3\x8D\xE0\xB2\xA8\xE0\xB2\xA1"},
    {"ko", "Korean", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4"},
    {"ku", "Kurdish", "Kurd\xC3\xAE"},
    {"la", "Latin", "Latina"},
    {"lb",
     "Luxembourgish",
     "L\xC3\xAB"
     "tzebuergesch"},
    {"lo", "Lao", "\xE0\xBA\xA5\xE0\xBA\xB2\xE0\xBA\xA7"},
    {"lt", "Lithuanian", "Lietuvi\xC5\xB3"},
    {"lv", "Latvian", "Latvie\xC5\xA1u"},
    {"mk",
     "Macedonian",
     "\xD0\x9C\xD0\xB0\xD0\xBA\xD0\xB5\xD0\xB4\xD0\xBE\xD0\xBD\xD1\x81\xD0\xBA\xD0\xB8"},
    {"ml", "Malayalam", "\xE0\xB4\xAE\xE0\xB4\xB2\xE0\xB4\xAF\xE0\xB4\xBE\xE0\xB4\xB3\xE0\xB4\x82"},
    {"mn", "Mongolian", "\xD0\x9C\xD0\xBE\xD0\xBD\xD0\xB3\xD0\xBE\xD0\xBB"},
    {"mr", "Marathi", "\xE0\xA4\xAE\xE0\xA4\xB0\xE0\xA4\xBE\xE0\xA4\xA0\xE0\xA5\x80"},
    {"ms", "Malay", "Melayu"},
    {"nb", "Norwegian Bokmal", "Norsk bokm\xC3\xA5l"},
    {"ne", "Nepali", "\xE0\xA4\xA8\xE0\xA5\x87\xE0\xA4\xAA\xE0\xA4\xBE\xE0\xA4\xB2\xE0\xA5\x80"},
    {"nl", "Dutch", "Nederlands"},
    {"nn", "Norwegian Nynorsk", "Norsk nynorsk"},
    {"oc", "Occitan", "Occitan"},
    {"or", "Odia", "\xE0\xAC\x93\xE0\xAC\xA1\xE0\xAC\xBC\xE0\xAC\xBF\xE0\xAC\x86"},
    {"pa", "Punjabi", "\xE0\xA8\xAA\xE0\xA9\xB0\xE0\xA8\x9C\xE0\xA8\xBE\xE0\xA8\xAC\xE0\xA9\x80"},
    {"pl", "Polish", "Polski"},
    {"pt", "Portuguese", "Portugu\xC3\xAAs"},
    {"ro", "Romanian", "Rom\xC3\xA2n\xC4\x83"},
    {"ru", "Russian", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"},
    {"si", "Sinhala", "\xE0\xB7\x83\xE0\xB7\x92\xE0\xB6\x82\xE0\xB7\x84\xE0\xB6\xBD"},
    {"sk",
     "Slovak",
     "Sloven\xC4\x8D"
     "ina"},
    {"sl",
     "Slovenian",
     "Sloven\xC5\xA1\xC4\x8D"
     "ina"},
    {"sq", "Albanian", "Shqip"},
    {"sr", "Serbian", "\xD0\xA1\xD1\x80\xD0\xBF\xD1\x81\xD0\xBA\xD0\xB8"},
    {"sv", "Swedish", "Svenska"},
    {"sw", "Swahili", "Kiswahili"},
    {"ta", "Tamil", "\xE0\xAE\xA4\xE0\xAE\xAE\xE0\xAE\xBF\xE0\xAE\xB4\xE0\xAF\x8D"},
    {"te", "Telugu", "\xE0\xB0\xA4\xE0\xB1\x86\xE0\xB0\xB2\xE0\xB1\x81\xE0\xB0\x97\xE0\xB1\x81"},
    {"th", "Thai", "\xE0\xB9\x84\xE0\xB8\x97\xE0\xB8\xA2"},
    {"tr",
     "Turkish",
     "T\xC3\xBCrk\xC3\xA7"
     "e"},
    {"uk",
     "Ukrainian",
     "\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0"},
    {"ur", "Urdu", "\xD8\xA7\xD8\xB1\xD8\xAF\xD9\x88"},
    {"uz", "Uzbek", "O\xE2\x80\x98zbek"},
    {"vi", "Vietnamese", "Ti\xE1\xBA\xBFng Vi\xE1\xBB\x87t"},
    {"zh", "Chinese", "\xE4\xB8\xAD\xE6\x96\x87"},
    {"zu", "Zulu", "IsiZulu"},
};

// language_TERRITORY → the variant's own name, where it has one.
constexpr Name kVariants[] = {
    {"en_US", "American English"},
    {"en_GB", "British English"},
    {"en_AU", "Australian English"},
    {"en_CA", "Canadian English"},
    {"de_AT", "\xC3\x96sterreichisches Deutsch"},
    {"de_CH", "Schweizer Hochdeutsch"},
    {"es_ES",
     "Espa\xC3\xB1ol de Espa\xC3\xB1"
     "a"},
    {"es_MX", "Espa\xC3\xB1ol de M\xC3\xA9xico"},
    {"fr_CA",
     "Fran\xC3\xA7"
     "ais canadien"},
    {"fr_CH",
     "Fran\xC3\xA7"
     "ais suisse"},
    {"nl_BE", "Vlaams"},
    {"pt_PT", "Portugu\xC3\xAAs europeu"},
};

bool asciiLetters(std::string_view s) {
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            return false;
    return !s.empty();
}

// The table's row for a bare language code; null when it has none.
const auto *rowOf(std::string_view code) {
    for (const auto &l : kLanguages)
        if (code == l.code)
            return &l;
    return static_cast<decltype(&kLanguages[0])>(nullptr);
}

} // namespace

const char *englishName(std::string_view code) {
    const auto *l = rowOf(code);
    return l ? l->english : nullptr;
}

const char *nativeName(std::string_view code) {
    const auto *l = rowOf(code);
    return l ? l->native : nullptr;
}

std::string languageName(std::string_view code) {
    // "de_DE_frami", "en-US", "pt_BR": language, territory, the rest.
    std::string norm(code);
    for (char &c : norm)
        if (c == '-')
            c = '_';
    const size_t           us   = norm.find('_');
    const std::string_view lang = std::string_view(norm).substr(0, us);
    std::string_view       terr;
    if (us != std::string::npos) {
        const std::string_view rest = std::string_view(norm).substr(us + 1);
        terr                        = rest.substr(0, rest.find('_'));
        if (terr.size() != 2 || !asciiLetters(terr))
            terr = {};
    }
    const char *name = nullptr;
    if (!terr.empty()) {
        const std::string lt = str::concat({lang, "_", str::asciiUpper(terr)});
        for (const Name &v : kVariants)
            if (lt == v.code)
                name = v.name;
    }
    // Without a territory the language itself is named, in English: a bare
    // "en" is no one's regional English.
    if (!name)
        if (const auto *l = rowOf(lang))
            name = terr.empty() ? l->english : l->native;
    if (!name)
        return std::string(code);
    return str::concat({name, " (", code, ")"});
}

namespace detail {

std::string localeCode(std::string_view os) {
    // "en_US.UTF-8@euro", "en-US", "zh-Hans-CN" → "en_US" / "zh_CN".
    os = os.substr(0, os.find_first_of(".@"));
    std::string lang, terr;
    size_t      i = 0, part = 0;
    while (i <= os.size()) {
        const size_t           j   = std::min(os.find_first_of("_-", i), os.size());
        const std::string_view seg = os.substr(i, j - i);
        if (part == 0)
            lang = str::asciiLower(seg);
        else if (seg.size() == 2 && asciiLetters(seg) && terr.empty())
            terr = str::asciiUpper(seg);
        ++part;
        i = j + 1;
    }
    if (lang.empty() || lang == "c" || lang == "posix" || !asciiLetters(lang))
        return {};
    return terr.empty() ? lang : str::concat({lang, "_", terr});
}

} // namespace detail

} // namespace spell
