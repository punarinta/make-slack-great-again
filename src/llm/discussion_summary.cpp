// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "llm/discussion_summary.h"

#include <QLocale>
#include <QStringList>

namespace DiscussionSummary {
namespace {

// Transcript bounds — keep the request cheap and inside any model's context.
// A single pathological message (pasted log) can't eat the budget, and an
// over-long span drops its middle: the head anchors what the discussion was
// about, the tail holds the conclusions a summary needs most.
constexpr qsizetype kMaxEntryChars      = 600;
constexpr qsizetype kMaxTranscriptChars = 24000;

QString entryLine(const Entry &e) {
    QString text = e.text;
    text.replace(QChar('\n'), QStringLiteral(" "));
    text = text.simplified();
    if (text.size() > kMaxEntryChars)
        text = text.left(kMaxEntryChars - 1) + QChar(0x2026);
    QString line;
    if (e.threadReply)
        line += QStringLiteral("    ↳ "); // "↳" marks a thread reply
    if (!e.author.isEmpty())
        line += e.author + QStringLiteral(": ");
    return line + text;
}

// English name of the target language ("ja" → "Japanese"); instructing the
// model in English with the language *named* is more reliable than a bare
// ISO code. Unknown/empty codes fall back to English.
QString languageName(const QString &code) {
    const QLocale::Language lang = QLocale::codeToLanguage(code, QLocale::ISO639Part1);
    if (lang == QLocale::AnyLanguage)
        return QStringLiteral("English");
    return QLocale::languageToString(lang);
}

} // namespace

QString modelForProvider(const QString &providerId) {
    if (providerId == QStringLiteral("anthropic"))
        return QStringLiteral("claude-haiku-4-5");
    if (providerId == QStringLiteral("openai"))
        return QStringLiteral("gpt-5.4-nano");
    return {};
}

Llm::Request buildRequest(const std::vector<Entry> &entries, const QString &languageCode) {
    QStringList lines;
    lines.reserve(static_cast<qsizetype>(entries.size()));
    for (const Entry &e : entries)
        lines << entryLine(e);

    qsizetype total = 0;
    for (const QString &l : lines)
        total += l.size() + 1;
    if (total > kMaxTranscriptChars) {
        // Keep the head and the tail, drop the middle.
        QStringList head, tail;
        qsizetype   headSize = 0, tailSize = 0;
        qsizetype   i = 0, j = lines.size() - 1;
        while (i <= j) {
            if (headSize <= tailSize) {
                if (headSize + lines[i].size() > kMaxTranscriptChars / 2)
                    break;
                headSize += lines[i].size() + 1;
                head << lines[i++];
            } else {
                if (tailSize + lines[j].size() > kMaxTranscriptChars / 2)
                    break;
                tailSize += lines[j].size() + 1;
                tail.prepend(lines[j--]);
            }
        }
        const qsizetype omitted = j - i + 1;
        lines                   = head;
        lines << QStringLiteral("[… %1 messages omitted …]").arg(omitted);
        lines += tail;
    }

    const QString language = languageName(languageCode);

    Llm::Request req;
    req.maxTokens = 512;
    req.system    = QStringLiteral(
                     "You summarize workplace chat discussions.\n"
                        "Write in %1 only, no matter what language the transcript is in.\n"
                        "Use plain, everyday language — short sentences, simple words, like a "
                        "colleague catching someone up. No corporate or bureaucratic phrasing.\n"
                        "Say what the discussion was about and the details that matter: anything "
                        "decided, done, or left open. A short paragraph is usually enough. Use "
                        "Markdown bullet points or bold labels only when the content genuinely "
                        "calls for them — never force a fixed template or add a section that "
                        "would be empty.\n"
                        "Emphasize the key stuff with Markdown: bold (**…**) the few words that "
                        "matter most — decisions, deadlines, names, numbers — so the summary can "
                        "be skimmed. Emphasis, not decoration: a handful of bolded phrases, not "
                        "whole sentences.\n"
                        "Keep the whole summary under 100 words. No preamble, no closing remarks."
    )
                     .arg(language);
    // The language instruction is repeated AFTER the transcript: small models
    // otherwise drift into the transcript's language — a system-prompt line
    // thousands of tokens back loses to the content in front of the answer.
    req.messages = {
        {Llm::Message::Role::User,
         QStringLiteral(
             "Summarize the following discussion transcript. Lines starting with "
             "\"↳\" are thread replies to the message above them.\n\n"
         ) + lines.join(QChar('\n')) +
             QStringLiteral(
                 "\n\nWrite the summary in %1, even if the transcript is in a "
                 "different language. Keep the wording simple and conversational."
             )
                 .arg(language)},
    };
    return req;
}

} // namespace DiscussionSummary
