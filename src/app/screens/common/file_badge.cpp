#include "app/screens/common/file_badge.h"

#include "base/file.h"
#include "base/str.h"

namespace screens {

namespace {

bool isCodeExt(std::string_view e) {
    static const char *const kCode[] = {"c",    "cpp", "h",     "hpp", "cs",  "css", "go",
                                        "java", "js",  "ts",    "tsx", "jsx", "py",  "rb",
                                        "rs",   "sh",  "swift", "kt",  "php", "sql", "diff"};
    for (const char *c : kCode)
        if (e == c)
            return true;
    return false;
}

gfx::Color typeColor(const model::File &f, bool code) {
    // The file type's colour.
    const std::string &m   = f.mime;
    auto               has = [&](const char *s) { return m.find(s) != std::string::npos; };
    if (code)
        return 0xffde4e2bU;
    if (has("pdf"))
        return 0xffe44d4dU;
    if (has("word") || has("document"))
        return 0xff2b579aU;
    if (has("excel") || has("spreadsheet"))
        return 0xff217346U;
    if (has("powerpoint") || has("presentation"))
        return 0xffd24726U;
    if (m.rfind("video/", 0) == 0)
        return 0xff7b2d8bU;
    if (m.rfind("audio/", 0) == 0)
        return 0xff1e7a6eU;
    if (has("zip") || has("x-tar") || has("gzip") || has("x-7z") || has("x-rar"))
        return 0xff8b6914U;
    if (m.rfind("text/", 0) == 0 || has("json") || has("xml"))
        return 0xff555555U;
    return 0xff888888U;
}

} // namespace

FileBadge fileBadge(const model::File &f) {
    FileBadge        b;
    std::string_view ext = file::extension(f.name);
    b.code               = isCodeExt(ext);
    b.color              = typeColor(f, b.code);
    if (ext.size() >= 1 && ext.size() <= 5)
        b.label = str::asciiUpper(ext.substr(0, 4));
    else if (!f.prettyType.empty())
        b.label = str::asciiUpper(std::string_view(f.prettyType).substr(0, 4));
    else
        b.label = "FILE";
    return b;
}

} // namespace screens
