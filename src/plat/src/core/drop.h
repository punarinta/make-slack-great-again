// Drop-target rules the backends share: which offered types a Drop reads,
// the modifier convention for the action, and the Event fields that mirror
// the dropped items. Pure code, no OS calls.
#pragma once

#include "plat/plat.h"

#include <string_view>

namespace plat::core {

// Drop data is read only for types that are cheap and meant for us: the
// standard ones and plat-style MIME types, not the BMP/TIFF a bitmap drag
// also offers or other image conversions (the others are listed without
// data).
bool readOnDrop(std::string_view mime);

// The usual modifier convention (Explorer, toolkits, file managers): Shift
// moves, Ctrl copies, both link — when `allowed` (DropActions) permits it;
// otherwise, and with neither held, preferredAction(allowed).
DropAction modifierDropAction(bool shift, bool ctrl, uint32_t allowed);

// e.uris from a text/uri-list item and e.text from a text item (the last
// of each wins), for a Drop whose items carry their data.
void fillDropText(Event &e);

} // namespace plat::core
