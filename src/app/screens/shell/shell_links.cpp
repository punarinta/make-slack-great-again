// The shell's side of agent thread links (app/claude/links.h): what the
// message toolbar's robot and the message menu offer (Context::agentLink),
// what they do — the session picker, opening the agent's thread, "Allow
// <name> to ask agent", "Unlink agent" — and the thread panel's chip back to
// the Slack thread. Shell members kept apart from shell.cpp.
#include "app/claude/links.h"
#include "base/i18n.h"
#include "screens/shell/session_dialogs.h"
#include "screens/shell/shell.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/message_list.h"
#include "app/screens/messages/thread_panel.h"
#endif

#include <algorithm>

using i18n::tr;
using model::ConvRef;
using model::kNoConv;
using model::Ts;
using Action = screens::Context::AgentLinkAction;

namespace shell {

void Shell::setAgentLinks(claude::Links *links) {
    if (_links) {
        _links->onError   = nullptr;
        _links->onChanged = nullptr;
    }
    _links = links;
    if (!links) {
        _ctx.agentLink       = nullptr;
        _ctx.agentLinkAction = nullptr;
        _ctx.agentLinkSource = nullptr;
        agentLinksChanged();
        return;
    }
    links->onError       = [this](const std::string &message) { showError(message); };
    links->onChanged     = [this] { agentLinksChanged(); };
    _ctx.agentLink       = [this](ConvRef c, const model::Message &m) { return agentLink(c, m); };
    _ctx.agentLinkAction = [this](ConvRef c, Ts ts, Action a) { agentLinkAction(c, ts, a); };
    _ctx.agentLinkSource = [this](ConvRef c, Ts root) -> std::string {
        if (!_links || &_ctx.store() != _links->agentStore())
            return {};
        const claude::Links::Link *l  = _links->findBranch(c, root);
        const model::Store        *st = l ? _links->owStore(*l) : nullptr;
        const ConvRef              ow = l ? _links->owConv(*l) : kNoConv;
        if (!st || ow == kNoConv)
            return {};
        const model::Conversation &conv  = st->conversation(ow);
        const std::string          where = conv.isDirect() ? st->displayName(ow) : "#" + conv.name;
        return i18n::arg(tr("%1 in %2"), where, st->workspaceName);
    };
    agentLinksChanged();
}

// Offered on messages of the Slack workspaces while a Claude Code one can
// be asked; never in a Claude Code workspace itself.
screens::Context::AgentLink Shell::agentLink(ConvRef conv, const model::Message &m) const {
    screens::Context::AgentLink out;
    if (!_links || !_links->available() || _ctx.backend().capabilities().agentSessions ||
        &_ctx.store() == _links->agentStore())
        return out;
    const model::Store &st          = _ctx.store();
    out.offered                     = true;
    const Ts                   root = m.isReply() ? m.threadTs : m.ts;
    const claude::Links::Link *l    = _links->find(st, conv, root);
    if (!l)
        return out;
    out.linked              = true;
    out.root                = m.ts == root;
    const model::User &user = st.user(m.user);
    out.canAllow = m.user != model::kNoUser && m.user != st.me && !user.bot &&
                   std::find(l->askers.begin(), l->askers.end(), user.id) == l->askers.end();
    return out;
}

void Shell::agentLinkAction(ConvRef conv, Ts ts, Action a) {
    if (!_links)
        return;
    model::Store &st = _ctx.store();
    if (a == Action::OpenSource) {
        // From the agent's thread to the Slack thread it answers.
        const claude::Links::Link *l = _links->findBranch(conv, ts);
        if (!l)
            return;
        const std::string id = l->id;
        if (!showWorkspaceOf(_links->owStore(*l)))
            return;
        if (const claude::Links::Link *x = _links->byId(id)) {
            const ConvRef ow = _links->owConv(*x);
            if (ow == kNoConv)
                return;
            open(ow);
            if (ow == _current)
                openThread(ow, x->thread);
        }
        return;
    }
    const model::Message *m = st.findMessage(conv, ts);
    if (!m)
        return;
    const Ts                   root = m->isReply() ? m->threadTs : m->ts;
    const claude::Links::Link *l    = _links->find(st, conv, root);
    if (a == Action::Allow) {
        if (l)
            _links->allow(l->id, st.user(m->user).id);
        return;
    }
    if (a == Action::Unlink) {
        if (l)
            _links->unlink(l->id);
        return;
    }
    if (l) {
        // A linked thread: the agent's side of it, in the Claude Code
        // workspace (the session itself until it has a branch).
        const std::string id = l->id;
        if (!showWorkspaceOf(_links->agentStore()))
            return;
        if (const claude::Links::Link *x = _links->byId(id)) {
            const ConvRef c = _links->sessionConv(*x);
            if (c == kNoConv)
                return;
            open(c);
            if (const Ts r = _links->branchRoot(*x); r && c == _current)
                openThread(c, r);
        }
        return;
    }
    // Not linked yet: which session answers it.
    model::Backend *agents = _links->agents();
    if (!agents)
        return;
    model::Store *from = &st;
    showSessionPicker(
        _ctx, _win, _avatars, *agents, [this, from, conv, ts](const std::string &sessionId) {
            if (!_links || !_links->agents() || !_links->agentStore())
                return;
            // Listed in msga if it wasn't: the link needs it there.
            const ConvRef c = _links->agents()->addFoundSession(sessionId);
            if (c == kNoConv) {
                showError(tr("The session is gone."));
                return;
            }
            _links->link(*from, conv, ts, _links->agentStore()->conversation(c).id);
        }
    );
}

void Shell::agentLinksChanged() {
#ifdef MSGA_HAVE_MESSAGES
    if (_messages)
        static_cast<screens::MessageList *>(_messages)->refreshToolbar();
    if (auto *p = static_cast<screens::ThreadPanel *>(threadPanel())) {
        p->list().refreshToolbar();
        p->refreshLink();
    }
#endif
}

} // namespace shell
