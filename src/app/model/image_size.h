// Image dimensions from a file's header (PNG, GIF, JPEG, WebP), no decoder
// needed: the message list sizes image cards with it before they load, and
// the demo fixture fills File records with it.
#pragma once

#include <cstdint>
#include <string>

namespace model {

// Width/height from an image file's header; false if not an image we know.
bool imageSize(const std::string &absPath, int32_t *w, int32_t *h);

} // namespace model
