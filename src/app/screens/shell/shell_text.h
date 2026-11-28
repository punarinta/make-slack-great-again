// Small text helpers the shell's views share: how a conversation is titled
// and which glyph stands for it, and the one-line elided layout.
#pragma once

#include "gfx/icons_generated.h"
#include "model/store.h"
#include "text/text.h"

#include <memory>
#include <string>
#include <string_view>

namespace ui {
class Label;
}

namespace shell {

// "#general" for a channel, the name alone for a DM or group DM.
std::string convTitle(const model::Store &store, model::ConvRef conv);
// A DM or group DM's speech bubble, a private channel's lock, else the #.
gfx::Icon   convIcon(const model::Conversation &c);

// `t` on one line at most `maxWidth` wide, an ellipsis where it is cut.
std::unique_ptr<text::Layout>
oneLineLayout(const text::AttributedText &t, float maxWidth, float scale);
std::unique_ptr<text::Layout>
oneLineLayout(std::string_view s, const text::Style &st, float maxWidth, float scale);

// The label's text, all in one style.
void setStyledText(ui::Label *label, std::string_view s, const text::Style &st);

} // namespace shell
