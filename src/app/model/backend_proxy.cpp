#include "app/model/backend_proxy.h"

namespace model {

void BackendProxy::connect(Done d) {
    _t->connect(std::move(d));
}
void BackendProxy::loadHistory(ConvRef c, Ts before, Done d) {
    _t->loadHistory(c, before, std::move(d));
}
void BackendProxy::loadThread(ConvRef c, Ts root, Done d) {
    _t->loadThread(c, root, std::move(d));
}
void BackendProxy::send(ConvRef c, std::string text, Ts thread, Done d) {
    _t->send(c, std::move(text), thread, std::move(d));
}
void BackendProxy::sendWithFiles(
    ConvRef c, std::string text, Ts thread, std::vector<std::string> files, Done d
) {
    _t->sendWithFiles(c, std::move(text), thread, std::move(files), std::move(d));
}
void BackendProxy::edit(ConvRef c, Ts ts, std::string text) {
    _t->edit(c, ts, std::move(text));
}
void BackendProxy::remove(ConvRef c, Ts ts) {
    _t->remove(c, ts);
}
void BackendProxy::react(ConvRef c, Ts ts, std::string_view name, bool add) {
    _t->react(c, ts, name, add);
}
void BackendProxy::markRead(ConvRef c, Ts ts) {
    _t->markRead(c, ts);
}
void BackendProxy::markUnread(ConvRef c, Ts ts) {
    _t->markUnread(c, ts);
}
void BackendProxy::setStarred(ConvRef c, bool on) {
    _t->setStarred(c, on);
}
void BackendProxy::setMuted(ConvRef c, bool on) {
    _t->setMuted(c, on);
}
void BackendProxy::setNotifyLevel(ConvRef c, NotifyLevel level) {
    _t->setNotifyLevel(c, level);
}
void BackendProxy::leave(ConvRef c) {
    _t->leave(c);
}
void BackendProxy::openDm(UserRef u, std::function<void(ConvRef)> done) {
    _t->openDm(u, std::move(done));
}

void BackendProxy::joinChannel(ConvRef c, ConvDone done) {
    _t->joinChannel(c, std::move(done));
}

void BackendProxy::createChannel(std::string name, bool isPrivate, ConvDone done) {
    _t->createChannel(std::move(name), isPrivate, std::move(done));
}
void BackendProxy::setPinned(ConvRef c, Ts ts, bool on) {
    _t->setPinned(c, ts, on);
}
void BackendProxy::setSaved(ConvRef c, Ts ts, bool on) {
    _t->setSaved(c, ts, on);
}
void BackendProxy::deleteFile(ConvRef c, Ts ts, const std::string &fileId) {
    _t->deleteFile(c, ts, fileId);
}

void BackendProxy::deleteAttachment(ConvRef c, Ts ts, int attachmentId, Done d) {
    _t->deleteAttachment(c, ts, attachmentId, std::move(d));
}
void BackendProxy::downloadFile(const std::string &url, std::string toPath, Done done) {
    _t->downloadFile(url, std::move(toPath), std::move(done));
}
void BackendProxy::setReminder(ConvRef c, Ts ts, int64_t due) {
    _t->setReminder(c, ts, due);
}
bool BackendProxy::isAgentSession(ConvRef c) const {
    return _t->isAgentSession(c);
}
bool BackendProxy::canStopSession(ConvRef c) const {
    return _t->canStopSession(c);
}
void BackendProxy::stopSession(ConvRef c) {
    _t->stopSession(c);
}
void BackendProxy::userTyping(ConvRef c, Ts thread) {
    _t->userTyping(c, thread);
}
Backend::Capabilities BackendProxy::capabilities() const {
    return _t->capabilities();
}
Backend::SelfPresence BackendProxy::selfPresence() const {
    return _t->selfPresence();
}
void BackendProxy::requestPresence(UserRef u) {
    _t->requestPresence(u);
}
void BackendProxy::setPresence(bool away, Done d) {
    _t->setPresence(away, std::move(d));
}
void BackendProxy::setPresenceMode(PresenceMode mode) {
    _t->setPresenceMode(mode);
}
void BackendProxy::setNamesMode(NamesMode mode) {
    _t->setNamesMode(mode);
}
Backend::PresenceLink BackendProxy::presenceLink() const {
    return _t->presenceLink();
}
void BackendProxy::noteUserActivity() {
    _t->noteUserActivity();
}

void BackendProxy::setWindowVisible(bool visible) {
    _t->setWindowVisible(visible);
}
void BackendProxy::setStatus(std::string emoji, std::string text, int64_t expiry, Done d) {
    _t->setStatus(std::move(emoji), std::move(text), expiry, std::move(d));
}
void BackendProxy::loadMyProfile(std::function<void(MyProfile)> done) {
    _t->loadMyProfile(std::move(done));
}
void BackendProxy::updateProfile(std::string name, std::string email, std::string phone, Done d) {
    _t->updateProfile(std::move(name), std::move(email), std::move(phone), std::move(d));
}
void BackendProxy::setPhoto(std::string path, Done d) {
    _t->setPhoto(std::move(path), std::move(d));
}
void BackendProxy::sendBroadcast(ConvRef c, std::string text, Ts thread, Done d) {
    _t->sendBroadcast(c, std::move(text), thread, std::move(d));
}
void BackendProxy::scheduleMessage(ConvRef c, std::string text, Ts thread, int64_t postAt, Done d) {
    _t->scheduleMessage(c, std::move(text), thread, postAt, std::move(d));
}
void BackendProxy::sendBlocks(
    ConvRef c, std::string text, std::string blocks, Ts thread, bool broadcast, Done d
) {
    _t->sendBlocks(c, std::move(text), std::move(blocks), thread, broadcast, std::move(d));
}
void BackendProxy::editBlocks(ConvRef c, Ts ts, std::string text, std::string blocks) {
    _t->editBlocks(c, ts, std::move(text), std::move(blocks));
}
void BackendProxy::scheduleBlocks(
    ConvRef c, std::string text, std::string blocks, Ts thread, int64_t postAt, Done d
) {
    _t->scheduleBlocks(c, std::move(text), std::move(blocks), thread, postAt, std::move(d));
}
void BackendProxy::refreshScheduled() {
    _t->refreshScheduled();
}
void BackendProxy::cancelScheduled(const std::string &id, Done d) {
    _t->cancelScheduled(id, std::move(d));
}
void BackendProxy::loadMembers(ConvRef c, MembersDone done) {
    _t->loadMembers(c, std::move(done));
}
bool BackendProxy::gifSearchAvailable() const {
    return _t->gifSearchAvailable();
}
void BackendProxy::searchGifs(std::string query, std::function<void(std::vector<Gif>)> done) {
    _t->searchGifs(std::move(query), std::move(done));
}
std::string BackendProxy::promptSuggestion(ConvRef c) const {
    return _t->promptSuggestion(c);
}
void BackendProxy::search(std::string query, std::function<void(std::vector<SearchHit>)> done) {
    _t->search(std::move(query), std::move(done));
}
void BackendProxy::loadMessage(ConvRef c, Ts ts, MessageDone done) {
    _t->loadMessage(c, ts, std::move(done));
}
void BackendProxy::postAgentReply(
    ConvRef c, Ts root, std::string text, std::string blocks, PostDone d
) {
    _t->postAgentReply(c, root, std::move(text), std::move(blocks), std::move(d));
}
void BackendProxy::editAgentReply(ConvRef c, Ts ts, std::string text, std::string blocks, Done d) {
    _t->editAgentReply(c, ts, std::move(text), std::move(blocks), std::move(d));
}
void BackendProxy::deleteAgentReply(ConvRef c, Ts ts, Done d) {
    _t->deleteAgentReply(c, ts, std::move(d));
}
void BackendProxy::postAgentFiles(
    ConvRef c, Ts root, std::string text, std::vector<std::string> files, PostDone d
) {
    _t->postAgentFiles(c, root, std::move(text), std::move(files), std::move(d));
}
void BackendProxy::loadThreadReplies(ConvRef c, Ts root, Ts after, RepliesDone done) {
    _t->loadThreadReplies(c, root, after, std::move(done));
}
void BackendProxy::watchThread(ConvRef c, Ts root, Ts after, bool busy) {
    _t->watchThread(c, root, after, busy);
}
void BackendProxy::unwatchThread(ConvRef c, Ts root) {
    _t->unwatchThread(c, root);
}

void BackendProxy::loadThreadsView(std::string cursor, ThreadsViewDone done) {
    _t->loadThreadsView(std::move(cursor), std::move(done));
}
void BackendProxy::markThreadRead(ConvRef c, Ts root, Ts ts) {
    _t->markThreadRead(c, root, ts);
}
int64_t BackendProxy::nowSecs() const {
    return _t->nowSecs();
}

} // namespace model

namespace model {
void BackendProxy::setActiveConversation(ConvRef c, Ts thread) {
    _t->setActiveConversation(c, thread);
}
std::vector<Backend::AgentRole> BackendProxy::agentRoles() const {
    return _t->agentRoles();
}
std::string BackendProxy::saveAgentRole(const AgentRole &role, std::string *error) {
    return _t->saveAgentRole(role, error);
}
void BackendProxy::removeAgentRole(const std::string &id) {
    _t->removeAgentRole(id);
}
void BackendProxy::restoreAgentRole(const std::string &id) {
    _t->restoreAgentRole(id);
}
std::string BackendProxy::agentSessionBlocker(const std::string &dir) const {
    return _t->agentSessionBlocker(dir);
}
void BackendProxy::startAgentSession(
    const std::string                                &dir,
    bool                                              skip,
    const std::string                                &role,
    std::function<void(ConvRef, const std::string &)> done
) {
    _t->startAgentSession(dir, skip, role, std::move(done));
}
std::string BackendProxy::agentSessionFolder(ConvRef c) const {
    return _t->agentSessionFolder(c);
}
std::string BackendProxy::agentSessionRole(ConvRef c) const {
    return _t->agentSessionRole(c);
}
void BackendProxy::findAgentSessions(std::function<void(std::vector<FoundSession>)> done) {
    _t->findAgentSessions(std::move(done));
}
ConvRef BackendProxy::addFoundSession(const std::string &id) {
    return _t->addFoundSession(id);
}
std::vector<Backend::Command> BackendProxy::commands(ConvRef c) {
    return _t->commands(c);
}
uint64_t BackendProxy::commandsRevision(ConvRef c) {
    return _t->commandsRevision(c);
}
Backend::LocalResult BackendProxy::runLocalCommand(
    ConvRef c, Ts thread, const std::string &name, const std::string &args
) {
    return _t->runLocalCommand(c, thread, name, args);
}
std::vector<std::string> BackendProxy::promptHistory(ConvRef c) {
    return _t->promptHistory(c);
}
std::vector<std::string> BackendProxy::folderPromptHistory(const std::string &dir) {
    return _t->folderPromptHistory(dir);
}
bool BackendProxy::threadAcceptsReplies(ConvRef c, Ts root) const {
    return _t->threadAcceptsReplies(c, root);
}
bool BackendProxy::threadOpensAsSession(ConvRef c, Ts root) const {
    return _t->threadOpensAsSession(c, root);
}
ConvRef BackendProxy::openThreadAsSession(ConvRef c, Ts root) {
    return _t->openThreadAsSession(c, root);
}
void BackendProxy::startAgentBranch(
    const std::string       &session,
    std::string              prompt,
    std::vector<std::string> files,
    std::vector<std::string> deniedTools,
    AgentBranchDone          started,
    AgentTurnFn              turn
) {
    _t->startAgentBranch(
        session,
        std::move(prompt),
        std::move(files),
        std::move(deniedTools),
        std::move(started),
        std::move(turn)
    );
}
void BackendProxy::continueAgentBranch(
    const std::string &branch, std::string prompt, std::vector<std::string> files, AgentTurnFn turn
) {
    _t->continueAgentBranch(branch, std::move(prompt), std::move(files), std::move(turn));
}
void BackendProxy::watchAgentBranch(const std::string &branch, AgentTurnFn turn) {
    _t->watchAgentBranch(branch, std::move(turn));
}
void BackendProxy::setAgentBranchLabel(const std::string &branch, std::string label) {
    _t->setAgentBranchLabel(branch, std::move(label));
}
void BackendProxy::agentSessionMovedOn(
    const std::string &session, const std::string &forkPoint, std::function<void(bool)> done
) {
    _t->agentSessionMovedOn(session, forkPoint, std::move(done));
}
bool BackendProxy::canDeleteMessage(ConvRef c, Ts ts) const {
    return _t->canDeleteMessage(c, ts);
}
void BackendProxy::pressButton(ConvRef c, Ts ts, const std::string &buttonId, Done d) {
    _t->pressButton(c, ts, buttonId, std::move(d));
}
void BackendProxy::setZenMode(bool on) {
    _t->setZenMode(on);
}
void BackendProxy::setLocalName(ConvRef c, std::string name) {
    _t->setLocalName(c, std::move(name));
}
void BackendProxy::createSection(std::string name, std::string emoji, ConvRef first) {
    _t->createSection(std::move(name), std::move(emoji), first);
}
void BackendProxy::updateSection(const std::string &id, std::string name, std::string emoji) {
    _t->updateSection(id, std::move(name), std::move(emoji));
}
void BackendProxy::deleteSection(const std::string &id) {
    _t->deleteSection(id);
}
void BackendProxy::moveToSection(ConvRef c, const std::string &id) {
    _t->moveToSection(c, id);
}
void BackendProxy::moveSection(const std::string &id, int step) {
    _t->moveSection(id, step);
}
void BackendProxy::loadChannelCanvas(ConvRef c, std::function<void(std::string)> done) {
    _t->loadChannelCanvas(c, std::move(done));
}
void BackendProxy::loadCanvasMeta(const std::string &fileId, CanvasMetaDone done) {
    _t->loadCanvasMeta(fileId, std::move(done));
}
void BackendProxy::loadCanvasContent(const std::string &fileId, CanvasHtmlDone done) {
    _t->loadCanvasContent(fileId, std::move(done));
}
void BackendProxy::createChannelCanvas(ConvRef c, std::string markdown, CanvasCreated done) {
    _t->createChannelCanvas(c, std::move(markdown), std::move(done));
}
void BackendProxy::editCanvas(
    const std::string &fileId, std::vector<CanvasChange> changes, Done d
) {
    _t->editCanvas(fileId, std::move(changes), std::move(d));
}
void BackendProxy::deleteCanvas(const std::string &fileId, Done d) {
    _t->deleteCanvas(fileId, std::move(d));
}
bool BackendProxy::threadFollowed(ConvRef c, Ts root) const {
    return _t->threadFollowed(c, root);
}
void BackendProxy::resolveUser(UserRef u) {
    _t->resolveUser(u);
}
void BackendProxy::resolveChannel(const std::string &id) {
    _t->resolveChannel(id);
}
void BackendProxy::loadSidebarTheme(std::function<void(SidebarTheme, std::string)> done) {
    _t->loadSidebarTheme(std::move(done));
}

} // namespace model
