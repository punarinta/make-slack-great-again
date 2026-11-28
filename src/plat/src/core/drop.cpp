#include "core/drop.h"
#include "core/transfer.h"

namespace plat::core {

bool readOnDrop(std::string_view m) {
    return m == kTextMime || m == "text/html" || m == "image/png" || m == "text/uri-list" ||
           (m.find('/') != std::string_view::npos && m.substr(0, 6) != "image/");
}

DropAction modifierDropAction(bool shift, bool ctrl, uint32_t allowed) {
    DropAction want = DropAction::None;
    uint32_t   bit  = 0;
    if (shift && ctrl)
        want = DropAction::Link, bit = ActLink;
    else if (shift)
        want = DropAction::Move, bit = ActMove;
    else if (ctrl)
        want = DropAction::Copy, bit = ActCopy;
    return (allowed & bit) ? want : preferredAction(allowed);
}

void fillDropText(Event &e) {
    for (const DataItem &i : e.items) {
        if (i.mime == "text/uri-list")
            e.uris = parseUriList(i.data);
        else if (isTextMime(i.mime))
            e.text = i.data;
    }
}

} // namespace plat::core
