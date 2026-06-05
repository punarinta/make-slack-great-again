// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "mrkdwn_parser.h"

#include <QRegularExpression>

namespace MrkdwnParser {

// Single-pass scanner. We build plain text while recording entity spans.
// Offsets in TextEntity refer to positions in the *output* plain text, not the mrkdwn.

struct Builder {
    QString              text;
    std::vector<TextEntity> entities;

    void appendPlain(QChar c) { text.append(c); }
    void appendPlain(const QString &s) { text.append(s); }

    void addSpan(EntityType type, int start, const QString &data = {}) {
        entities.push_back(TextEntity{
            type,
            start,
            static_cast<int>(text.size()) - start,
            data
        });
    }
};

// Try to consume a paired delimiter (*,_,~,`) starting at pos.
// Returns the closing pos+1 if matched, -1 otherwise.
static int findClose(const QString &src, int start, QChar delim) {
    for (int i = start; i < src.size(); ++i) {
        if (src[i] == delim && (i == 0 || src[i-1] != '\\'))
            return i + 1;
        // Don't cross newlines for inline marks
        if (src[i] == '\n') return -1;
    }
    return -1;
}

// Try to consume a fenced code block starting at pos (pos points just after the opening ```).
// Returns the closing position after the closing ```, or -1.
static int findCodeFenceClose(const QString &src, int pos) {
    while (pos < src.size() - 2) {
        if (src[pos] == '`' && src[pos+1] == '`' && src[pos+2] == '`')
            return pos + 3;
        ++pos;
    }
    return -1;
}

TextWithEntities parse(const QString &mrkdwn) {
    Builder b;
    int i = 0;
    const int n = mrkdwn.size();

    while (i < n) {
        QChar c = mrkdwn[i];

        // ── Code fence ``` ──
        if (c == '`' && i+2 < n && mrkdwn[i+1] == '`' && mrkdwn[i+2] == '`') {
            int contentStart = i + 3;
            // Skip optional language hint on same line
            int lineEnd = mrkdwn.indexOf('\n', contentStart);
            if (lineEnd != -1 && lineEnd < n) contentStart = lineEnd + 1;
            int closePos = findCodeFenceClose(mrkdwn, contentStart);
            if (closePos != -1) {
                int entityStart = b.text.size();
                b.appendPlain(mrkdwn.mid(contentStart, closePos - 3 - contentStart));
                b.addSpan(EntityType::Pre, entityStart);
                i = closePos;
                continue;
            }
        }

        // ── Inline code ` ──
        if (c == '`') {
            int close = findClose(mrkdwn, i+1, '`');
            if (close != -1) {
                int entityStart = b.text.size();
                b.appendPlain(mrkdwn.mid(i+1, close-1 - (i+1)));
                b.addSpan(EntityType::Code, entityStart);
                i = close;
                continue;
            }
        }

        // ── Bold *text* ──
        if (c == '*') {
            int close = findClose(mrkdwn, i+1, '*');
            if (close != -1) {
                int entityStart = b.text.size();
                // Recurse-parse inner for nested marks? Keep flat for now.
                b.appendPlain(mrkdwn.mid(i+1, close-1 - (i+1)));
                b.addSpan(EntityType::Bold, entityStart);
                i = close;
                continue;
            }
        }

        // ── Italic _text_ ──
        if (c == '_') {
            int close = findClose(mrkdwn, i+1, '_');
            if (close != -1) {
                int entityStart = b.text.size();
                b.appendPlain(mrkdwn.mid(i+1, close-1 - (i+1)));
                b.addSpan(EntityType::Italic, entityStart);
                i = close;
                continue;
            }
        }

        // ── Strikethrough ~text~ ──
        if (c == '~') {
            int close = findClose(mrkdwn, i+1, '~');
            if (close != -1) {
                int entityStart = b.text.size();
                b.appendPlain(mrkdwn.mid(i+1, close-1 - (i+1)));
                b.addSpan(EntityType::Strike, entityStart);
                i = close;
                continue;
            }
        }

        // ── Angular bracket constructs <…> ──
        if (c == '<') {
            int close = mrkdwn.indexOf('>', i+1);
            if (close != -1) {
                auto inner = mrkdwn.mid(i+1, close - i - 1);
                i = close + 1;

                // User mention: <@UXXXXX> or <@UXXXXX|name>
                if (inner.startsWith('@')) {
                    auto parts = inner.mid(1).split('|');
                    auto uid = parts[0];
                    auto label = parts.size() > 1 ? parts[1] : ("@" + uid);
                    int entityStart = b.text.size();
                    b.appendPlain(label);
                    b.addSpan(EntityType::UserMention, entityStart, uid);
                    continue;
                }

                // Channel mention: <#CXXXXX|name>
                if (inner.startsWith('#')) {
                    auto parts = inner.mid(1).split('|');
                    auto cid  = parts[0];
                    auto name = parts.size() > 1 ? parts[1] : cid;
                    int entityStart = b.text.size();
                    b.appendPlain("#" + name);
                    b.addSpan(EntityType::ChannelMention, entityStart, cid);
                    continue;
                }

                // Special commands: <!here>, <!channel>, <!subteam^S|name>
                if (inner.startsWith('!')) {
                    auto cmd = inner.mid(1);
                    if (cmd == "here") {
                        int s = b.text.size();
                        b.appendPlain("@here");
                        b.addSpan(EntityType::HereCommand, s);
                    } else if (cmd == "channel") {
                        int s = b.text.size();
                        b.appendPlain("@channel");
                        b.addSpan(EntityType::ChannelCommand, s);
                    } else {
                        // subteam or unknown: show as @name
                        auto parts = cmd.split('|');
                        auto label = parts.size() > 1 ? parts.last() : cmd;
                        int s = b.text.size();
                        b.appendPlain("@" + label);
                        b.addSpan(EntityType::HereCommand, s, cmd);
                    }
                    continue;
                }

                // Link: <url|label> or <url>
                {
                    auto parts = inner.split('|');
                    auto url   = parts[0];
                    auto label = parts.size() > 1 ? parts[1] : url;
                    int entityStart = b.text.size();
                    b.appendPlain(label);
                    b.addSpan(EntityType::Link, entityStart, url);
                    continue;
                }
            }
        }

        // ── Emoji :name: ──
        if (c == ':') {
            int close = mrkdwn.indexOf(':', i+1);
            if (close != -1 && close > i+1) {
                auto name = mrkdwn.mid(i+1, close - i - 1);
                // Validate: emoji names are [a-z0-9_+-]+
                static QRegularExpression validEmoji("^[a-zA-Z0-9_+\\-]+$");
                if (validEmoji.match(name).hasMatch()) {
                    int entityStart = b.text.size();
                    b.appendPlain(":" + name + ":");
                    b.addSpan(EntityType::Emoji, entityStart, name);
                    i = close + 1;
                    continue;
                }
            }
        }

        // ── Blockquote (> at line start) ──
        // Consume all consecutive >-prefixed lines as a single Blockquote entity.
        if (c == '>' && (i == 0 || mrkdwn[i-1] == '\n')) {
            int entityStart = b.text.size();
            bool first = true;
            while (i < n && mrkdwn[i] == '>') {
                if (!first) b.appendPlain('\n');
                first = false;
                ++i; // skip '>'
                if (i < n && mrkdwn[i] == ' ') ++i; // skip optional space
                // Collect content until end of line
                while (i < n && mrkdwn[i] != '\n')
                    b.appendPlain(mrkdwn[i++]);
                if (i < n) ++i; // skip '\n'
                // Check if next non-empty line continues the quote
                if (i >= n || mrkdwn[i] != '>') break;
            }
            b.addSpan(EntityType::Blockquote, entityStart);
            continue;
        }

        b.appendPlain(c);
        ++i;
    }

    return TextWithEntities{ b.text, b.entities };
}

} // namespace MrkdwnParser
