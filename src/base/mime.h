// File types: the extension table, the short labels shown on file cards,
// and the magic bytes that say what a file really is.
#pragma once

#include <string_view>

namespace mime {

// The MIME type for a file name's (or path's) extension, any case; "" when
// it is not one of the known types.
std::string_view fromName(std::string_view name);
// fromName, else application/octet-stream.
std::string_view fromNameOr(std::string_view name);
// The extension for a MIME type, without the dot ("image/jpeg" -> "jpg");
// "" when it is not one of the known types.
std::string_view extension(std::string_view mime);
// Whether a file name's extension is one of plain text (prose, data, config,
// source code, logs), any case: a preview can show its first lines.
bool             isTextName(std::string_view name);
// What the leading bytes say (PNG, JPEG, GIF, WebP, PDF, an ID3 tag); "" for
// anything else.
std::string_view sniff(std::string_view head);
// The file card label for a MIME type ("PNG", "Plain text", "WebM"); "" for
// one without a label of its own.
std::string_view label(std::string_view mime);

} // namespace mime
