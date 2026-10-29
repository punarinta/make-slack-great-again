// BackendProxy — the Backend the screens hold (Context::backend is a
// reference), forwarding to whichever backend is current: none signed in
// (NullBackend), the demo, or a signed-in workspace. Signing in, out or
// switching workspaces swaps the target instead of rebuilding the screens.
#pragma once

#include "app/model/backend.h"

namespace model {

class BackendProxy final : public Backend {
public:
    BackendProxy(Store &store, Backend &initial) : Backend(store), _t(&initial) {}
    void     setTarget(Backend &b) { _t = &b; }
    Backend &target() const { return *_t; }

    void connect(Done d) override;
    void loadHistory(ConvRef c, Ts before, Done d) override;
    void loadThread(ConvRef c, Ts root, Done d) override;
    void setActiveConversation(ConvRef c, Ts thread) override;
    void send(ConvRef c, std::string text, Ts thread, Done d) override;
    void sendWithFiles(
        ConvRef c, std::string text, Ts thread, std::vector<std::string> files, Done d
    ) override;
    void edit(ConvRef c, Ts ts, std::string text) override;
    void remove(ConvRef c, Ts ts) override;
    void react(ConvRef c, Ts ts, std::string_view name, bool add) override;
    void markRead(ConvRef c, Ts ts) override;
    void markUnread(ConvRef c, Ts ts) override;
    void setStarred(ConvRef c, bool on) override;
    void setMuted(ConvRef c, bool on) override;
    void setNotifyLevel(ConvRef c, NotifyLevel level) override;
    void leave(ConvRef c) override;
    void openDm(UserRef u, std::function<void(ConvRef)> done) override;
    void joinChannel(ConvRef c, ConvDone done) override;
    void createChannel(std::string name, bool isPrivate, ConvDone done) override;
    void loadChannelDirectory() override;
    void setPinned(ConvRef c, Ts ts, bool on) override;
    void setSaved(ConvRef c, Ts ts, bool on) override;
    void deleteFile(ConvRef c, Ts ts, const std::string &fileId) override;
    void deleteAttachment(ConvRef c, Ts ts, int attachmentId, Done d) override;
    void downloadFile(const std::string &url, std::string toPath, Done done) override;
    void setReminder(ConvRef c, Ts ts, int64_t due) override;
    bool isAgentSession(ConvRef c) const override;
    bool canStopSession(ConvRef c) const override;
    void stopSession(ConvRef c) override;
    std::vector<AgentRole> agentRoles() const override;
    std::string            saveAgentRole(const AgentRole &role, std::string *error) override;
    void                   removeAgentRole(const std::string &id) override;
    void                   restoreAgentRole(const std::string &id) override;
    std::string            agentSessionBlocker(const std::string &dir) const override;
    void                   startAgentSession(
        const std::string                                &dir,
        bool                                              skip,
        const std::string                                &role,
        std::function<void(ConvRef, const std::string &)> done
    ) override;
    std::string agentSessionFolder(ConvRef c) const override;
    std::string agentSessionRole(ConvRef c) const override;
    void        findAgentSessions(std::function<void(std::vector<FoundSession>)> done) override;
    ConvRef     addFoundSession(const std::string &id) override;
    std::vector<Command> commands(ConvRef c) override;
    uint64_t             commandsRevision(ConvRef c) override;
    LocalResult          runLocalCommand(
        ConvRef c, Ts thread, const std::string &name, const std::string &args
    ) override;
    std::vector<std::string> promptHistory(ConvRef c) override;
    std::vector<std::string> folderPromptHistory(const std::string &dir) override;
    bool                     threadAcceptsReplies(ConvRef c, Ts root) const override;
    bool                     threadOpensAsSession(ConvRef c, Ts root) const override;
    ConvRef                  openThreadAsSession(ConvRef c, Ts root) override;
    void                     startAgentBranch(
        const std::string       &session,
        std::string              prompt,
        std::vector<std::string> files,
        std::vector<std::string> deniedTools,
        AgentBranchDone          started,
        AgentTurnFn              turn
    ) override;
    void continueAgentBranch(
        const std::string       &branch,
        std::string              prompt,
        std::vector<std::string> files,
        AgentTurnFn              turn
    ) override;
    void watchAgentBranch(const std::string &branch, AgentTurnFn turn) override;
    void agentSessionMovedOn(
        const std::string &session, const std::string &forkPoint, std::function<void(bool)> done
    ) override;
    void         setAgentBranchLabel(const std::string &branch, std::string label) override;
    bool         canDeleteMessage(ConvRef c, Ts ts) const override;
    void         pressButton(ConvRef c, Ts ts, const std::string &buttonId, Done d) override;
    void         setZenMode(bool on) override;
    void         setLocalName(ConvRef c, std::string name) override;
    void         createSection(std::string name, std::string emoji, ConvRef first) override;
    void         updateSection(const std::string &id, std::string name, std::string emoji) override;
    void         deleteSection(const std::string &id) override;
    void         moveToSection(ConvRef c, const std::string &id) override;
    void         moveSection(const std::string &id, int step) override;
    void         userTyping(ConvRef c, Ts thread) override;
    Capabilities capabilities() const override;
    SelfPresence selfPresence() const override;
    void         requestPresence(UserRef u) override;
    void         setPresence(bool away, Done d) override;
    void         setPresenceMode(PresenceMode mode) override;
    void         setNamesMode(NamesMode mode) override;
    PresenceLink presenceLink() const override;
    void         noteUserActivity() override;
    void         setWindowVisible(bool visible) override;
    void         setStatus(std::string emoji, std::string text, int64_t expiry, Done d) override;
    void         loadMyProfile(std::function<void(MyProfile)> done) override;
    void updateProfile(std::string name, std::string email, std::string phone, Done d) override;
    void setPhoto(std::string path, Done d) override;
    void sendBroadcast(ConvRef c, std::string text, Ts thread, Done d) override;
    void scheduleMessage(ConvRef c, std::string text, Ts thread, int64_t postAt, Done d) override;
    void sendBlocks(
        ConvRef c, std::string text, std::string blocks, Ts thread, bool broadcast, Done d
    ) override;
    void editBlocks(ConvRef c, Ts ts, std::string text, std::string blocks) override;
    void scheduleBlocks(
        ConvRef c, std::string text, std::string blocks, Ts thread, int64_t postAt, Done d
    ) override;
    void        refreshScheduled() override;
    void        cancelScheduled(const std::string &id, Done d) override;
    void        loadMembers(ConvRef c, MembersDone done) override;
    bool        gifSearchAvailable() const override;
    void        searchGifs(std::string query, std::function<void(std::vector<Gif>)> done) override;
    std::string promptSuggestion(ConvRef c) const override;
    void search(std::string query, std::function<void(std::vector<SearchHit>)> done) override;
    void loadMessage(ConvRef c, Ts ts, MessageDone done) override;
    void
    postAgentReply(ConvRef c, Ts root, std::string text, std::string blocks, PostDone d) override;
    void editAgentReply(ConvRef c, Ts ts, std::string text, std::string blocks, Done d) override;
    void deleteAgentReply(ConvRef c, Ts ts, Done d) override;
    void postAgentFiles(
        ConvRef c, Ts root, std::string text, std::vector<std::string> files, PostDone d
    ) override;
    void    loadThreadReplies(ConvRef c, Ts root, Ts after, RepliesDone done) override;
    void    watchThread(ConvRef c, Ts root, Ts after, bool busy) override;
    void    unwatchThread(ConvRef c, Ts root) override;
    void    loadThreadsView(std::string cursor, ThreadsViewDone done) override;
    void    markThreadRead(ConvRef c, Ts root, Ts ts) override;
    int64_t nowSecs() const override;
    void    loadChannelCanvas(ConvRef c, std::function<void(std::string)> done) override;
    void    loadCanvasMeta(const std::string &fileId, CanvasMetaDone done) override;
    void    loadCanvasContent(const std::string &fileId, CanvasHtmlDone done) override;
    void    createChannelCanvas(ConvRef c, std::string markdown, CanvasCreated done) override;
    void editCanvas(const std::string &fileId, std::vector<CanvasChange> changes, Done d) override;
    void deleteCanvas(const std::string &fileId, Done d) override;
    bool threadFollowed(ConvRef c, Ts root) const override;
    void resolveUser(UserRef u) override;
    void resolveChannel(const std::string &id) override;
    void loadSidebarTheme(std::function<void(SidebarTheme, std::string)> done) override;

private:
    Backend *_t;
};

} // namespace model
