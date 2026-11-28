#include "app/claude/common.h"

#include "base/file.h"
#include "base/process.h"
#include "base/str.h"
#include "base/utf8.h"
#include "gfx/gfx.h"

namespace claude {

namespace {
Dirs gDirs;
} // namespace

void setDirs(Dirs d) {
    gDirs = std::move(d);
}

const Dirs &dirs() {
    return gDirs;
}

std::string homeRelative(std::string_view path) {
    const std::string home = base::homeDir();
    if (!home.empty() && path.substr(0, home.size()) == home &&
        (path.size() == home.size() || path[home.size()] == '/'))
        return "~" + std::string(path.substr(home.size()));
    std::string out(path);
#ifdef _WIN32
    for (char &c : out)
        if (c == '/')
            c = '\\';
#endif
    return out;
}

std::string ellipsized(std::string_view s, size_t max) {
    return utf8::ellipsize(s, max, max - 1);
}

bool LineReader::next(std::string_view *line) {
    constexpr size_t kChunk = 1 << 20;
    for (;;) {
        const size_t nl = _buf.find('\n', _pos);
        if (nl != std::string::npos) {
            *line = std::string_view(_buf).substr(_pos, nl - _pos);
            _pos  = nl + 1;
            return true;
        }
        if (_eof || _failed) {
            if (_pos >= _buf.size())
                return false;
            *line = std::string_view(_buf).substr(_pos); // the last, without its newline
            _pos  = _buf.size();
            return true;
        }
        _buf.erase(0, _pos);
        _pos = 0;
        if (!file::readRange(_path, _at, kChunk, &_chunk)) {
            _failed = true;
            _buf.clear();
            return false;
        }
        _at += int64_t(_chunk.size());
        _eof = _chunk.size() < kChunk;
        _buf += _chunk;
    }
}

bool showsAsPicture(std::string_view path, std::string_view mime) {
    if (mime == "image/svg+xml")
        return true;
    std::string head;
    return str::startsWith(mime, "image/") && file::readRange(path, 0, 64, &head) &&
           gfx::canDecodeImage(head);
}

} // namespace claude
