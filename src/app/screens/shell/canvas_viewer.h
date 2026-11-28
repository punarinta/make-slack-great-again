// The canvas viewer: a canvas shared as a message file (huddle
// notes, a canvas posted into a chat) opened over the window — the canvas
// page (readable and editable like a channel's canvas tab) in a panel on the
// viewer backdrop, under a small bar: "Canvas", Open in browser, Close.
// Escape, Close or a click on the backdrop dismisses it, saving edits first.
#pragma once

#include "screens/common/context.h"

#include <functional>
#include <string>

namespace shell {

ui::Popup *showCanvasViewer(
    screens::Context                             &ctx,
    ui::Window                                   &w,
    model::ConvRef                                conv,
    const model::File                            &canvas,
    std::function<void(const std::string &error)> onError
);

} // namespace shell
