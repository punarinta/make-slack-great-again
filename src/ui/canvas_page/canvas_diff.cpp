// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "canvas_diff.h"

#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFrame>
#include <QTextList>
#include <QTextTable>

namespace CanvasDiff {

QString normalizeMd(QString md) {
    // Custom-emoji inline images carry an "emoji:<name>" src so they survive the
    // HTML round-trip; turn them back into ":name:" shortcodes — both for the
    // wire form sent to Slack and so an untouched emoji section diffs equal on
    // the base and document sides.
    static const QRegularExpression kEmojiImg(
        QStringLiteral("!\\[[^\\]]*\\]\\(emoji:([^)\\s]+)\\)")
    );
    md.replace(kEmojiImg, QStringLiteral(":\\1:"));

    // Drop inline images whose URL is relative/host-less — Slack canvas HTML
    // references embedded pictures as "/collab-slack-blob/<blob>/<fileId>", which
    // cannot round-trip to canvas markdown (Slack needs a real uploaded-file ref).
    // Sending one back breaks the save, so strip it here; the picture's own
    // section is never rewritten by the section diff, so it survives on the
    // server. Equality on both the base and document sides stays consistent
    // because both run through normalizeMd.
    static const QRegularExpression kRelImg(QStringLiteral("!\\[[^\\]]*\\]\\(/[^)\\s]*\\)"));
    md.remove(kRelImg);

    QStringList lines = md.split('\n');
    for (auto &l : lines) {
        while (!l.isEmpty() && (l.back() == ' ' || l.back() == '\t'))
            l.chop(1);
    }
    md = lines.join('\n');
    static const QRegularExpression kBlankRuns("\n{3,}");
    md.replace(kBlankRuns, "\n\n");
    return md.trimmed();
}

static QString htmlToMd(const QString &html) {
    QTextDocument doc;
    doc.setHtml(html);
    return normalizeMd(doc.toMarkdown(QTextDocument::MarkdownDialectGitHub));
}

std::pair<QString, QString> splitTitleH1(const QString &html, const QStringList &titleCandidates) {
    static const QRegularExpression re(
        QStringLiteral(
            "\\A\\s*(?:<div[^>]*class=\"quip-canvas-content\"[^>]*>)?\\s*"
            "(<h1[^>]*>(.*?)</h1>)"
        ),
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption
    );
    const auto m = re.match(html);
    if (!m.hasMatch())
        return {{}, html};
    const QString text    = QTextDocumentFragment::fromHtml(m.captured(2)).toPlainText().trimmed();
    bool          matches = false;
    for (const auto &cand : titleCandidates)
        matches = matches || (!cand.trimmed().isEmpty() && cand.trimmed() == text);
    if (!matches)
        return {{}, html};
    QString rest = html;
    rest.remove(m.capturedStart(1), m.capturedLength(1));
    return {text, rest};
}

// ── Base-side parsing (server HTML) ───────────────────────────────────────────

namespace {

QString attrId(const QString &openingTag) {
    static const QRegularExpression re("\\bid=['\"]([^'\"]+)['\"]");
    const auto                      m = re.match(openingTag);
    return m.hasMatch() ? m.captured(1) : QString();
}

// End index (exclusive) of the element whose opening tag starts at `start`,
// counting nested same-name tags. -1 when unbalanced.
int elementEnd(const QString &html, int start, const QString &tag) {
    const QRegularExpression open(
        QStringLiteral("<%1\\b").arg(tag), QRegularExpression::CaseInsensitiveOption
    );
    const QRegularExpression close(
        QStringLiteral("</%1\\s*>").arg(tag), QRegularExpression::CaseInsensitiveOption
    );
    int depth = 0;
    int pos   = start;
    while (pos < html.size()) {
        const auto mOpen  = open.match(html, pos);
        const auto mClose = close.match(html, pos);
        if (!mClose.hasMatch())
            return -1;
        if (mOpen.hasMatch() && mOpen.capturedStart() < mClose.capturedStart()) {
            ++depth;
            pos = mOpen.capturedEnd();
        } else {
            --depth;
            pos = mClose.capturedEnd();
            if (depth == 0)
                return pos;
        }
    }
    return -1;
}

} // namespace

std::optional<std::vector<Chunk>> parseBaseChunks(const QString &bodyHtml) {
    QString html = bodyHtml;

    // Drop the quip wrapper if present.
    static const QRegularExpression wrapOpen(
        QStringLiteral("\\A\\s*<div[^>]*class=\"quip-canvas-content\"[^>]*>"),
        QRegularExpression::CaseInsensitiveOption
    );
    if (const auto m = wrapOpen.match(html); m.hasMatch()) {
        html.remove(0, m.capturedEnd());
        static const QRegularExpression wrapClose(QStringLiteral("</div>\\s*\\z"));
        html.remove(wrapClose);
    }

    static const QRegularExpression tagRe(
        QStringLiteral("<([a-zA-Z][a-zA-Z0-9]*)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption
    );

    std::vector<Chunk> chunks;
    int                pos = 0;
    while (pos < html.size()) {
        while (pos < html.size() && html.at(pos).isSpace())
            ++pos;
        if (pos >= html.size())
            break;

        const auto m = tagRe.match(html, pos);
        if (!m.hasMatch() || m.capturedStart() != pos)
            return std::nullopt; // stray text at top level — unknown structure
        const QString tag     = m.captured(1).toLower();
        const QString opening = m.captured(0);

        // Top-level inline image (void element — no closing tag, so elementEnd
        // would fail). Its markdown is a relative blob ref that normalizeMd
        // strips to nothing, so it becomes an empty chunk that's dropped below —
        // i.e. the image is treated as invisible context. Handling it here (vs.
        // bailing to a whole-document replace) keeps a picture-bearing canvas on
        // the surgical section-diff path, so the image section is never rewritten.
        if (tag == "img") {
            Chunk imgChunk{Chunk::Kind::Para, attrId(opening), false, htmlToMd(opening)};
            if (!imgChunk.md.isEmpty())
                chunks.push_back(std::move(imgChunk));
            pos = m.capturedEnd();
            continue;
        }

        const int end = elementEnd(html, pos, tag);
        if (end < 0)
            return std::nullopt;
        const QString element = html.mid(pos, end - pos);

        Chunk chunk;
        if (tag.size() == 2 && tag[0] == 'h' && tag[1].isDigit()) {
            chunk = {Chunk::Kind::Heading, attrId(opening), false, htmlToMd(element)};
        } else if (tag == "p") {
            chunk = {Chunk::Kind::Para, attrId(opening), false, htmlToMd(element)};
        } else if (tag == "div" || tag == "ul" || tag == "ol") {
            // List wrapper: the <ul> carries the section id.
            static const QRegularExpression ulRe(
                QStringLiteral("<[uo]l\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption
            );
            const auto ulM = ulRe.match(element);
            if (!ulM.hasMatch())
                return std::nullopt;
            chunk = {Chunk::Kind::List, attrId(ulM.captured(0)), false, htmlToMd(element)};
        } else if (tag == "blockquote") {
            chunk = {Chunk::Kind::Quote, {}, true, htmlToMd(element)};
        } else if (tag == "table") {
            chunk = {Chunk::Kind::Table, {}, true, htmlToMd(element)};
        } else {
            return std::nullopt; // <hr>, embeds, … — fall back to whole-doc save
        }

        if (!chunk.fragile && chunk.id.isEmpty())
            return std::nullopt; // section without an id can't be addressed
        // Qt's HTML import drops empty <p> spacer sections entirely, so they
        // can never appear on the document side — exclude them here too or
        // every diff would try to delete them.
        if (!(chunk.kind == Chunk::Kind::Para && chunk.md.isEmpty()))
            chunks.push_back(std::move(chunk));
        pos = end;
    }
    return chunks;
}

// ── Document-side chunking (edited QTextDocument) ─────────────────────────────

namespace {

// List/table groups: extract via the selection's HTML round-trip (lists and
// tables survive it; block-level formats like heading level do not).
QString rangeMd(QTextDocument *doc, int from, int to) {
    QTextCursor c(doc);
    c.setPosition(from);
    c.setPosition(to, QTextCursor::KeepAnchor);
    QTextDocument tmp;
    tmp.setHtml(c.selection().toHtml());
    return normalizeMd(tmp.toMarkdown(QTextDocument::MarkdownDialectGitHub));
}

// Heading/paragraph/quote groups: rebuild the blocks verbatim. Neither
// insertFragment (merges away the first block's format) nor the HTML
// round-trip (drops heading/quote levels) preserves block formats reliably.
QString blocksMd(QTextDocument *doc, int from, int to) {
    QTextDocument tmp;
    QTextCursor   tc(&tmp);

    bool first = true;
    for (QTextBlock block = doc->findBlock(from); block.isValid() && block.position() <= to;
         block            = block.next()) {
        if (first) {
            tc.setBlockFormat(block.blockFormat());
            first = false;
        } else {
            tc.insertBlock(block.blockFormat(), QTextCharFormat());
        }
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid())
                continue;
            if (frag.charFormat().isImageFormat())
                tc.insertImage(frag.charFormat().toImageFormat());
            else
                tc.insertText(frag.text(), frag.charFormat());
        }
    }
    return normalizeMd(tmp.toMarkdown(QTextDocument::MarkdownDialectGitHub));
}

} // namespace

std::vector<Chunk> documentChunks(QTextDocument *doc) {
    // Pass 1: group root-level items into [kind, fragile, from, to) ranges.
    struct Group {
        Chunk::Kind kind;
        bool        fragile;
        int         from, to;
        const void *list; // grouping key: blocks of one QTextList = one chunk
    };
    std::vector<Group> groups;

    QTextFrame *root = doc->rootFrame();
    for (auto it = root->begin(); !it.atEnd(); ++it) {
        if (QTextFrame *frame = it.currentFrame()) {
            const bool isTable = qobject_cast<QTextTable *>(frame) != nullptr;
            // Non-table frames don't occur in canvas content; treat as fragile.
            groups.push_back(
                {isTable ? Chunk::Kind::Table : Chunk::Kind::Quote,
                 true,
                 frame->firstPosition() - 1,
                 frame->lastPosition() + 1,
                 nullptr}
            );
            continue;
        }

        const QTextBlock block = it.currentBlock();
        if (!block.isValid())
            continue;
        const auto fmt  = block.blockFormat();
        const int  from = block.position();
        const int  to   = block.position() + block.length() - 1;

        if (QTextList *list = block.textList()) {
            if (!groups.empty() && groups.back().kind == Chunk::Kind::List &&
                groups.back().list == list) {
                groups.back().to = to; // extend the current list chunk
            } else {
                groups.push_back({Chunk::Kind::List, false, from, to, list});
            }
            continue;
        }

        if (fmt.intProperty(QTextFormat::BlockQuoteLevel) > 0) {
            if (!groups.empty() && groups.back().kind == Chunk::Kind::Quote &&
                groups.back().list == nullptr && groups.back().fragile) {
                groups.back().to = to; // consecutive quote lines = one blockquote
            } else {
                groups.push_back({Chunk::Kind::Quote, true, from, to, nullptr});
            }
            continue;
        }

        const auto kind = fmt.headingLevel() > 0 ? Chunk::Kind::Heading : Chunk::Kind::Para;
        groups.push_back({kind, false, from, to, nullptr});
    }

    // Pass 2: render each group to markdown. Empty paragraphs are excluded on
    // both sides (see parseBaseChunks).
    std::vector<Chunk> chunks;
    chunks.reserve(groups.size());
    for (const auto &g : groups) {
        const bool viaHtml = g.kind == Chunk::Kind::List || g.kind == Chunk::Kind::Table;
        Chunk      chunk{
            g.kind,
                 {},
            g.fragile,
            viaHtml ? rangeMd(doc, g.from, g.to) : blocksMd(doc, g.from, g.to)
        };
        if (!(chunk.kind == Chunk::Kind::Para && chunk.md.isEmpty()))
            chunks.push_back(std::move(chunk));
    }
    return chunks;
}

// ── Diff ──────────────────────────────────────────────────────────────────────

std::optional<std::vector<CanvasChange>>
diff(const std::vector<Chunk> &base, const std::vector<Chunk> &current) {
    const int n = int(base.size());
    const int m = int(current.size());

    const auto eq = [&](int i, int j) {
        return base[i].kind == current[j].kind && base[i].md == current[j].md;
    };

    // LCS table
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (int i = n - 1; i >= 0; --i)
        for (int j = m - 1; j >= 0; --j)
            dp[i][j] = eq(i, j) ? dp[i + 1][j + 1] + 1 : std::max(dp[i + 1][j], dp[i][j + 1]);

    std::vector<CanvasChange> inserts, replaces, deletes;

    const auto safeMd = [](const QString &md) {
        // Slack rejects empty document_content; a lone space yields an empty line.
        return md.isEmpty() ? QStringLiteral(" ") : md;
    };

    int i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && eq(i, j)) {
            ++i;
            ++j;
            continue;
        }
        // Maximal unmatched gap [i, bi) × [j, cj)
        int bi = i, cj = j;
        while (bi < n || cj < m) {
            if (bi < n && cj < m && eq(bi, cj) && dp[bi][cj] == dp[i][j])
                break; // next match reached
            if (bi < n && dp[bi + 1][cj] == dp[i][j]) {
                ++bi;
                continue;
            }
            if (cj < m && dp[bi][cj + 1] == dp[i][j]) {
                ++cj;
                continue;
            }
            break;
        }
        const int lenB = bi - i, lenC = cj - j;
        const int pairs = std::min(lenB, lenC);

        for (int p = 0; p < pairs; ++p) {
            const Chunk &b = base[i + p];
            if (b.fragile || b.id.isEmpty())
                return std::nullopt;
            replaces.push_back(
                {.op        = CanvasChange::Op::ReplaceSection,
                 .sectionId = b.id,
                 .markdown  = safeMd(current[j + p].md)}
            );
        }
        for (int p = pairs; p < lenB; ++p) {
            const Chunk &b = base[i + p];
            if (b.fragile || b.id.isEmpty())
                return std::nullopt;
            deletes.push_back(
                {.op = CanvasChange::Op::DeleteSection, .sectionId = b.id, .markdown = {}}
            );
        }
        if (lenC > pairs) {
            // Anchor for the extra inserted chunks. Ops are sent inserts-first,
            // so anchoring on a chunk that is later replaced/deleted is fine —
            // its id is still alive when the insert applies.
            QString anchorAfter;  // insert_after this id, REVERSED order
            QString anchorBefore; // insert_before this id, natural order
            if (lenB > 0 && !base[i + lenB - 1].fragile)
                anchorAfter = base[i + lenB - 1].id;
            else if (bi < n && !base[bi].fragile && !base[bi].id.isEmpty())
                anchorBefore = base[bi].id;
            else if (i > 0 && !base[i - 1].fragile && !base[i - 1].id.isEmpty())
                anchorAfter = base[i - 1].id;
            else
                return std::nullopt;

            if (!anchorBefore.isEmpty()) {
                for (int p = pairs; p < lenC; ++p)
                    inserts.push_back(
                        {.op        = CanvasChange::Op::InsertBefore,
                         .sectionId = anchorBefore,
                         .markdown  = safeMd(current[j + p].md)}
                    );
            } else {
                for (int p = lenC - 1; p >= pairs; --p)
                    inserts.push_back(
                        {.op        = CanvasChange::Op::InsertAfter,
                         .sectionId = anchorAfter,
                         .markdown  = safeMd(current[j + p].md)}
                    );
            }
        }
        i = bi;
        j = cj;
    }

    std::vector<CanvasChange> ops;
    ops.reserve(inserts.size() + replaces.size() + deletes.size());
    ops.insert(ops.end(), inserts.begin(), inserts.end());
    ops.insert(ops.end(), replaces.begin(), replaces.end());
    ops.insert(ops.end(), deletes.begin(), deletes.end());
    return ops;
}

} // namespace CanvasDiff
