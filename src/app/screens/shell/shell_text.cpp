#include "screens/shell/shell_text.h"

#include "base/str.h"
#include "ui/widgets.h"

namespace shell {

std::string convTitle(const model::Store &store, model::ConvRef conv) {
    return str::concat({store.conversation(conv).isDirect() ? "" : "#", store.displayName(conv)});
}

gfx::Icon convIcon(const model::Conversation &c) {
    return c.isDirect()                         ? gfx::Icon::MessageSquare
           : c.kind == model::ConvKind::Private ? gfx::Icon::Lock
                                                : gfx::Icon::Hash;
}

std::unique_ptr<text::Layout>
oneLineLayout(const text::AttributedText &t, float maxWidth, float scale) {
    text::LayoutOptions o;
    o.maxLines = 1;
    o.ellipsis = true;
    o.maxWidth = maxWidth;
    return text::Layout::build(t, o, scale);
}

std::unique_ptr<text::Layout>
oneLineLayout(std::string_view s, const text::Style &st, float maxWidth, float scale) {
    text::AttributedText t;
    t.append(s, st);
    return oneLineLayout(t, maxWidth, scale);
}

void setStyledText(ui::Label *label, std::string_view s, const text::Style &st) {
    text::AttributedText t;
    t.append(s, st);
    label->setRichText(std::move(t));
}

} // namespace shell
