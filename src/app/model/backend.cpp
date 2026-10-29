#include "app/model/backend.h"

#include "base/str.h"

#include <algorithm>

namespace model {

// The defaults of the calls a backend may lack (see the header).

void Backend::edit(ConvRef, Ts, std::string) {}
void Backend::remove(ConvRef, Ts) {}
void Backend::react(ConvRef, Ts, std::string_view, bool) {}
void Backend::markUnread(ConvRef, Ts) {}
void Backend::setStarred(ConvRef, bool) {}
void Backend::setMuted(ConvRef, bool) {}
void Backend::setNotifyLevel(ConvRef, NotifyLevel) {}
void Backend::leave(ConvRef) {}

void Backend::openDm(UserRef, std::function<void(ConvRef)> done) {
    if (done)
        done(kNoConv);
}

void Backend::setPinned(ConvRef, Ts, bool) {}
void Backend::setSaved(ConvRef, Ts, bool) {}
void Backend::deleteFile(ConvRef, Ts, const std::string &) {}
void Backend::setReminder(ConvRef, Ts, int64_t) {}
void Backend::userTyping(ConvRef, Ts) {}

// A reply sent to the channel too sits in the channel's list without its
// thread (mapPage drops threadTs there): only the service knows its root.
void Backend::resolveThreadRoot(ConvRef conv, Ts ts, std::function<void(Ts)> done) {
    if (const Message *m = _store.findMessage(conv, ts); m && m->subtype() != "thread_broadcast") {
        if (done)
            done(m->threadTs ? m->threadTs : m->ts);
        return;
    }
    loadMessage(conv, ts, [done = std::move(done)](bool ok, Message m) {
        if (done)
            done(!ok ? 0 : m.threadTs ? m.threadTs : m.ts);
    });
}

std::string Backend::selfName() const {
    return std::string(_store.user(_store.me).label());
}

void Backend::search(std::string, std::function<void(std::vector<SearchHit>)> done) {
    if (done)
        done({});
}

// ── Sidebar sections, kept in this client ──────────────────────────────────

void Backend::createSection(std::string name, std::string emoji, ConvRef first) {
    std::vector<Store::Section> all = _store.sections();
    std::string                 id;
    for (int64_t n = int64_t(all.size()) + 1; id.empty() || _store.findSection(id); ++n)
        id = str::concat({"local-", str::number(base::nowSecs()), "-", str::number(n)});
    all.insert(all.begin(), Store::Section{id, std::move(name), std::move(emoji), {}});
    _store.setSections(std::move(all));
    sectionsChanged();
    if (first != kNoConv)
        moveToSection(first, id);
}

void Backend::updateSection(const std::string &id, std::string name, std::string emoji) {
    std::vector<Store::Section> all = _store.sections();
    for (Store::Section &s : all)
        if (s.id == id) {
            s.name  = std::move(name);
            s.emoji = std::move(emoji);
        }
    _store.setSections(std::move(all));
    sectionsChanged();
}

void Backend::deleteSection(const std::string &id) {
    std::vector<Store::Section> all = _store.sections();
    std::erase_if(all, [&](const Store::Section &s) { return s.id == id; });
    _store.setSections(std::move(all));
    sectionsChanged();
}

void Backend::moveToSection(ConvRef conv, const std::string &id) {
    if (conv >= _store.conversationCount())
        return;
    const std::string           convId = _store.conversation(conv).id;
    std::vector<Store::Section> all    = _store.sections();
    for (Store::Section &s : all) {
        std::erase(s.convs, convId);
        if (s.id == id)
            s.convs.push_back(convId);
    }
    _store.setSections(std::move(all));
    sectionsChanged();
    if (!id.empty() && _store.conversation(conv).starred)
        setStarred(conv, false);
}

void Backend::moveSection(const std::string &id, int step) {
    std::vector<Store::Section> all = _store.sections();
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].id == id) {
            const size_t to =
                size_t(std::clamp<int64_t>(int64_t(i) + step, 0, int64_t(all.size()) - 1));
            std::swap(all[i], all[to]);
            break;
        }
    _store.setSections(std::move(all));
    sectionsChanged();
}

void Backend::leaveSection(ConvRef conv) {
    if (conv < _store.conversationCount() && _store.sectionOf(_store.conversation(conv).id))
        moveToSection(conv, {});
}

} // namespace model
