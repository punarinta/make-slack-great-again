// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "cc_transcript.h"
#include "cc_roles.h"

#include "text/markdown_compose.h"
#include "text/mrkdwn_parser.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

namespace claude_code {
namespace {

qint64 parseIsoMicros(const QString &iso) {
    if (iso.isEmpty())
        return 0;
    const QDateTime dt = QDateTime::fromString(iso, Qt::ISODateWithMs);
    return dt.isValid() ? dt.toMSecsSinceEpoch() * 1000 : 0;
}

QString oneLine(QString s, int maxLen = 100) {
    s            = s.trimmed();
    const int nl = s.indexOf(QLatin1Char('\n'));
    if (nl >= 0)
        s = s.left(nl).trimmed() + QStringLiteral(" …");
    if (s.size() > maxLen)
        s = s.left(maxLen - 1) + QStringLiteral("…");
    return s;
}

QString tagContent(const QString &s, const QString &tag) {
    const QString open  = QLatin1Char('<') + tag + QLatin1Char('>');
    const QString close = QStringLiteral("</") + tag + QLatin1Char('>');
    const int     a     = s.indexOf(open);
    if (a < 0)
        return {};
    const int b = s.indexOf(close, a + open.size());
    return (b < 0 ? s.mid(a + open.size()) : s.mid(a + open.size(), b - a - open.size())).trimmed();
}

// What a typed prompt shows as — empty for the system text Claude Code records
// as "user" turns (command output, notifications, reminders, interruptions).
QString cleanPrompt(const QString &raw) {
    const QString s = raw.trimmed();
    if (s.isEmpty())
        return {};
    if (s.startsWith(QLatin1String("<command-name>"))) {
        QString       name = tagContent(s, QStringLiteral("command-name"));
        const QString args = tagContent(s, QStringLiteral("command-args"));
        if (!name.startsWith(QLatin1Char('/')))
            name.prepend(QLatin1Char('/'));
        return args.isEmpty() ? name : name + QLatin1Char(' ') + args;
    }
    if (s.startsWith(QLatin1String("<bash-input>")))
        return QStringLiteral("! ") + tagContent(s, QStringLiteral("bash-input"));
    // Every other leading tag is an injection, not something the user typed:
    // <local-command-stdout>, <bash-stdout>, <task-notification>, <system-reminder>…
    static const QRegularExpression kLeadingTag(QStringLiteral("^<[a-z][a-z-]*>"));
    if (kLeadingTag.match(s).hasMatch())
        return {};
    if (s.startsWith(QLatin1String("[Request interrupted")))
        return {};
    return s;
}

// A local command's output ("<local-command-stdout>…</local-command-stdout>"),
// terminal colours stripped; null when `raw` isn't command output.
QString commandOutput(const QString &raw) {
    const QString s = raw.trimmed();
    for (const auto tag :
         {QStringLiteral("local-command-stdout"), QStringLiteral("local-command-stderr")}) {
        if (!s.startsWith(QLatin1Char('<') + tag + QLatin1Char('>')))
            continue;
        static const QRegularExpression kAnsi(QStringLiteral("\x1b\\[[0-9;?]*[A-Za-z]"));
        QString                         out = tagContent(s, tag);
        out.remove(kAnsi);
        return out.isNull() ? QString(QLatin1String("")) : out;
    }
    return {};
}

bool isAgentTool(const QString &name) {
    return name == QLatin1String("Agent") || name == QLatin1String("Task");
}

QString homeRelative(const QString &path) {
    const QString home = QDir::homePath();
    if (!home.isEmpty() && path.startsWith(home + QLatin1Char('/')))
        return QStringLiteral("~") + path.mid(home.size());
    return path;
}

} // namespace

Ts microsToTs(qint64 micros) {
    const qint64 secs = micros / 1000000;
    const qint64 frac = micros % 1000000;
    return QString::number(secs) + QLatin1Char('.') +
           QStringLiteral("%1").arg(frac, 6, 10, QLatin1Char('0'));
}

QString summarizeToolInput(const QString &toolName, const QJsonObject &input) {
    auto    str = [&](const char *key) { return input.value(QLatin1String(key)).toString(); };
    QString s;
    if (toolName == QLatin1String("Bash"))
        s = str("description").isEmpty() ? str("command") : str("description");
    else if (
        toolName == QLatin1String("Read") || toolName == QLatin1String("Write") ||
        toolName == QLatin1String("Edit") || toolName == QLatin1String("NotebookEdit")
    )
        s = homeRelative(str("file_path").isEmpty() ? str("notebook_path") : str("file_path"));
    else if (toolName == QLatin1String("Grep") || toolName == QLatin1String("Glob"))
        s = str("pattern");
    else if (toolName == QLatin1String("WebFetch"))
        s = str("url");
    else if (toolName == QLatin1String("WebSearch"))
        s = str("query");
    else if (isAgentTool(toolName))
        s = str("description");
    else if (toolName == QLatin1String("Skill"))
        s = str("skill");
    if (s.isEmpty()) {
        // Anything else: its first string argument says the most.
        for (auto it = input.begin(); it != input.end(); ++it)
            if (it.value().isString() && !it.value().toString().trimmed().isEmpty()) {
                s = it.value().toString();
                break;
            }
    }
    return oneLine(s);
}

void TranscriptParser::feed(const QByteArray &bytes) {
    _partial.append(bytes);
    qsizetype start = 0;
    for (;;) {
        const qsizetype nl = _partial.indexOf('\n', start);
        if (nl < 0)
            break;
        const QByteArray line = _partial.mid(start, nl - start).trimmed();
        if (!line.isEmpty())
            handleLine(line);
        start = nl + 1;
    }
    _partial.remove(0, start);
}

Ts TranscriptParser::nextTs(qint64 micros, qint64 *outDate) {
    // Strictly increasing, so two records in the same millisecond (a text block
    // and a tool call streamed together) still get distinct, ordered ids.
    if (micros <= _lastMicros)
        micros = _lastMicros + 1;
    _lastMicros = micros;
    *outDate    = micros;
    return microsToTs(micros);
}

void TranscriptParser::closeToolGroup() {
    _openToolGroup = -1;
}

void TranscriptParser::resolvePendingText(TranscriptItem::State state) {
    if (_pendingText < 0)
        return;
    _items[size_t(_pendingText)].state = state;
    _pendingText                       = -1;
}

void TranscriptParser::endTurn() {
    resolvePendingText(TranscriptItem::State::Final);
    closeToolGroup();
    _turnOpen = false;
}

void TranscriptParser::addPrompt(
    const QString &text, qint64 micros, const QStringList &images, const QStringList &imageNames
) {
    TranscriptItem item;
    item.kind = TranscriptItem::Kind::UserPrompt;
    item.ts   = nextTs(micros, &item.date);
    item.text = text;
    static const QRegularExpression kRelay(QStringLiteral(
        "^The user replied in the thread of subagent ([A-Za-z0-9_-]+)\\. Pass their message on "
        "to it verbatim with SendMessage \\(to: \"\\1\"\\):\\n\\n([\\s\\S]+)$"
    ));
    if (const auto relay = kRelay.match(text); relay.hasMatch()) {
        item.relayTo = relay.captured(1);
        item.text    = relay.captured(2);
    }
    item.images     = images;
    item.imageNames = imageNames;
    item.uuid       = _lineUuid;
    _items.push_back(std::move(item));
    _turnOpen = true;
}

void TranscriptParser::addCommandOutput(const QString &output, qint64 micros) {
    closeToolGroup();
    const QString text = output.trimmed();
    if (!text.isEmpty()) {
        TranscriptItem item;
        item.kind = TranscriptItem::Kind::AssistantText;
        item.ts   = nextTs(micros, &item.date);
        // Several lines are a terminal layout (a chart, a list): kept monospaced.
        item.text = text.contains(QLatin1Char('\n'))
                        ? QStringLiteral("```\n") + text + QStringLiteral("\n```")
                        : text;
        item.uuid = _lineUuid;
        _items.push_back(std::move(item));
        _commandOutput = int(_items.size()) - 1;
    }
    endTurn();
}

void TranscriptParser::handleLine(const QByteArray &line) {
    QJsonParseError   err;
    const QJsonObject o = QJsonDocument::fromJson(line, &err).object();
    if (err.error != QJsonParseError::NoError || o.isEmpty())
        return; // a torn or foreign line — skip, never fail the whole transcript

    const QString type = o.value(QLatin1String("type")).toString();
    _lineUuid          = o.value(QLatin1String("uuid")).toString();
    // A copy of a session starts with the records it was copied from, same
    // uuids and all — the ones read already, from the session it continues.
    if (!_lineUuid.isEmpty()) {
        if (_seenUuids.contains(_lineUuid))
            return;
        _seenUuids.insert(_lineUuid);
    }
    const qint64 micros = parseIsoMicros(o.value(QLatin1String("timestamp")).toString());
    if (micros > _lastActivity)
        _lastActivity = micros;

    if (const QString v = o.value(QLatin1String("version")).toString(); !v.isEmpty())
        _version = v;
    if (const QString m = o.value(QLatin1String("permissionMode")).toString(); !m.isEmpty())
        _permissionMode = m;
    if (type == QLatin1String("ai-title")) {
        _aiTitle = o.value(QLatin1String("aiTitle")).toString().trimmed();
        return;
    }
    if (type == QLatin1String("system")) {
        const QString subtype = o.value(QLatin1String("subtype")).toString();
        if (subtype == QLatin1String("turn_duration")) {
            endTurn();
        } else if (subtype == QLatin1String("local_command")) {
            // A command Claude Code runs itself (/context, /model…): the command,
            // then its output — which is all there is to that turn.
            const QString content = o.value(QLatin1String("content")).toString();
            const QString output  = commandOutput(content);
            if (!output.isNull()) {
                addCommandOutput(output, micros);
            } else if (const QString cmd = cleanPrompt(content); !cmd.isEmpty()) {
                resolvePendingText(TranscriptItem::State::Final);
                closeToolGroup();
                addPrompt(cmd, micros);
            }
        }
        return;
    }
    if (type == QLatin1String("attachment")) {
        // A prompt typed while Claude was busy: recorded as a queued command that
        // the running turn picks up — the turn goes on, so nothing is resolved.
        const QJsonObject a = o.value(QLatin1String("attachment")).toObject();
        // The system prompt as sent, recorded once and sent again on resume:
        // a role msga started the session with is its last part (cc_roles).
        if (a.value(QLatin1String("type")).toString() == QLatin1String("prompt_snapshot")) {
            const RoleMark mark =
                roleInSystemPrompt(a.value(QLatin1String("systemPrompt")).toArray());
            _role     = mark.id;
            _roleName = mark.name;
            return;
        }
        if (a.value(QLatin1String("type")).toString() == QLatin1String("queued_command") &&
            a.value(QLatin1String("commandMode")).toString() == QLatin1String("prompt")) {
            const QString text = cleanPrompt(a.value(QLatin1String("prompt")).toString());
            if (!text.isEmpty()) {
                closeToolGroup();
                addPrompt(text, micros);
            }
        }
        return;
    }

    const QJsonObject message = o.value(QLatin1String("message")).toObject();
    const QJsonValue  content = message.value(QLatin1String("content"));

    if (type == QLatin1String("user")) {
        // What Claude Code tells the model (skill bodies, caveats) and the
        // summary a compaction starts from aren't anything anyone typed.
        if (o.value(QLatin1String("isMeta")).toBool()) {
            // /context also writes its report as markdown for the model, right
            // after the terminal drawing: that reads far better here.
            const QString md = content.toString().trimmed();
            if (_commandOutput >= 0 && _commandOutput == int(_items.size()) - 1 && !md.isEmpty() &&
                !md.startsWith(QLatin1Char('<')))
                _items[_commandOutput].text = md;
            _commandOutput = -1;
            return;
        }
        if (o.value(QLatin1String("isCompactSummary")).toBool())
            return;
        const QJsonValue origin = o.value(QLatin1String("origin"));
        if (origin.isObject() &&
            origin.toObject().value(QLatin1String("kind")).toString() != QLatin1String("human"))
            return; // task notifications and other machine-originated turns

        QString     text;
        QStringList images;
        if (content.isString()) {
            text = content.toString();
        } else if (content.isArray()) {
            bool sawToolResult = false;
            for (const auto &v : content.toArray()) {
                const QJsonObject b  = v.toObject();
                const QString     bt = b.value(QLatin1String("type")).toString();
                if (bt == QLatin1String("tool_result")) {
                    sawToolResult         = true;
                    const QString id      = b.value(QLatin1String("tool_use_id")).toString();
                    const bool    failed  = b.value(QLatin1String("is_error")).toBool();
                    const QString agentId = o.value(QLatin1String("toolUseResult"))
                                                .toObject()
                                                .value(QLatin1String("agentId"))
                                                .toString();
                    // Newest first: the call is almost always in the latest item.
                    for (auto it = _items.rbegin(); it != _items.rend(); ++it) {
                        auto call = std::find_if(
                            it->tools.begin(), it->tools.end(), [&](const ToolCall &c) {
                                return c.toolUseId == id;
                            }
                        );
                        if (call == it->tools.end())
                            continue;
                        call->error = failed;
                        if (it->kind == TranscriptItem::Kind::Subagent && !agentId.isEmpty())
                            it->agentId = agentId;
                        break;
                    }
                } else if (bt == QLatin1String("text")) {
                    if (!text.isEmpty())
                        text += QLatin1Char('\n');
                    text += b.value(QLatin1String("text")).toString();
                } else if (bt == QLatin1String("image")) {
                    // A pasted image rides the prompt as base64.
                    const QJsonObject src = b.value(QLatin1String("source")).toObject();
                    if (src.value(QLatin1String("type")).toString() == QLatin1String("base64")) {
                        const QString path = cachePastedImage(
                            src.value(QLatin1String("media_type")).toString(),
                            src.value(QLatin1String("data")).toString().toLatin1()
                        );
                        if (!path.isEmpty())
                            images << path;
                    }
                }
            }
            if (sawToolResult)
                return; // tool output rides a "user" record; it isn't a prompt
        }
        if (const QString output = commandOutput(text); !output.isNull()) {
            addCommandOutput(output, micros); // e.g. what /compact says when done
            return;
        }
        const bool isCommand = text.trimmed().startsWith(QLatin1String("<command-name>"));
        text                 = cleanPrompt(text);
        // /compact records the prompt as typed, then again as the command it ran.
        if (isCommand && !_items.empty() &&
            _items.back().kind == TranscriptItem::Kind::UserPrompt && _items.back().text == text)
            return;
        if (!images.isEmpty()) {
            // The images are attached; drop their "[Image #3]" placeholders (and
            // the "[Image: source: …]" lines some clients add) from the text.
            static const QRegularExpression kPlaceholder(
                QStringLiteral("[ \\t]?\\[Image(?: #\\d+|: source: [^\\]]*)\\]")
            );
            text.remove(kPlaceholder);
            text = text.trimmed();
        }
        if (text.isEmpty() && images.isEmpty())
            return;
        // A new prompt starts a new turn: whatever Claude said last in the
        // previous one was its answer.
        resolvePendingText(TranscriptItem::State::Final);
        closeToolGroup();
        // Named after Claude Code's paste numbers ("[Image #3]"), in order.
        QStringList      names;
        const QJsonArray ids = o.value(QLatin1String("imagePasteIds")).toArray();
        for (int i = 0; i < images.size(); ++i) {
            const QString ext = QFileInfo(images[i]).suffix();
            names
                << (i < ids.size() ? QStringLiteral("Image %1.%2").arg(ids[i].toInt()).arg(ext)
                                   : QStringLiteral("Image.%1").arg(ext));
        }
        addPrompt(text, micros, images, names);
        return;
    }

    if (type != QLatin1String("assistant") || !content.isArray())
        return;
    if (const QString m = message.value(QLatin1String("model")).toString();
        !m.isEmpty() && m != QLatin1String("<synthetic>"))
        _model = m;
    // Claude Code's stand-in answer to a turn that asked for none (after a
    // command's output) — not something Claude said.
    if (message.value(QLatin1String("model")).toString() == QLatin1String("<synthetic>") &&
        content.toArray().size() == 1 &&
        content.toArray().first().toObject().value(QLatin1String("text")).toString().trimmed() ==
            QLatin1String("No response requested."))
        return;
    for (const auto &v : content.toArray()) {
        const QJsonObject b  = v.toObject();
        const QString     bt = b.value(QLatin1String("type")).toString();
        if (bt == QLatin1String("text")) {
            const QString text = b.value(QLatin1String("text")).toString().trimmed();
            if (text.isEmpty())
                continue;
            // Two texts in a row within one turn: the earlier one wasn't the end.
            resolvePendingText(TranscriptItem::State::Progress);
            closeToolGroup();
            TranscriptItem item;
            item.kind  = TranscriptItem::Kind::AssistantText;
            item.state = TranscriptItem::State::Pending;
            item.ts    = nextTs(micros, &item.date);
            item.text  = text;
            item.uuid  = _lineUuid;
            _items.push_back(std::move(item));
            _pendingText = int(_items.size()) - 1;
            _turnOpen    = true;
        } else if (bt == QLatin1String("tool_use")) {
            // Claude kept working after its text, so that text was an update.
            resolvePendingText(TranscriptItem::State::Progress);
            ToolCall call;
            call.toolUseId          = b.value(QLatin1String("id")).toString();
            call.name               = b.value(QLatin1String("name")).toString();
            const QJsonObject input = b.value(QLatin1String("input")).toObject();
            call.summary            = summarizeToolInput(call.name, input);
            _turnOpen               = true;
            if (isAgentTool(call.name)) {
                closeToolGroup();
                TranscriptItem item;
                item.kind      = TranscriptItem::Kind::Subagent;
                item.ts        = nextTs(micros, &item.date);
                item.text      = call.summary;
                item.agentType = input.value(QLatin1String("subagent_type")).toString();
                item.tools.push_back(std::move(call));
                _items.push_back(std::move(item));
                continue;
            }
            if (_openToolGroup < 0) {
                TranscriptItem item;
                item.kind = TranscriptItem::Kind::ToolGroup;
                item.ts   = nextTs(micros, &item.date);
                _items.push_back(std::move(item));
                _openToolGroup = int(_items.size()) - 1;
            }
            _items[size_t(_openToolGroup)].tools.push_back(std::move(call));
        }
        // thinking, redacted_thinking, …: not shown
    }
}

QString cachePastedImage(const QString &mediaType, const QByteArray &base64) {
    if (!mediaType.startsWith(QLatin1String("image/")) || base64.isEmpty())
        return {};
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                        QStringLiteral("/claude-code/images");
    QString       ext = mediaType.mid(6).toLower();
    if (ext == QLatin1String("jpeg"))
        ext = QStringLiteral("jpg");
    const QString path =
        dir + QLatin1Char('/') +
        QString::fromLatin1(QCryptographicHash::hash(base64, QCryptographicHash::Sha1).toHex()) +
        QLatin1Char('.') + ext;
    if (QFileInfo::exists(path))
        return path;
    QDir().mkpath(dir);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(QByteArray::fromBase64(base64));
    return f.commit() ? path : QString();
}

// Slack's server turns every bare URL into a <url> link before a client sees
// it; Claude's text comes raw. Wrap them the same way (in already-escaped text),
// outside code, and leave markdown [label](url) links to the converter. Claude's
// own <url> autolinks arrive escaped as &lt;url&gt; and are restored too.
// A teammate mention ("@claude:role:engineer", "@claude:agent") is how the
// composer's pill reaches Claude; shown, it becomes a <@…> mention again. A
// prompt sent before pills were unwrapped holds the raw <@…> token (escaped
// here), which must not gain a second pair of brackets.
static QString linkBareUrls(const QString &escaped) {
    static const QRegularExpression kAutolink(
        QStringLiteral("&lt;(https?://[^\\s&]+(?:&amp;[^\\s&]+)*)&gt;")
    );
    static const QRegularExpression kUrl(QStringLiteral("(?<!\\]\\()https?://[^\\s<>\\[\\]`]+"));
    static const QRegularExpression kTeammate(QStringLiteral(
        "&lt;@(claude:(?:agent|role:[a-z0-9-]+))&gt;|"
        "(?<![\\w@/:.-])@(claude:(?:agent|role:[a-z0-9-]+))(?![\\w-])"
    ));
    static const QRegularExpression kWholeTeammate(
        QStringLiteral("^@claude:(?:agent|role:[a-z0-9-]+)$")
    );
    auto linkPlain = [&](const QString &part) {
        QString res = part;
        res.replace(kAutolink, QStringLiteral("<\\1>"));
        res.replace(kTeammate, QStringLiteral("<@\\1\\2>"));
        QString out;
        int     last = 0;
        for (auto it = kUrl.globalMatch(res); it.hasNext();) {
            const auto m     = it.next();
            int        start = int(m.capturedStart());
            // Already a <url> token (restored autolink above).
            if (start > 0 && res[start - 1] == QLatin1Char('<'))
                continue;
            QString url = m.captured();
            // Sentence punctuation after a URL isn't part of it, nor is a ")"
            // closing a parenthesis the URL sits in.
            while (!url.isEmpty()) {
                const QChar c = url.back();
                if (QStringLiteral(".,;:!?'\"*_~").contains(c) ||
                    (c == QLatin1Char(')') &&
                     url.count(QLatin1Char('(')) < url.count(QLatin1Char(')'))))
                    url.chop(1);
                else if (url.endsWith(QLatin1String("&gt;")) || url.endsWith(QLatin1String("&lt;")))
                    url.chop(4);
                else
                    break;
            }
            if (url.size() <= 8) // "https://" alone
                continue;
            out += res.mid(last, start - last) + QLatin1Char('<') + url + QLatin1Char('>');
            last = start + int(url.size());
        }
        return out + res.mid(last);
    };
    // Code (``` fences and `spans`) keeps its URLs as text.
    QString out;
    bool    inFence = false;
    for (const QString &line : escaped.split(QLatin1Char('\n'))) {
        QString done;
        if (line.trimmed().startsWith(QLatin1String("```"))) {
            inFence = !inFence;
            done    = line;
        } else if (inFence) {
            done = line;
        } else {
            const QStringList spans = line.split(QLatin1Char('`'));
            for (int i = 0; i < spans.size(); ++i) {
                // Odd pieces sit between backticks — unless the last backtick
                // is unpaired, then that tail is plain text.
                const bool code = i % 2 == 1 && (i < spans.size() - 1 || spans.size() % 2 == 1);
                // Claude likes to quote a teammate's id as `@claude:role:x`; a
                // span that is nothing but one is a mention, not code.
                if (code && kWholeTeammate.match(spans[i]).hasMatch()) {
                    done += QStringLiteral("<") + spans[i] + QLatin1Char('>');
                    ++i;
                    if (i < spans.size())
                        done += linkPlain(spans[i]);
                    continue;
                }
                if (i > 0)
                    done += QLatin1Char('`');
                done += code ? spans[i] : linkPlain(spans[i]);
            }
        }
        out += done + QLatin1Char('\n');
    }
    out.chop(1);
    return out;
}

TextWithEntities renderMarkdown(const QString &markdown) {
    // Pre-pass on whole lines, outside code fences: headings have no mrkdwn form,
    // so bold them; a table is only readable aligned, so fence it (monospace).
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    QStringList       out;
    out.reserve(lines.size() + 4);
    bool inFence = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line    = lines[i];
        const QString  trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1String("```"))) {
            inFence = !inFence;
            out << line;
            continue;
        }
        if (inFence) {
            out << line;
            continue;
        }
        if (trimmed.startsWith(QLatin1Char('|'))) {
            int end = i;
            while (end + 1 < lines.size() && lines[end + 1].trimmed().startsWith(QLatin1Char('|')))
                ++end;
            if (end > i) {
                out << QStringLiteral("```");
                for (int k = i; k <= end; ++k)
                    out << lines[k];
                out << QStringLiteral("```");
                i = end;
                continue;
            }
        }
        static const QRegularExpression kHeading(QStringLiteral("^#{1,6}\\s+(.+?)\\s*#*$"));
        if (const auto m = kHeading.match(trimmed); m.hasMatch()) {
            out << QStringLiteral("**") + m.captured(1) + QStringLiteral("**");
            continue;
        }
        out << line;
    }
    // Claude's text is plain markdown, never Slack tokens: escape what mrkdwn
    // would read as a token or entity. The parser decodes these back.
    QString text = out.join(QLatin1Char('\n'));
    text.replace(QLatin1Char('&'), QLatin1String("&amp;"))
        .replace(QLatin1Char('<'), QLatin1String("&lt;"))
        .replace(QLatin1Char('>'), QLatin1String("&gt;"));
    return MrkdwnParser::parse(MarkdownCompose::convert(linkBareUrls(text)).mrkdwn);
}

namespace {

// "| a | b |" → {"a", "b"}. A pipe escaped (\|) or inside `code` stays in its cell.
QStringList splitTableRow(const QString &line) {
    QString s = line.trimmed();
    if (s.startsWith(QLatin1Char('|')))
        s.remove(0, 1);
    if (s.endsWith(QLatin1Char('|')) && !s.endsWith(QLatin1String("\\|")))
        s.chop(1);
    QStringList cells;
    QString     cell;
    bool        inCode = false;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == QLatin1Char('\\') && i + 1 < s.size() && s[i + 1] == QLatin1Char('|')) {
            cell += QLatin1Char('|');
            ++i;
        } else if (c == QLatin1Char('`')) {
            inCode = !inCode;
            cell += c;
        } else if (c == QLatin1Char('|') && !inCode) {
            cells << cell.trimmed();
            cell.clear();
        } else {
            cell += c;
        }
    }
    cells << cell.trimmed();
    return cells;
}

bool isTableSeparator(const QString &line) {
    static const QRegularExpression kSep(
        QStringLiteral("^\\|?\\s*:?-+:?\\s*(\\|\\s*:?-+:?\\s*)*\\|?$")
    );
    return kSep.match(line.trimmed()).hasMatch();
}

} // namespace

std::vector<Block> markdownBlocks(const QString &markdown) {
    const QStringList  lines = markdown.split(QLatin1Char('\n'));
    std::vector<Block> blocks;
    QStringList        text; // lines waiting to become a rich_text block
    bool               sawTable  = false;
    auto               flushText = [&] {
        const QString chunk = text.join(QLatin1Char('\n')).trimmed();
        text.clear();
        if (chunk.isEmpty())
            return;
        Block b;
        b.typeStr = QStringLiteral("rich_text");
        b.text    = renderMarkdown(chunk);
        blocks.push_back(std::move(b));
    };
    bool inFence = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString trimmed = lines[i].trimmed();
        if (trimmed.startsWith(QLatin1String("```")))
            inFence = !inFence;
        // A table: a "|" row followed by its "|---|" separator line.
        if (!inFence && trimmed.startsWith(QLatin1Char('|')) && i + 1 < lines.size() &&
            isTableSeparator(lines[i + 1])) {
            flushText();
            Block table;
            table.typeStr = QStringLiteral("table");
            auto addRow   = [&](const QString &line, bool header) {
                std::vector<TextWithEntities> row;
                for (const QString &cell : splitTableRow(line)) {
                    TextWithEntities t = renderMarkdown(cell);
                    if (header && !t.text.isEmpty())
                        t.entities.insert(
                            t.entities.begin(), TextEntity{EntityType::Bold, 0, int(t.text.size())}
                        );
                    row.push_back(std::move(t));
                }
                table.tableRows.push_back(std::move(row));
            };
            addRow(lines[i], /*header=*/true);
            i += 2;
            for (; i < lines.size() && lines[i].trimmed().startsWith(QLatin1Char('|')); ++i)
                addRow(lines[i], false);
            --i;
            blocks.push_back(std::move(table));
            sawTable = true;
            continue;
        }
        text << lines[i];
    }
    if (!sawTable)
        return {};
    flushText();
    return blocks;
}

QString subagentReplyPrompt(const QString &agentId, const QString &reply) {
    return QStringLiteral(
               "The user replied in the thread of subagent %1. Pass their message on "
               "to it verbatim with SendMessage (to: \"%1\"):\n\n%2"
    )
        .arg(agentId, reply);
}

Message toMessage(const TranscriptItem &item, const UserId &me, const UserId &claude) {
    Message m;
    m.ts   = item.ts;
    m.date = item.date;
    switch (item.kind) {
    case TranscriptItem::Kind::UserPrompt:
        m.author  = me;
        m.rawText = item.text;
        m.text    = renderMarkdown(item.text);
        m.blocks  = markdownBlocks(item.text);
        for (int i = 0; i < item.images.size(); ++i) {
            const QString &path = item.images[i];
            const QString  url  = QUrl::fromLocalFile(path).toString();
            File           f;
            f.id   = item.ts + QStringLiteral("-img%1").arg(i);
            f.name = i < item.imageNames.size() ? item.imageNames[i] : QFileInfo(path).fileName();
            f.mimeType           = QStringLiteral("image/") + QFileInfo(path).suffix();
            f.urlPrivate         = url;
            f.urlPrivateDownload = url;
            f.thumbUrl           = url;
            f.size               = QFileInfo(path).size();
            const QSize sz       = QImageReader(path).size();
            f.imageWidth         = sz.width() > 0 ? sz.width() : 1;
            f.imageHeight        = sz.height() > 0 ? sz.height() : 1;
            if (sz.width() > 0)
                f.thumbs.push_back(FileThumb{sz.width(), sz.height(), url});
            m.files.push_back(std::move(f));
        }
        break;
    case TranscriptItem::Kind::AssistantText:
        m.author  = claude;
        m.rawText = item.text;
        m.text    = renderMarkdown(item.text);
        m.blocks  = markdownBlocks(item.text);
        if (item.state == TranscriptItem::State::Progress)
            m.subtype = QString::fromLatin1(kProgressSubtype);
        break;
    case TranscriptItem::Kind::ToolGroup: {
        m.author                = claude;
        m.subtype               = QString::fromLatin1(kProgressSubtype);
        // One card per run of calls: the tool and what it touched, one per line.
        // Capped so a turn of a hundred calls stays a compact card.
        constexpr int kMaxLines = 12;
        const int     n         = int(item.tools.size());
        Attachment    card;
        card.color = QStringLiteral("#8a8a8a");
        card.title = QCoreApplication::translate("claude_code", "%n tool call(s)", nullptr, n);
        TextWithEntities body;
        const int        first = std::max(0, n - kMaxLines);
        if (first > 0) {
            body.text = QCoreApplication::translate("claude_code", "… %n earlier", nullptr, first) +
                        QLatin1Char('\n');
        }
        for (int i = first; i < n; ++i) {
            const auto &c    = item.tools[size_t(i)];
            const int   from = int(body.text.size());
            body.text += c.name;
            body.entities.push_back(TextEntity{EntityType::Bold, from, int(c.name.size()), {}});
            if (!c.summary.isEmpty())
                body.text += QStringLiteral("  ") + c.summary;
            if (c.error)
                body.text += QStringLiteral("  ✗");
            if (i + 1 < n)
                body.text += QLatin1Char('\n');
        }
        card.fallback = card.title;
        card.text     = std::move(body);
        // No text of its own: the card says it all (a title line above it would
        // just repeat the card's). Previews read the card's fallback.
        m.attachments.push_back(std::move(card));
        break;
    }
    case TranscriptItem::Kind::Subagent: {
        m.author        = claude;
        m.subtype       = QString::fromLatin1(kProgressSubtype);
        const QString t = QCoreApplication::translate("claude_code", "Subagent: %1")
                              .arg(item.text.isEmpty() ? item.tools.front().name : item.text);
        m.text          = {t, {TextEntity{EntityType::Italic, 0, int(t.size()), {}}}};
        m.rawText       = t;
        break;
    }
    }
    return m;
}

namespace {

// Thinking only: the reasoning half of an answer, written as a record of its own.
bool isThinkingRecord(const QJsonObject &o) {
    if (o.value(QLatin1String("type")).toString() != QLatin1String("assistant"))
        return false;
    const QJsonArray blocks =
        o.value(QLatin1String("message")).toObject().value(QLatin1String("content")).toArray();
    if (blocks.isEmpty())
        return false;
    for (const auto &v : blocks) {
        const QString bt = v.toObject().value(QLatin1String("type")).toString();
        if (bt != QLatin1String("thinking") && bt != QLatin1String("redacted_thinking"))
            return false;
    }
    return true;
}

// Where the next turn starts: a turn's end, or a prompt someone typed.
bool endsTurn(const QJsonObject &o) {
    const QString type = o.value(QLatin1String("type")).toString();
    if (type == QLatin1String("system"))
        return o.value(QLatin1String("subtype")).toString() == QLatin1String("turn_duration");
    if (type != QLatin1String("user") || o.value(QLatin1String("isMeta")).toBool())
        return false;
    const QJsonValue content =
        o.value(QLatin1String("message")).toObject().value(QLatin1String("content"));
    if (content.isString())
        return true;
    for (const auto &v : content.toArray())
        if (v.toObject().value(QLatin1String("type")).toString() == QLatin1String("tool_result"))
            return false;
    return true;
}

} // namespace

bool hasTurnSince(const QString &path, qint64 from, qint64 afterMs) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(from))
        return false;
    while (!f.atEnd()) {
        const QJsonObject rec  = QJsonDocument::fromJson(f.readLine()).object();
        const QString     type = rec.value(QLatin1String("type")).toString();
        if (type != QLatin1String("user") && type != QLatin1String("assistant"))
            continue;
        if (afterMs <= 0)
            return true;
        const QDateTime at = QDateTime::fromString(
            rec.value(QLatin1String("timestamp")).toString(), Qt::ISODateWithMs
        );
        if (at.isValid() && at.toMSecsSinceEpoch() > afterMs)
            return true;
    }
    return false;
}

bool removeFromTranscript(const QString &path, const QString &uuid, QString *error) {
    auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    QFile f(path);
    if (uuid.isEmpty() || !f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("can't read %1").arg(path));
    QList<QByteArray> lines = f.readAll().split('\n');
    f.close();
    if (!lines.isEmpty() && lines.back().isEmpty())
        lines.removeLast();
    std::vector<QJsonObject> recs(size_t(lines.size())); // empty for a line that isn't JSON
    int                      target = -1;
    for (int i = 0; i < lines.size(); ++i) {
        recs[size_t(i)] = QJsonDocument::fromJson(lines[i]).object();
        if (recs[size_t(i)].value(QLatin1String("uuid")).toString() == uuid)
            target = i;
    }
    if (target < 0)
        return fail(QStringLiteral("no record %1 in %2").arg(uuid, path));

    // What goes, each with the parent that records linked to it are moved to.
    QHash<QString, QString> gone;
    auto                    drop = [&](const QJsonObject &o) {
        gone.insert(
            o.value(QLatin1String("uuid")).toString(),
            o.value(QLatin1String("parentUuid")).toString()
        );
    };
    const QJsonObject &t = recs[size_t(target)];
    drop(t);
    if (t.value(QLatin1String("type")).toString() == QLatin1String("assistant")) {
        // An answer: the thinking written as part of the same message.
        const QString id =
            t.value(QLatin1String("message")).toObject().value(QLatin1String("id")).toString();
        for (const auto &o : recs)
            if (!id.isEmpty() && isThinkingRecord(o) &&
                o.value(QLatin1String("message")).toObject().value(QLatin1String("id")) == id)
                drop(o);
    } else {
        // A prompt: the thinking of the turn it started.
        for (size_t i = size_t(target) + 1; i < recs.size() && !endsTurn(recs[i]); ++i)
            if (isThinkingRecord(recs[i]))
                drop(recs[i]);
    }
    // Claude Code's own bookkeeping keeps a typed prompt's text as well (its
    // input queue, the last prompt for `claude --resume`): none of it reaches
    // Claude, but a deleted message shouldn't linger in the file.
    const QJsonValue promptText =
        t.value(QLatin1String("message")).toObject().value(QLatin1String("content"));
    std::vector<bool> copies(recs.size(), false);
    if (t.value(QLatin1String("type")).toString() == QLatin1String("user") && promptText.isString())
        for (size_t i = 0; i < recs.size(); ++i) {
            const QString type = recs[i].value(QLatin1String("type")).toString();
            copies[i]          = (type == QLatin1String("queue-operation") &&
                                  recs[i].value(QLatin1String("content")) == promptText) ||
                                 (type == QLatin1String("last-prompt") &&
                                  recs[i].value(QLatin1String("lastPrompt")) == promptText);
        }
    auto relink = [&gone](QString id) {
        for (int hops = 0; gone.contains(id) && hops < gone.size(); ++hops)
            id = gone.value(id);
        return id;
    };

    QByteArray out;
    for (int i = 0; i < lines.size(); ++i) {
        QJsonObject &o = recs[size_t(i)];
        if (copies[size_t(i)] || gone.contains(o.value(QLatin1String("uuid")).toString()))
            continue;
        bool changed = false;
        for (const char *key : {"parentUuid", "logicalParentUuid", "leafUuid"}) {
            const QString link = o.value(QLatin1String(key)).toString();
            if (link.isEmpty() || !gone.contains(link))
                continue;
            const QString to = relink(link);
            o.insert(QLatin1String(key), to.isEmpty() ? QJsonValue() : QJsonValue(to));
            changed = true;
        }
        // Untouched lines are kept byte for byte.
        out += changed ? QJsonDocument(o).toJson(QJsonDocument::Compact) : lines[i];
        out += '\n';
    }
    // In place, not by replacing the file: a writer holding it open keeps
    // appending to the same file.
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(out) != out.size())
        return fail(QStringLiteral("can't write %1").arg(path));
    return true;
}

} // namespace claude_code
