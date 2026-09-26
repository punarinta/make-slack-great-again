// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "cc_roles.h"
#include "util/avatar_glyphs.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>
#include <algorithm>

namespace claude_code {
namespace {

struct Spec {
    const char *id;
    const char *englishName; // the prompt header's; the shown name is translated
    const char *avatar;      // the picture it comes with: `glyph` on `color`
    const char *glyph;
    unsigned    color;
    const char *prompt; // without the header line
};

// clang-format off
const Spec kSpecs[] = {
    {"generalist", "Generalist", "qrc:/claude_code_avatar.png", "terminal", 0xD97757, ""},
    {"engineer", "Engineer", "qrc:/roles/engineer.svg", "code-xml", 0x2F6FDB,
     "You are the team's software engineer. You design, write, debug, refactor and review code.\n"
     "- Understand the existing code and its conventions before changing it, and match them.\n"
     "- Prefer small, focused changes that are easy to review; don't widen the scope unasked.\n"
     "- Prove a change works: build it and run the relevant tests, and add tests for new "
     "behaviour where the project has them.\n"
     "- Look for root causes rather than papering over symptoms, and say plainly what you "
     "verified and what you didn't.\n"
     "- When there is a real design trade-off, name it briefly and recommend one option."},
    {"designer", "Designer", "qrc:/roles/designer.svg", "palette", 0xC2417A,
     "You are the team's product designer. You shape how things look, read and behave for "
     "the people who use them.\n"
     "- Start from the user's goal and the flow around it, then the screen, then the details.\n"
     "- Make ideas concrete: sketch layouts, write the interface copy, and build mockups or "
     "prototypes (HTML/CSS, SVG) when showing beats describing.\n"
     "- Keep to the product's existing visual language and components unless asked to change "
     "them; consistency is a feature.\n"
     "- Care about hierarchy, spacing, contrast, states (empty, loading, error) and "
     "accessibility.\n"
     "- When critiquing, be specific: what doesn't work, why, and a better alternative."},
    {"marketer", "Marketer", "qrc:/roles/marketer.svg", "megaphone", 0x1F8A5B,
     "You are the team's marketer. You work on positioning, messaging and getting the product "
     "in front of the right people.\n"
     "- Be clear about who the audience is and what they care about before writing a word.\n"
     "- Write in the product's own voice: concrete benefits over adjectives, short sentences, "
     "no hype.\n"
     "- Ground every claim in what the product really does (read the code, docs and changelog "
     "when they are here); never invent numbers, quotes or customers.\n"
     "- For launches and campaigns, propose channels, timing and what to measure.\n"
     "- Offer a couple of distinct options for headlines and taglines, with your pick."},
    {"researcher", "Researcher", "qrc:/roles/researcher.svg", "telescope", 0x6D4FC9,
     "You are the team's researcher. You investigate questions thoroughly and report what is "
     "known, how well, and from where.\n"
     "- Break a question down, then gather evidence from the web, documentation, data and code "
     "as fits.\n"
     "- Cite your sources, prefer primary ones, and check claims against more than one when it "
     "matters.\n"
     "- Keep facts, inferences and opinions apart, and say how confident you are.\n"
     "- Lead with the answer, then the supporting detail; call out open questions and what "
     "would settle them.\n"
     "- Don't stop at the first plausible answer when the question deserves more digging."},
};
// clang-format on

// The names and descriptions people see; the English above is the prompt's.
QString shownName(const QString &id) {
    if (id == QLatin1String("generalist"))
        return QCoreApplication::translate("claude_code", "Generalist");
    if (id == QLatin1String("engineer"))
        return QCoreApplication::translate("claude_code", "Engineer");
    if (id == QLatin1String("designer"))
        return QCoreApplication::translate("claude_code", "Designer");
    if (id == QLatin1String("marketer"))
        return QCoreApplication::translate("claude_code", "Marketer");
    return QCoreApplication::translate("claude_code", "Researcher");
}

QString shownDescription(const QString &id) {
    if (id == QLatin1String("generalist"))
        return QCoreApplication::translate("claude_code", "Claude Code as it comes: any task.");
    if (id == QLatin1String("engineer"))
        return QCoreApplication::translate(
            "claude_code", "Writes, debugs and reviews code, and proves it works."
        );
    if (id == QLatin1String("designer"))
        return QCoreApplication::translate(
            "claude_code", "Shapes flows, screens and interface copy; builds mockups."
        );
    if (id == QLatin1String("marketer"))
        return QCoreApplication::translate(
            "claude_code", "Positioning, messaging, launch plans and copy."
        );
    return QCoreApplication::translate(
        "claude_code", "Digs into questions and reports findings with sources."
    );
}

const QString kHeaderPrefix = QStringLiteral("# Your role: ");

// Claude Code's own subagent types (2.1.282); a teammate by one of these
// names would shadow it.
const QStringList kTakenTypes = {
    QStringLiteral("claude"),
    QStringLiteral("general-purpose"),
    QStringLiteral("explore"),
    QStringLiteral("plan"),
    QStringLiteral("statusline-setup"),
    QStringLiteral("claude-code-guide"),
};

const QString kNoteOpen  = QStringLiteral("<msga-teammates>");
const QString kNoteClose = QStringLiteral("</msga-teammates>");

// The header line's name and id: "Data analyst (msga: data-analyst)" — or, as
// sessions started before ids were written have it, a built-in's English
// name alone ("Engineer").
RoleMark parseHeaderLine(const QString &line) {
    static const QRegularExpression kWithId(
        QStringLiteral("^(.*?)\\s*\\(msga: ([a-z0-9-]+)\\)\\s*$")
    );
    if (const auto m = kWithId.match(line); m.hasMatch())
        return {m.captured(2), m.captured(1).trimmed()};
    const QString name = line.trimmed();
    for (const Role &r : builtInRoles())
        if (name == r.promptName)
            return {r.id, name};
    return {};
}

// A line from a teammate file's header: "key: value".
QHash<QString, QString> parseFields(const QString &block) {
    QHash<QString, QString> out;
    for (const QString &l : block.split(QLatin1Char('\n'))) {
        const qsizetype colon = l.indexOf(QLatin1Char(':'));
        if (colon > 0)
            out.insert(l.left(colon).trimmed(), l.mid(colon + 1).trimmed());
    }
    return out;
}

QString oneLine(const QString &s) {
    return s.simplified();
}

} // namespace

QString roleHeader(const QString &name, const QString &id) {
    return kHeaderPrefix + name + QStringLiteral(" (msga: ") + id + QLatin1Char(')');
}

QString appendedPrompt(const Role &role) {
    const QString body = role.prompt.trimmed();
    if (body.isEmpty())
        return {};
    return roleHeader(role.promptName.isEmpty() ? role.name : role.promptName, role.id) +
           QLatin1Char('\n') + body;
}

QString subagentsJson(const std::vector<Role> &roles) {
    QJsonObject agents;
    for (const Role &r : roles) {
        const QString prompt = appendedPrompt(r);
        if (prompt.isEmpty() || kTakenTypes.contains(r.id))
            continue;
        agents.insert(
            r.id,
            QJsonObject{
                {QStringLiteral("description"),
                 QStringLiteral("%1, a teammate (mentioned as @claude:role:%2). %3")
                     .arg(
                         r.promptName.isEmpty() ? r.name : r.promptName,
                         r.id,
                         oneLine(r.description)
                     )
                     .trimmed()},
                {QStringLiteral("prompt"), prompt},
            }
        );
    }
    return agents.isEmpty()
               ? QString()
               : QString::fromUtf8(QJsonDocument(agents).toJson(QJsonDocument::Compact));
}

QString
teammateNote(const QString &prompt, const std::function<const Role *(const QString &)> &find) {
    static const QRegularExpression kMention(
        QStringLiteral("(?<![\\w@/:.-])@claude:role:([a-z0-9-]+)(?![\\w-])")
    );
    QStringList seen;
    QString     body;
    for (auto it = kMention.globalMatch(prompt); it.hasNext();) {
        const QString id = it.next().captured(1);
        if (seen.contains(id))
            continue;
        seen << id;
        const Role   *r      = find(id);
        const QString append = r ? appendedPrompt(*r) : QString();
        if (append.isEmpty())
            continue;
        const QString name = r->promptName.isEmpty() ? r->name : r->promptName;
        body += kTakenTypes.contains(id)
                    ? QStringLiteral(
                          "\n@claude:role:%1 is the teammate %2. To spawn it, use the "
                          "Agent tool with subagent_type \"claude\" and begin the "
                          "prompt with its role, verbatim:\n"
                      )
                          .arg(id, name)
                    : QStringLiteral(
                          "\n@claude:role:%1 is the teammate %2. To spawn it, use the "
                          "Agent tool with subagent_type \"%1\". If there is no such "
                          "type, use subagent_type \"claude\" instead and begin the "
                          "prompt with its role, verbatim:\n"
                      )
                          .arg(id, name);
        body += append + QLatin1Char('\n');
    }
    if (body.isEmpty())
        return {};
    return QStringLiteral("\n\n") + kNoteOpen + body + kNoteClose;
}

QString withoutTeammateNote(const QString &prompt) {
    const qsizetype open = prompt.lastIndexOf(QStringLiteral("\n\n") + kNoteOpen);
    if (open < 0 || !prompt.trimmed().endsWith(kNoteClose))
        return prompt;
    return prompt.left(open);
}

QString roleInAgentPrompt(const QString &prompt) {
    for (const QString &line : prompt.split(QLatin1Char('\n')))
        if (line.startsWith(kHeaderPrefix))
            if (const RoleMark m = parseHeaderLine(line.mid(kHeaderPrefix.size())); !m.id.isEmpty())
                return m.id;
    static const QRegularExpression kSelfWritten(
        QStringLiteral("^\\s*Role:\\s*([A-Za-z][A-Za-z0-9-]*)\\b")
    );
    if (const auto m = kSelfWritten.match(prompt); m.hasMatch())
        return m.captured(1).toLower();
    return {};
}

RoleMark roleInSystemPrompt(const QJsonArray &systemPrompt) {
    for (qsizetype i = systemPrompt.size() - 1; i >= 0; --i) {
        const QString part = systemPrompt.at(i).toString();
        if (part.startsWith(kHeaderPrefix))
            return parseHeaderLine(part.mid(kHeaderPrefix.size()).section(QLatin1Char('\n'), 0, 0));
    }
    return {};
}

RoleMark roleInTranscriptBytes(const QByteArray &bytes) {
    // In the JSON the part is a string of its own: `"# Your role: Engineer\n…`.
    static const QByteArray kNeedle = '"' + kHeaderPrefix.toUtf8();
    const qsizetype         at      = bytes.lastIndexOf(kNeedle);
    if (at < 0)
        return {};
    // Our part goes on after the header line; a prompt someone typed that
    // happens to read like it ends there instead.
    const QByteArray rest = bytes.mid(at + kNeedle.size(), 400);
    QByteArray       line;
    for (qsizetype i = 0; i < rest.size(); ++i) {
        if (rest[i] == '"')
            return {}; // the string ended on this line
        if (rest[i] == '\\') {
            if (i + 1 < rest.size() && rest[i + 1] == 'n')
                return parseHeaderLine(QString::fromUtf8(line));
            if (i + 1 < rest.size())
                line += rest[++i]; // \" or \\ in the name
            continue;
        }
        line += rest[i];
    }
    return {}; // cut off before the line ended
}

const std::vector<Role> &builtInRoles() {
    // Built on first use, after the translator is installed.
    static const std::vector<Role> all = [] {
        std::vector<Role> out;
        for (const Spec &s : kSpecs) {
            Role r;
            r.id          = QString::fromLatin1(s.id);
            r.name        = shownName(r.id);
            r.description = shownDescription(r.id);
            r.avatarUrl   = QString::fromLatin1(s.avatar);
            r.glyph       = QString::fromLatin1(s.glyph);
            r.color       = QColor::fromRgb(QRgb(s.color));
            r.prompt      = QString::fromUtf8(s.prompt);
            r.promptName  = QString::fromLatin1(s.englishName);
            r.builtIn     = true;
            out.push_back(std::move(r));
        }
        return out;
    }();
    return all;
}

// ── Team ─────────────────────────────────────────────────────────────────────

Team::Team(QString dir) : _dir(std::move(dir)) {
    load();
}

void Team::load() {
    _roles = builtInRoles();
    std::vector<Role> added;
    const QDir        dir(_dir);
    for (const QString &file : dir.entryList({QStringLiteral("*.md")}, QDir::Files)) {
        QFile f(dir.filePath(file));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QString text = QString::fromUtf8(f.readAll());
        if (!text.startsWith(QLatin1String("---\n")))
            continue;
        const qsizetype close = text.indexOf(QLatin1String("\n---"), 4);
        if (close < 0)
            continue;
        const auto    fields = parseFields(text.mid(4, close - 4));
        const QString id     = file.chopped(3);
        Role          r;
        const auto    base =
            std::find_if(_roles.begin(), _roles.end(), [&](const Role &b) { return b.id == id; });
        if (base != _roles.end())
            r = *base;
        r.id          = id;
        r.name        = fields.value(QStringLiteral("name"), r.name);
        r.description = fields.value(QStringLiteral("description"), r.description);
        r.glyph       = fields.value(QStringLiteral("glyph"), r.glyph);
        if (const QColor c(fields.value(QStringLiteral("color"))); c.isValid())
            r.color = c;
        r.created = fields.value(QStringLiteral("created")).toLongLong();
        r.removed = !r.builtIn && fields.value(QStringLiteral("removed")) == QLatin1String("true");
        // The prompt: everything after the closing line.
        const qsizetype body = text.indexOf(QLatin1Char('\n'), close + 4);
        r.prompt             = body < 0 ? QString() : text.mid(body + 1).trimmed();
        if (r.name.isEmpty())
            continue;
        if (base != _roles.end()) {
            r.edited     = true;
            r.promptName = r.name == base->name ? base->promptName : r.name;
            r.avatarUrl  = avatarUrlFor(r);
            *base        = r;
        } else {
            r.promptName = r.name;
            r.avatarUrl  = avatarUrlFor(r);
            added.push_back(std::move(r));
        }
    }
    std::sort(added.begin(), added.end(), [](const Role &a, const Role &b) {
        return a.created != b.created ? a.created < b.created : a.id < b.id;
    });
    for (auto &r : added)
        _roles.push_back(std::move(r));
}

std::vector<Role> Team::listed() const {
    std::vector<Role> out;
    for (const Role &r : _roles)
        if (!r.removed)
            out.push_back(r);
    return out;
}

const Role *Team::find(const QString &id) const {
    for (const Role &r : _roles)
        if (r.id == id)
            return &r;
    return nullptr;
}

Role Team::resolve(const QString &id, const QString &nameHint) const {
    if (id.isEmpty())
        return generalist();
    if (const Role *r = find(id))
        return *r;
    // A former teammate: named as its sessions have it, in a plain grey.
    Role r;
    r.id         = id;
    r.name       = !nameHint.isEmpty() ? nameHint : _formers.value(id, id);
    r.promptName = r.name;
    r.glyph      = QStringLiteral("briefcase");
    r.color      = AvatarGlyphs::colors().back();
    r.former     = true;
    r.avatarUrl  = avatarUrlFor(r);
    return r;
}

bool Team::noteFormer(const QString &id, const QString &name) {
    if (id.isEmpty() || find(id) || name.isEmpty() || _formers.value(id) == name)
        return false;
    _formers.insert(id, name);
    return true;
}

QString Team::avatarUrlFor(const Role &role) const {
    // A built-in as it comes keeps its bundled picture.
    for (const Role &b : builtInRoles())
        if (b.id == role.id && b.glyph == role.glyph && b.color == role.color)
            return b.avatarUrl;
    // Others are drawn once into a file named after what it shows — no
    // staleness to manage, and the image cache keys by url.
    const QString name =
        QStringLiteral("%1-%2.svg")
            .arg(
                AvatarGlyphs::hasGlyph(role.glyph) ? role.glyph : AvatarGlyphs::glyphs().front().id
            )
            .arg(role.color.name().mid(1));
    const QString path = _dir + QStringLiteral("/avatars/") + name;
    if (!QFileInfo::exists(path)) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(AvatarGlyphs::svg(role.glyph, role.color));
            f.commit();
        }
    }
    return QUrl::fromLocalFile(path).toString();
}

QString Team::newId(const QString &name) const {
    QString base;
    for (const QChar c : name.toLower()) {
        if ((c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9'))
            base += c;
        else if (!base.isEmpty() && !base.endsWith(QLatin1Char('-')))
            base += QLatin1Char('-');
    }
    while (base.endsWith(QLatin1Char('-')))
        base.chop(1);
    base = base.left(40);
    if (base.isEmpty())
        base = QStringLiteral("teammate");
    // Never one a session could already carry: taken ids, formers, old files.
    QString id = base;
    for (int n = 2; find(id) || _formers.contains(id) ||
                    QFileInfo::exists(_dir + QLatin1Char('/') + id + QStringLiteral(".md"));
         ++n)
        id = base + QLatin1Char('-') + QString::number(n);
    return id;
}

bool Team::write(const Role &r, QString *error) {
    QDir().mkpath(_dir);
    QSaveFile f(_dir + QLatin1Char('/') + r.id + QStringLiteral(".md"));
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QString text = QStringLiteral("---\n");
    text += QStringLiteral("name: ") + oneLine(r.name) + QLatin1Char('\n');
    text += QStringLiteral("description: ") + oneLine(r.description) + QLatin1Char('\n');
    text += QStringLiteral("glyph: ") + r.glyph + QLatin1Char('\n');
    text += QStringLiteral("color: ") + r.color.name() + QLatin1Char('\n');
    if (r.created > 0)
        text += QStringLiteral("created: ") + QString::number(r.created) + QLatin1Char('\n');
    if (r.removed)
        text += QStringLiteral("removed: true\n");
    text += QStringLiteral("---\n") + r.prompt.trimmed() + QLatin1Char('\n');
    f.write(text.toUtf8());
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

QString Team::save(Role role, QString *error) {
    role.name = oneLine(role.name);
    if (role.name.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("claude_code", "A teammate needs a name.");
        return {};
    }
    if (!AvatarGlyphs::hasGlyph(role.glyph))
        role.glyph = AvatarGlyphs::glyphs().front().id;
    if (!role.color.isValid())
        role.color = AvatarGlyphs::colors().front();
    const Role *old = role.id.isEmpty() ? nullptr : find(role.id);
    if (role.id.isEmpty()) {
        role.id      = newId(role.name);
        role.created = QDateTime::currentMSecsSinceEpoch();
    } else if (!old) {
        if (error)
            *error = QCoreApplication::translate("claude_code", "That teammate is gone.");
        return {};
    } else {
        role.created = old->created;
        role.builtIn = old->builtIn;
    }
    role.removed = false;
    if (!write(role, error))
        return {};
    load();
    return role.id;
}

bool Team::remove(const QString &id) {
    const Role *r = find(id);
    if (!r || r->builtIn || r->removed)
        return false;
    Role gone    = *r;
    gone.removed = true;
    if (!write(gone, nullptr))
        return false;
    load();
    return true;
}

bool Team::restore(const QString &id) {
    const Role *r = find(id);
    if (!r || !r->builtIn || !r->edited)
        return false;
    if (!QFile::remove(_dir + QLatin1Char('/') + id + QStringLiteral(".md")))
        return false;
    load();
    return true;
}

} // namespace claude_code
