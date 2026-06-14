// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "canvas_emoji.h"

#include "ui/message_list/message_render.h"

namespace CanvasEmoji {
namespace {

// Expand emoji shortcodes in one text run (never inside a tag).
QString expandInText(const QString &text, const QHash<QString, QString> &customEmoji) {
    if (!text.contains(':'))
        return text;
    const QString px = QString::number(MsgRender::inlineEmojiPx());
    QString       out;
    out.reserve(text.size());
    int       i = 0;
    const int n = text.size();
    while (i < n) {
        if (text[i] != ':') {
            out += text[i++];
            continue;
        }
        int j = i + 1;
        while (j < n && text[j] != ':' && text[j] != ' ' && text[j] != '\n')
            ++j;
        if (j < n && text[j] == ':' && j > i + 1) {
            const QString name = text.mid(i + 1, j - i - 1);
            const auto    er   = MsgRender::resolveEmojiRich(name, customEmoji);
            if (!er.imageUrl.isEmpty()) {
                out += "<img src='emoji:" + name.toHtmlEscaped() + "' width='" + px + "' height='" +
                       px + "'>";
                i = j + 1;
                continue;
            }
            if (!er.unicode.isEmpty() && er.unicode != ":" + name + ":") {
                out += er.unicode;
                i = j + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

} // namespace

QString expandInHtml(const QString &html, const QHash<QString, QString> &customEmoji) {
    if (!html.contains(':'))
        return html;
    QString out;
    out.reserve(html.size());
    int       i = 0;
    const int n = html.size();
    while (i < n) {
        const int lt = html.indexOf('<', i);
        if (lt < 0) {
            out += expandInText(html.mid(i), customEmoji);
            break;
        }
        out += expandInText(html.mid(i, lt - i), customEmoji);
        const int gt = html.indexOf('>', lt);
        if (gt < 0) {
            out += html.mid(lt); // malformed tail; pass through verbatim
            break;
        }
        out += html.mid(lt, gt - lt + 1); // the tag, verbatim
        i = gt + 1;
    }
    return out;
}

} // namespace CanvasEmoji
