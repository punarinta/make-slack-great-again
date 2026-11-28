// The small tag pill: App/Ext after a name, or Vip on the sidebar's VIP
// unreads header in place of its icon.
#pragma once

#include "ui/ui.h"

namespace screens {

enum class TagKind { App, Ext, Vip };
ui::View *addTagBadge(ui::View *parent, TagKind kind, bool sidebar = false);

} // namespace screens
