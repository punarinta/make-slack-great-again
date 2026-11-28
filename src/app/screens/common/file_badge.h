// A file's type square: the colour of its kind and the short text in it
// (the extension, else the type, else "FILE"), or a code glyph for source
// files. The message file chip and the Files tab draw it.
#pragma once

#include "app/model/types.h"
#include "gfx/gfx.h"

#include <string>

namespace screens {

struct FileBadge {
    std::string label; // "PDF", "ZIP"; unused when code
    gfx::Color  color = 0;
    bool        code  = false; // a source file: the code glyph instead of the label
};

FileBadge fileBadge(const model::File &f);

} // namespace screens
