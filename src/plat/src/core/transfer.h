// Clipboard and drag-and-drop helpers every backend shares: which MIME names
// mean UTF-8 text, text/uri-list parsing, and percent / file:// URI coding.
// Pure string code, no OS calls; the backends add only what is theirs (the
// Win32 drive-letter and UNC forms, the X atoms, the pasteboard types).
#pragma once

#include "plat/plat.h"
#include "prim/str.h"

#include <string>
#include <string_view>
#include <vector>

namespace plat::core {

// The one name plat's API uses for text.
inline constexpr const char *kTextMime = "text/plain;charset=utf-8";

// True for every name peers use for the UTF-8 (or, X11 STRING, Latin-1) text
// plat can read and write: "text/plain" bare or with a UTF-8 charset (any
// case, "utf8" too, spaces around ';' allowed), and the X selection targets
// UTF8_STRING, STRING and TEXT. Another charset is not text: no backend
// transcodes it, so asking for it would hand the app the wrong bytes.
bool isTextMime(std::string_view mime);

// RFC 2483 text/uri-list: one URI per line (CRLF or LF), '#' lines are
// comments, and trailing CR, NUL (GTK terminates some lists) and spaces are
// dropped. Empty lines are skipped.
std::vector<std::string> parseUriList(std::string_view list);

// Percent-encodes every byte except the RFC 3986 unreserved characters and
// those in `keep` (e.g. "/" for a path), as upper-case %XX. prim's, shared
// with the app's base library, as is percentDecode.
using prim::percentEncode;
// Decodes %XX escapes (either case); a malformed escape stays literal.
using prim::percentDecode;

// The action a drop gets when the user asked for none in particular:
// Copy, else Move, else Link, out of the DropActions the source allows.
DropAction preferredAction(uint32_t allowed);

// file:// URI for an absolute POSIX path, and back. pathFromFileUri accepts
// file:///p and file://localhost/p; "" for another host, another scheme, or
// an escaped NUL.
std::string fileUri(std::string_view absPath);
std::string pathFromFileUri(std::string_view uri);

} // namespace plat::core
