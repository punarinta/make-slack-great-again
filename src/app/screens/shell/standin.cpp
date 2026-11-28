#include "screens/shell/standin.h"

#include "app/screens/common/message_rules.h"
#include "app/screens/common/message_text.h"
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"

using namespace ui;
using i18n::tr;

namespace shell {

MessageStandIn::MessageStandIn(screens::Context &ctx, Avatars &avatars)
    : _ctx(ctx), _avatars(avatars) {
    setBackground(C::Surface);
    content()->style().padding(0, 12, 0, 12).spacing(2);
    _observer = ctx.store.observe(model::Store::kAnyConv, [this](const model::Change &ch) {
        if (ch.conv == _conv && ch.kind != model::ChangeKind::Typing &&
            ch.kind != model::ChangeKind::Meta && ch.kind != model::ChangeKind::ThreadRead)
            rebuild();
    });
}

MessageStandIn::~MessageStandIn() {
    _ctx.store.unobserve(_observer);
}

void MessageStandIn::setTarget(model::ConvRef conv, model::Ts thread) {
    _conv   = conv;
    _thread = thread;
    rebuild();
}

void MessageStandIn::rebuild() {
    View *list = content();
    list->clearChildren();
    if (_conv == model::kNoConv)
        return;
    const auto                         &store = _ctx.store();
    std::vector<const model::Message *> msgs;
    if (_thread) {
        if (const auto *root = store.findMessage(_conv, _thread))
            msgs.push_back(root);
        if (const auto *replies = store.replies(_conv, _thread))
            for (const auto &m : *replies)
                msgs.push_back(&m);
    } else {
        for (const auto &m : store.conversation(_conv).messages)
            msgs.push_back(&m);
    }
    const int64_t now = base::nowSecs();
    for (const model::Message *m : msgs) {
        auto *row = list->add<View>();
        row->style().row().padding(20, 6).spacing(10).items(Align::Start);
        auto *img = row->add<Avatar>();
        img->style().size(36, 36);
        img->setRadius(6);
        img->setBitmap(_avatars.get(screens::authorAvatar(store, *m), 72));
        auto *col = row->add<View>();
        col->style().flex(1).spacing(2);
        auto *head = col->add<View>();
        head->style().row().spacing(8).items(Align::Center);
        head->add<Label>(std::string(screens::authorName(store, *m)), Font::BodyBold);
        head->add<Label>(base::dateTimeLabel(model::tsSecs(m->ts), now), Font::Small, C::TextFaint);
        std::string body = screens::plainText(store, m->text);
        for (const auto &a : m->attachments())
            body += str::concat(
                {body.empty() ? "" : "\n", "\xE2\x96\x8E ", a.title.empty() ? a.link : a.title}
            );
        for (const auto &f : m->files())
            body += str::concat({body.empty() ? "" : "\n", "\xF0\x9F\x93\x8E ", f.name});
        if (m->pending)
            col->setEnabled(false);
        col->add<Label>(std::move(body), Font::Body, m->pending ? C::TextMuted : C::Text);
        if (!_thread && m->replyCount) {
            auto *replies = col->add<Button>(
                m->replyCount == 1 ? std::string(tr("1 reply"))
                                   : i18n::arg(tr("%1 replies"), str::number(m->replyCount)),
                Button::Kind::Ghost
            );
            replies->setTextColor(C::Link);
            replies->style().alignSelf(Align::Start);
            const model::Ts root = m->ts;
            replies->onClick     = [this, root] {
                if (_ctx.openThread)
                    _ctx.openThread(_conv, root);
            };
        }
    }
    // New messages: follow the end like a chat.
    scrollTo(1e9f);
}

} // namespace shell
