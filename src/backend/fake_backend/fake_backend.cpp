// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "fake_backend.h"

namespace {

TextWithEntities plainText(const QString &text) {
    return TextWithEntities{text, {}};
}

Message makeMessage(const QString &ts, const QString &userId, const QString &text) {
    return Message{
        .ts     = ts,
        .date   = decimalTsToMicros(ts),
        .author = UserId{userId},
        .text   = plainText(text),
    };
}

} // namespace

FakeBackend::FakeBackend() : _authState(AuthState::LoggedIn) {
    _conversations = std::vector<Conversation>{
        {.id       = ConversationId{"C001"},
         .kind     = ConvKind::PublicChannel,
         .name     = "general",
         .isMember = true,
         .lastRead = "0",
         .unread   = 0},
        {.id       = ConversationId{"C002"},
         .kind     = ConvKind::PublicChannel,
         .name     = "random",
         .isMember = true,
         .lastRead = "0",
         .unread   = 2},
        {.id       = ConversationId{"C003"},
         .kind     = ConvKind::PrivateChannel,
         .name     = "secret",
         .isMember = true,
         .lastRead = "0",
         .unread   = 0},
        {.id       = ConversationId{"D001"},
         .kind     = ConvKind::Im,
         .name     = "alice-dm",
         .isMember = true,
         .lastRead = "0",
         .unread   = 1,
         .dmUser   = std::optional<UserId>(UserId{"U002"})},
        {.id       = ConversationId{"G001"},
         .kind     = ConvKind::Mpim,
         .name     = "mpdm-alice--bob--charlie-1",
         .isMember = true,
         .lastRead = "0",
         .unread   = 0,
         .members  = {UserId{"U002"}, UserId{"U001"}, UserId{"U004"}}},
    };

    _users = std::vector<User>{
        {UserId{"U001"}, "bob", "Bob Builder", "", false, true},
        {UserId{"U002"}, "alice", "Alice Wonder", "", false, false},
        {UserId{"U003"}, "bot", "HelperBot", "", true, false},
        {UserId{"U004"}, "charlie", "Charlie Dev", "", false, false},
    };

    _history["C001"] = {
        makeMessage("1000000000.000001", "U002", "Hey everyone, welcome!"),
        makeMessage("1000000000.000002", "U001", "Thanks! Excited to be here."),
        makeMessage("1000000000.000003", "U003", "I am a bot. Beep boop."),
    };

    _history["C002"] = {
        makeMessage("1000000001.000001", "U001", "Anyone up for a coffee break?"),
        makeMessage("1000000001.000002", "U002", "Always :coffee:"),
    };
}

rpl::producer<AuthState> FakeBackend::authState() const {
    return _authState.value();
}

Capabilities FakeBackend::capabilities() const {
    return Capabilities{}; // public-path defaults: no typing, no live presence
}

void FakeBackend::connectRealtime() {}
void FakeBackend::disconnectRealtime() {}

rpl::producer<UserId> FakeBackend::loadMe() {
    return rpl::variable<UserId>(UserId{"U001"}).value();
}

rpl::producer<std::vector<Conversation>> FakeBackend::loadConversations() {
    return _conversations.value();
}

rpl::producer<std::vector<User>> FakeBackend::loadUsers() {
    return _users.value();
}

rpl::producer<bool> FakeBackend::loadPresence(UserId) {
    return rpl::variable<bool>(false).value();
}

rpl::producer<SelfPresence> FakeBackend::loadSelfPresence() {
    // No official client connected — exercises the "phantom away" indicator in dev.
    return rpl::variable<SelfPresence>(SelfPresence{.loaded = true}).value();
}

rpl::producer<MessagePage>
FakeBackend::loadHistory(ConversationId conv, std::optional<QString> /*cursor*/) {
    auto it   = _history.find(conv.value);
    auto msgs = (it != _history.end()) ? it->second : std::vector<Message>{};
    auto page = MessagePage{std::move(msgs), std::nullopt};
    return rpl::variable<MessagePage>(std::move(page)).value();
}

rpl::producer<MessagePage> FakeBackend::loadThread(ConversationId, Ts, std::optional<QString>) {
    return rpl::variable<MessagePage>(MessagePage{}).value();
}

void FakeBackend::sendMessage(ConversationId, OutgoingMessage) {}
void FakeBackend::editMessage(ConversationId, Ts, TextWithEntities) {}
void FakeBackend::deleteMessage(ConversationId, Ts) {}
void FakeBackend::addReaction(ConversationId, Ts, QString) {}
void FakeBackend::removeReaction(ConversationId, Ts, QString) {}
void FakeBackend::markRead(ConversationId, Ts) {}

rpl::producer<Event> FakeBackend::events() const {
    return _events.events();
}

void FakeBackend::fireEvent(Event e) {
    _events.fire(std::move(e));
}

rpl::producer<std::vector<SlashCommand>> FakeBackend::listCommands() {
    return rpl::variable<std::vector<SlashCommand>>(std::vector<SlashCommand>{
                                                        {.name  = "remind",
                                                         .desc  = "Set a reminder",
                                                         .usage = "[@someone or #channel] [what] "
                                                                  "[when]"},
                                                        {.name    = "deploy",
                                                         .desc    = "Deploy a service",
                                                         .usage   = "[service]",
                                                         .appId   = "A012FAKE",
                                                         .appName = "Deploybot"},
                                                    })
        .value();
}

void FakeBackend::runCommand(
    ConversationId                     conv,
    const QString                     &command,
    const QString                     &text,
    std::function<void(bool, QString)> done
) {
    ranCommands.push_back({conv, command, text});
    if (done)
        done(true, {});
}

rpl::producer<std::vector<SearchResult>> FakeBackend::searchMessages(const QString &) {
    return rpl::variable<std::vector<SearchResult>>({}).value();
}

rpl::producer<QHash<QString, QString>> FakeBackend::loadEmojiList() {
    return rpl::variable<QHash<QString, QString>>({}).value();
}

void FakeBackend::loadChannelCanvas(ConversationId conv, std::function<void(QString, bool)> done) {
    if (!done)
        return;
    const auto it = _convCanvas.find(conv.value);
    if (it == _convCanvas.end())
        done({}, true);
    else
        done(it->second, _canvasHtml[it->second].isEmpty());
}

void FakeBackend::loadCanvasContent(
    const QString                    &fileId,
    std::function<void(QString html)> onHtml,
    std::function<void(QString)>      onError
) {
    const auto it = _canvasHtml.find(fileId);
    if (it == _canvasHtml.end()) {
        if (onError)
            onError(QStringLiteral("canvas_not_found"));
        return;
    }
    if (onHtml)
        onHtml(QStringLiteral("<div class=\"quip-canvas-content\">%1</div>").arg(it->second));
}

void FakeBackend::createChannelCanvas(
    ConversationId                      conv,
    const QString                      &markdown,
    std::function<void(QString fileId)> onSuccess,
    std::function<void(QString)>        onError
) {
    if (_convCanvas.count(conv.value)) {
        if (onError)
            onError(QStringLiteral("channel_canvas_already_exists"));
        return;
    }
    const QString fileId    = QStringLiteral("FCANVAS-%1").arg(conv.value);
    _convCanvas[conv.value] = fileId;
    // Fixture-grade markdown→HTML: one <p> per line, enough for assertions.
    QString html;
    for (const auto &line : markdown.split('\n', Qt::SkipEmptyParts))
        html += QStringLiteral("<p>%1</p>").arg(line.toHtmlEscaped());
    _canvasHtml[fileId] = html;
    if (onSuccess)
        onSuccess(fileId);
}

void FakeBackend::editCanvas(
    const QString                            &canvasId,
    const std::vector<CanvasChange>          &changes,
    std::function<void(bool ok, QString err)> done
) {
    const auto it = _canvasHtml.find(canvasId);
    if (it == _canvasHtml.end()) {
        if (done)
            done(false, QStringLiteral("canvas_not_found"));
        return;
    }
    for (const auto &c : changes) {
        const QString p = QStringLiteral("<p>%1</p>").arg(c.markdown.toHtmlEscaped());
        switch (c.op) {
        case CanvasChange::Op::InsertAtStart:
            it->second.prepend(p);
            break;
        case CanvasChange::Op::InsertAtEnd:
        case CanvasChange::Op::InsertAfter:
        case CanvasChange::Op::InsertBefore:
            it->second.append(p);
            break;
        case CanvasChange::Op::ReplaceSection:
        case CanvasChange::Op::ReplaceAll:
            it->second = p;
            break;
        case CanvasChange::Op::DeleteSection:
            break;
        case CanvasChange::Op::Rename:
            _canvasTitle[canvasId] = c.markdown;
            break;
        }
    }
    if (done)
        done(true, {});
}

void FakeBackend::loadCanvasMeta(
    const QString                                                               &fileId,
    std::function<void(QString title, QString permalink, CanvasMetaState state)> done
) {
    if (!done)
        return;
    if (!_canvasHtml.count(fileId)) {
        done({}, {}, CanvasMetaState::Gone);
        return;
    }
    const auto it = _canvasTitle.find(fileId);
    done(
        it == _canvasTitle.end() ? QString() : it->second,
        QStringLiteral("https://fake.slack.com/docs/T0/%1").arg(fileId),
        CanvasMetaState::Ok
    );
}

void FakeBackend::deleteCanvas(
    const QString &canvasId, std::function<void(bool ok, QString err)> done
) {
    if (!_canvasHtml.count(canvasId)) {
        if (done)
            done(false, QStringLiteral("canvas_not_found"));
        return;
    }
    _canvasHtml.erase(canvasId);
    _canvasTitle.erase(canvasId);
    for (auto it = _convCanvas.begin(); it != _convCanvas.end();) {
        if (it->second == canvasId)
            it = _convCanvas.erase(it);
        else
            ++it;
    }
    if (done)
        done(true, {});
}
