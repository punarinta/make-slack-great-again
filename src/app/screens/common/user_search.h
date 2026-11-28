// What a people search matches: a person's label (the display name, else
// the handle), their other name (full or display: whichever doesn't
// show) and handle, optionally the job title, folded once so each
// keystroke only compares (utf8::containsPrefolded).
#pragma once

#include "app/model/types.h"

#include <string>

namespace screens {

// "label[ other name] handle[ title]", folded.
std::string userSearchKey(const model::User &u, bool withTitle = false);

} // namespace screens
