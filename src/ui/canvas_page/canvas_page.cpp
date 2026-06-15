// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "canvas_page.h"
#include "canvas_diff.h"
#include "canvas_emoji.h"
#include "session/session.h"
#include "ui/app_dialog/app_dialog.h"
#include "ui/context_menu/context_menu.h"
#include "ui/icon_utils.h"
#include "ui/message_list/message_render.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "util/clipboard.h"
#include "util/emoji.h"
#include "util/mailto_link.h"

#include <QCursor>
#include <QDesktopServices>

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kColumnMaxW  = 1040; // editor column width, matches Slack's measure
constexpr int kSaveDelayMs = 2500; // autosave this long after typing stops

// Normalize Slack's canvas HTML for display, in ways that don't change the
// markdown a section round-trips to (so the section diff stays consistent
// between the base HTML and the edited document):
//  - Inline content images are pinned to a tiny placeholder size (e.g.
//    width='64' height='23'); the official client ignores that and scales them
//    to the column. Drop those size attrs so they render at the (downscaled)
//    resource size. (toMarkdown emits ![alt](src) regardless of size.)
//  - Hyperlinks come as a non-standard <lnk href>…</lnk> tag the rich-text
//    engine doesn't recognize, so they render as dead plain text. Rewrite them
//    to real <a> anchors — now clickable, hoverable, and preserved on save as
//    [text](url) (Qt drops the href entirely for the unknown <lnk> tag, so this
//    is also more faithful, and identical on the base and document sides).
QString prepareCanvasHtml(QString html) {
    static const QRegularExpression lnkOpen(
        QStringLiteral("<lnk\\b"), QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression lnkClose(
        QStringLiteral("</lnk\\s*>"), QRegularExpression::CaseInsensitiveOption
    );
    html.replace(lnkOpen, QStringLiteral("<a"));
    html.replace(lnkClose, QStringLiteral("</a>"));

    static const QRegularExpression imgRe(
        QStringLiteral("<img\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption
    );
    static const QRegularExpression sizeAttr(
        QStringLiteral("\\s(?:width|height)=['\"][^'\"]*['\"]"),
        QRegularExpression::CaseInsensitiveOption
    );
    QString out;
    int     last = 0;
    auto    it   = imgRe.globalMatch(html);
    while (it.hasNext()) {
        const auto m   = it.next();
        QString    tag = m.captured(0);
        if (tag.contains(QLatin1String("collab-slack-blob")))
            tag.replace(sizeAttr, QString());
        out += QStringView(html).mid(last, m.capturedStart() - last);
        out += tag;
        last = m.capturedEnd();
    }
    out += QStringView(html).mid(last);
    return out;
}
} // namespace

// Editable canvas body. QTextBrowser rather than QTextEdit purely for the
// loadResource hook: files.slack.com images need the auth header, so they are
// fetched through the Session and patched into the document as they arrive.
class CanvasEdit : public QTextBrowser {
public:
    explicit CanvasEdit(QWidget *parent) : QTextBrowser(parent) {
        // Don't let the browser navigate the document to the clicked URL (that
        // would replace the canvas); handle the click ourselves and open it in
        // the user's browser/mail client. QTextBrowser still shows the
        // pointing-hand cursor on hover and emits anchorClicked for anchors.
        setOpenLinks(false);
        setOpenExternalLinks(false);
        setFrameShape(QFrame::NoFrame);
        applyReadOnly(false); // editable + link activation on
        // Hover detection (the highlighted() signal) needs mouse tracking in
        // editable mode; without it the link URL wouldn't surface until a drag.
        viewport()->setMouseTracking(true);
        connect(this, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
            if (url.isEmpty())
                return;
            if (url.scheme() == QLatin1String("mailto")) {
                MailtoLink::openOrCopy(url.toString());
                return;
            }
            QDesktopServices::openUrl(url);
        });
    }

    Session *session = nullptr;

    // setReadOnly() rewrites the text-interaction flags (editable mode drops
    // link activation), so always re-add it. Use this instead of setReadOnly()
    // so links stay clickable/hoverable whether the canvas is editable or not.
    void applyReadOnly(bool readOnly) {
        setReadOnly(readOnly);
        setTextInteractionFlags(
            textInteractionFlags() | Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard
        );
    }

    QVariant loadResource(int type, const QUrl &url) override {
        if (type != QTextDocument::ImageResource)
            return QTextBrowser::loadResource(type, url);

        // Three image flavours appear in canvas HTML:
        //  - emoji:<name>  custom-emoji shortcodes embedded so they survive the
        //                  markdown round-trip (resolved to the workspace emoji URL)
        //  - http(s)://…   standard-emoji and remote images (load directly)
        //  - /collab-slack-blob/<blob>/<fileId>  host-less, auth-less references
        //                  to inline images; the trailing segment is a file id we
        //                  resolve through files.info.
        const bool emojiRef = url.scheme() == QLatin1String("emoji");
        const bool blobRef  = url.scheme().isEmpty() && url.path().contains("collab-slack-blob");
        const bool httpRef  = url.scheme().startsWith("http");
        if (!(emojiRef || blobRef || httpRef))
            return QTextBrowser::loadResource(type, url);

        const QString key = url.toString();
        if (const auto it = _images.constFind(key); it != _images.constEnd())
            return *it;
        if (!session || _pending.contains(key))
            return QImage();

        QPointer<CanvasEdit> guard(this);
        // Inline content images are downscaled to the column like the official
        // client; emoji keep their intrinsic inline size.
        const bool           scaleToFit = blobRef || httpRef;
        auto                 onData     = [guard, key, url, scaleToFit](QByteArray data) {
            if (!guard)
                return;
            QImage img;
            if (!img.loadFromData(data))
                return;
            if (scaleToFit) {
                // Scale in *device* pixels and tag the result with the screen's
                // devicePixelRatio so it lays out at the column width but renders
                // at full panel resolution — otherwise a logical-width image is
                // upscaled by the compositor and looks pixelated on HiDPI.
                const qreal dpr  = guard->devicePixelRatioF();
                const int   maxW = int(guard->maxImageWidth() * dpr);
                if (maxW > 0 && img.width() > maxW)
                    img = img.scaledToWidth(maxW, Qt::SmoothTransformation);
                img.setDevicePixelRatio(dpr);
            }
            guard->_images.insert(key, img);
            guard->document()->addResource(QTextDocument::ImageResource, url, img);
            guard->document()->markContentsDirty(0, guard->document()->characterCount());
        };
        auto onErr = [](const QString &) {}; // broken image is fine; no error banner

        if (blobRef) {
            _pending.insert(key);
            session->loadCanvasImage(url.path().section('/', -1), onData, onErr);
        } else if (emojiRef) {
            const QString fetchUrl = MsgRender::resolveEmojiRich(url.path(), session).imageUrl;
            if (!fetchUrl.isEmpty()) {
                _pending.insert(key);
                session->downloadFile(fetchUrl, onData, onErr);
            }
        } else { // httpRef
            _pending.insert(key);
            session->downloadFile(key, onData, onErr);
        }
        return QImage();
    }

    // Width (in logical px) inline content images are scaled down to so they
    // never overflow the editor column.
    int maxImageWidth() const { return qMax(0, viewport()->width() - 2); }

private:
    QHash<QString, QImage> _images;
    QSet<QString>          _pending;
};

namespace {

// "This canvas will be deleted forever" confirmation.
class DeleteCanvasDialog : public AppDialog {
public:
    explicit DeleteCanvasDialog(QWidget *parent)
        : AppDialog(QCoreApplication::translate("CanvasPage", "Delete canvas"), parent) {
        auto *cl = contentLayout();

        auto *warn = new QLabel(
            QCoreApplication::translate(
                "CanvasPage",
                "The canvas will be deleted for everyone in the conversation.\n"
                "This action cannot be undone."
            )
        );
        warn->setWordWrap(true);
        warn->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.secondary)));
        cl->addWidget(warn);

        auto *btnRow = new QHBoxLayout;
        btnRow->setSpacing(8);
        btnRow->addStretch();

        auto *cancelBtn = new QPushButton(QCoreApplication::translate("CanvasPage", "Cancel"));
        auto *deleteBtn =
            new QPushButton(QCoreApplication::translate("CanvasPage", "Delete canvas"));
        cancelBtn->setCursor(Qt::PointingHandCursor);
        deleteBtn->setCursor(Qt::PointingHandCursor);

        const auto &th = Th::c();
        cancelBtn->setStyleSheet(QString(
                                     "QPushButton { border: 1px solid %1; border-radius: 4px;"
                                     " padding: 6px 18px; background: %2; }"
                                     "QPushButton:hover { background: %3; }"
        )
                                     .arg(
                                         Th::qss(th.divider.strong),
                                         Th::qss(th.surface.raised),
                                         Th::qss(th.surface.sunken)
                                     ));
        deleteBtn->setStyleSheet(
            QString(
                "QPushButton { border: none; border-radius: 4px;"
                " padding: 6px 18px; background: %1; color: %2; }"
                "QPushButton:hover { background: %3; }"
            )
                .arg(Th::qss(th.danger.def), Th::qss(th.accent.text), Th::qss(th.danger.hover))
        );

        btnRow->addWidget(cancelBtn);
        btnRow->addWidget(deleteBtn);
        cl->addLayout(btnRow);

        connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
        connect(deleteBtn, &QPushButton::clicked, this, &QDialog::accept);
        updateCard();
    }
};

} // namespace

CanvasPage::CanvasPage(QWidget *parent) : QWidget(parent) {
    setObjectName("canvasPage");
    setAttribute(Qt::WA_StyledBackground);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    _column = new QWidget(this);
    _column->setMaximumWidth(kColumnMaxW);
    auto *col = new QVBoxLayout(_column);
    col->setContentsMargins(16, 56, 16, 24);
    col->setSpacing(12);

    _roNotice = new QLabel(_column); // text set by setReadOnlyUi per cause
    _roNotice->setWordWrap(true);
    _roNotice->hide();
    col->addWidget(_roNotice);

    _title = new QLineEdit(_column);
    _title->setObjectName("canvasTitle");
    _title->setFrame(false);
    _title->setPlaceholderText(tr("Your canvas title"));
    col->addWidget(_title);

    _body = new CanvasEdit(_column);
    _body->setPlaceholderText(tr("Go ahead, start writing!"));
    _body->document()->setDocumentMargin(0);
    col->addWidget(_body, 1);

    auto *centerRow = new QHBoxLayout;
    centerRow->setContentsMargins(0, 0, 0, 0);
    centerRow->addStretch();
    centerRow->addWidget(_column, 1);
    centerRow->addStretch();
    outer->addLayout(centerRow, 1);

    // Floating "⋮" actions button, top-right over the page (no layout).
    _menuBtn = new QPushButton(this);
    _menuBtn->setObjectName("canvasMenuBtn");
    _menuBtn->setFixedSize(34, 34);
    _menuBtn->setCursor(Qt::PointingHandCursor);
    _menuBtn->setIconSize(QSize(17, 17));
    _menuBtn->hide(); // meaningless until the canvas exists
    connect(_menuBtn, &QPushButton::clicked, this, &CanvasPage::showMenu);

    _saveTimer = new QTimer(this);
    _saveTimer->setSingleShot(true);
    _saveTimer->setInterval(kSaveDelayMs);
    connect(_saveTimer, &QTimer::timeout, this, &CanvasPage::flushPendingSave);

    connect(_body->document(), &QTextDocument::contentsChanged, this, [this] {
        if (_loading)
            return;
        _bodyDirty = true;
        _saveTimer->start();
    });
    connect(_title, &QLineEdit::textEdited, this, [this](const QString &text) {
        _titleDirty = true;
        _saveTimer->start();
        emit titleChanged(text);
    });

    // Show a hovered link's URL in our tooltip (highlighted() carries the URL on
    // enter and an empty URL on leave); hide it when a link is clicked.
    _linkTip = new PopupTooltip(_body);
    connect(_body, &QTextBrowser::highlighted, this, [this](const QUrl &url) {
        if (url.isEmpty()) {
            _linkTip->hide();
            return;
        }
        const QPoint g = QCursor::pos();
        _linkTip->showAbove(url.toString(), QRect(g - QPoint(0, 6), QSize(1, 1)));
    });
    connect(_body, &QTextBrowser::anchorClicked, this, [this] { _linkTip->hide(); });

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void CanvasPage::setSession(Session *session) {
    _session       = session;
    _body->session = session;
}

void CanvasPage::open(ConversationId conv, const QString &fileId, const QString &knownTitle) {
    // Same conversation, same canvas (or still no canvas): keep the editor —
    // but if nothing is unsaved, refetch so remote edits show up. Local edits
    // always win (they autosave shortly); a clean editor syncs to remote.
    if (_conv == conv && _fileId == fileId) {
        if (!fileId.isEmpty() && !_bodyDirty && !_titleDirty && !_saving)
            loadContent();
        return;
    }

    flushPendingSave(); // previous conversation's pending edits

    ++_openSeq;
    _conv           = conv;
    _fileId         = fileId;
    _permalink      = {};
    _lastHtml       = {};
    _serverTitle    = knownTitle;
    _baseRefetching = false;
    setReadOnlyUi(ReadOnlyCause::None);
    _title->setText(Emoji::expandCodes(knownTitle));
    _titleDirty = false;
    setBodyHtml({});
    _menuBtn->setVisible(!fileId.isEmpty());

    if (fileId.isEmpty()) {
        _body->setFocus(); // blank editor; canvas is created on first save
        return;
    }
    loadContent();
}

void CanvasPage::loadContent() {
    if (!_session || _fileId.isEmpty())
        return;
    const quint64        seq = _openSeq;
    QPointer<CanvasPage> guard(this);
    _baseRefetching = true;
    // Meta first: the file title identifies the title h1 in the content HTML.
    _session->loadCanvasMeta(
        _fileId, [guard, seq](QString title, QString permalink, CanvasMetaState state) {
            if (!guard || guard->_openSeq != seq)
                return;
            if (state == CanvasMetaState::Gone) {
                // Deleted elsewhere but conversations.info still references it —
                // drop ours and let the tab strip revert to "Add canvas".
                guard->_baseRefetching = false;
                guard->_fileId         = {};
                guard->_lastHtml       = {};
                guard->_serverTitle    = {};
                guard->_title->clear();
                guard->setBodyHtml({});
                guard->_menuBtn->hide();
                emit guard->canvasDeleted();
                return;
            }
            if (state == CanvasMetaState::NoAccess) {
                // The canvas exists but the token may not view it (files.info
                // not_visible — e.g. a public channel the user hasn't joined, or
                // restricted canvas access). The content download would fail the
                // same way, so don't attempt it — and never show an empty editable
                // document whose autosave would try to replace the whole canvas.
                guard->_baseRefetching = false;
                guard->_lastHtml       = {};
                guard->_serverTitle    = {};
                guard->_title->clear();
                guard->setBodyHtml({});
                guard->_menuBtn->hide();
                guard->setReadOnlyUi(ReadOnlyCause::NoAccess);
                return;
            }
            // Visible again after a NoAccess spell — re-enable editing.
            // (NotAddressable is permanent and never cleared here.)
            if (guard->_roCause == ReadOnlyCause::NoAccess)
                guard->setReadOnlyUi(ReadOnlyCause::None);
            guard->_permalink = permalink;
            if (!title.isEmpty())
                guard->_serverTitle = title;
            guard->_session->loadCanvasContent(
                guard->_fileId,
                [guard, seq](QString html) {
                    if (!guard || guard->_openSeq != seq)
                        return;
                    guard->applyRemoteHtml(html);
                },
                [guard, seq](const QString &) {
                    if (!guard || guard->_openSeq != seq)
                        return;
                    // Base unknown — force the whole-doc fallback on the next save.
                    guard->_baseRefetching = false;
                    guard->_lastHtml       = {};
                }
            );
        }
    );
}

void CanvasPage::applyRemoteHtml(const QString &rawHtml) {
    _baseRefetching = false;
    // Expand emoji codes before anything else: the result is both the displayed
    // body and the diff base (`_lastHtml`), so they must agree or every save
    // would diff Unicode (document) against shortcodes (base) as a spurious edit.
    static const QHash<QString, QString> kNoCustom;
    const QString                        html = prepareCanvasHtml(
        CanvasEmoji::expandInHtml(rawHtml, _session ? _session->emojiMap() : kNoCustom)
    );
    if (html == _lastHtml)
        return; // unchanged remote; don't reset the view
    _lastHtml = html;

    // Local edits win — the refreshed base is enough; the view keeps them.
    // But only when there's actually local content to protect: a stuck dirty/
    // saving flag (e.g. a save Slack keeps rejecting, which re-arms _bodyDirty)
    // must never leave the body permanently blank when the server has content.
    if ((_bodyDirty || _titleDirty || _saving) && !_body->document()->isEmpty())
        return;

    const auto [title, bodyHtml] = CanvasDiff::splitTitleH1(
        html, {Emoji::expandCodes(_serverTitle), _title->text().trimmed()}
    );
    _title->setText(title);
    emit titleChanged(title);
    setBodyHtml(bodyHtml);
}

void CanvasPage::setBodyHtml(const QString &html) {
    _loading = true;
    if (html.isEmpty()) {
        _body->clear();
    } else {
        _body->setHtml(html);
        styleHeadings();
    }
    _loading   = false;
    _bodyDirty = false;
}

// Size heading blocks to a decreasing scale below the canvas title. Done
// per-block because Qt's HTML engine ignores font-size on h1..h6 in the
// default stylesheet and falls back to its oversized built-in multipliers.
// Display-only: the heading *level* (what toMarkdown emits as #) is untouched,
// so the section diff is unaffected.
void CanvasPage::styleHeadings() {
    QTextDocument *doc = _body->document();
    QTextCursor    c(doc);
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const int lvl = b.blockFormat().headingLevel();
        if (lvl <= 0)
            continue;
        // Title (QLineEdit) is 28px; keep every heading strictly below it.
        const int px = lvl == 1 ? 23 : lvl == 2 ? 20 : lvl == 3 ? 17 : 15;
        // Per fragment so inline runs keep their own colour/italic/bold. A plain
        // mergeCharFormat can't drop the FontSizeAdjustment that Qt's <h*> import
        // stamps on (it inflates the size); replacing each fragment's format with
        // a copy that has the adjustment cleared and the pixel size set does.
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid())
                continue;
            QTextCharFormat cf = frag.charFormat();
            QFont           f  = cf.font();
            f.setPixelSize(px);
            f.setBold(true);
            cf.setFont(f);
            cf.clearProperty(QTextFormat::FontSizeAdjustment);
            c.setPosition(frag.position());
            c.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
            c.setCharFormat(cf);
        }
    }
}

void CanvasPage::flushPendingSave() {
    if (!_session || _conv.value.isEmpty() || _roCause != ReadOnlyCause::None)
        return;
    if (!_bodyDirty && !_titleDirty)
        return;
    if (_saving || _baseRefetching) { // wait for the in-flight save / base refresh
        _saveTimer->start();
        return;
    }
    _saveTimer->stop();

    const QString bodyMd = CanvasDiff::normalizeMd(
        _body->document()->toMarkdown(QTextDocument::MarkdownDialectGitHub)
    );
    const QString title = _title->text().trimmed();

    // Nothing to create a canvas from yet.
    if (_fileId.isEmpty() && bodyMd.isEmpty() && title.isEmpty()) {
        _bodyDirty = _titleDirty = false;
        return;
    }

    const quint64        seq = _openSeq;
    QPointer<CanvasPage> guard(this);
    _saving = true;

    if (_fileId.isEmpty()) {
        // Body-only content; the title is set via a follow-up rename (the
        // title is a separate field that renders as a leading h1 — weaving
        // "# title" into the document would duplicate it).
        _bodyDirty = _titleDirty = false;
        _session->createChannelCanvas(
            _conv,
            bodyMd,
            [guard, seq, title](QString fileId) {
                if (!guard)
                    return;
                if (guard->_openSeq != seq || fileId.isEmpty()) {
                    guard->_saving = false;
                    return;
                }
                guard->_fileId = fileId;
                guard->_menuBtn->show();
                emit       guard->canvasCreated(fileId);
                const auto finish = [guard, seq](bool, QString) {
                    if (!guard)
                        return;
                    guard->_saving = false;
                    if (guard->_openSeq == seq)
                        guard->loadContent();
                };
                if (title.isEmpty())
                    finish(true, {});
                else
                    guard->_session->editCanvas(
                        fileId,
                        {{.op = CanvasChange::Op::Rename, .sectionId = {}, .markdown = title}},
                        finish
                    );
            },
            [guard, seq](const QString &err) {
                // Banner already shown by Session; content stays in the editor.
                if (!guard)
                    return;
                guard->_saving = false;
                if (guard->_openSeq != seq)
                    return;
                guard->_bodyDirty = true;
                // The channel already has a canvas we failed to detect (e.g. a
                // free-team canvas tab): adopt it and retry the save as an edit
                // instead of looping on create.
                if (err.contains(QLatin1String("already_exists"))) {
                    guard->_session->loadChannelCanvas(
                        guard->_conv, [guard, seq](QString fileId, bool) {
                            if (!guard || guard->_openSeq != seq || fileId.isEmpty())
                                return;
                            guard->_fileId = fileId;
                            guard->_menuBtn->show();
                            emit guard->canvasCreated(fileId);
                            guard->flushPendingSave();
                        }
                    );
                    return;
                }
                guard->_saveTimer->start();
            }
        );
        return;
    }

    // Section-level ops when the base is known and the structure permits;
    // whole-document replace otherwise. The whole-doc replace touches content
    // sections only — the title h1 survives it (verified), so the body never
    // carries a woven "# title".
    std::optional<std::vector<CanvasChange>> sectionOps;
    if (!_bodyDirty) {
        sectionOps = std::vector<CanvasChange>{}; // title-only save
    } else if (!_lastHtml.isEmpty()) {
        const auto [t, baseBody] =
            CanvasDiff::splitTitleH1(_lastHtml, {_serverTitle, title, _title->text().trimmed()});
        if (const auto base = CanvasDiff::parseBaseChunks(baseBody))
            sectionOps = CanvasDiff::diff(*base, CanvasDiff::documentChunks(_body->document()));
    }

    std::vector<CanvasChange> changes;
    if (_titleDirty && !title.isEmpty())
        changes.push_back({.op = CanvasChange::Op::Rename, .sectionId = {}, .markdown = title});
    if (sectionOps) {
        changes.insert(changes.end(), sectionOps->begin(), sectionOps->end());
    } else {
        changes.push_back(
            {.op        = CanvasChange::Op::ReplaceAll,
             .sectionId = {},
             // Slack rejects empty document_content; a lone space clears the page.
             .markdown  = bodyMd.isEmpty() ? QStringLiteral(" ") : bodyMd}
        );
    }

    _bodyDirty = _titleDirty = false;
    if (changes.empty()) {
        _saving = false;
        return;
    }
    _session->editCanvas(_fileId, changes, [guard, seq](bool ok, QString err) {
        if (!guard)
            return;
        guard->_saving = false;
        if (guard->_openSeq != seq)
            return;
        if (ok) {
            // Section ids may have changed (replace reissues them) — refresh
            // the base before the next diff; saves defer while it's in flight.
            guard->loadContent();
            return;
        }
        if (err.contains(QLatin1String("canvas_not_found"))) {
            // Not addressable as a canvas (e.g. a free-team tab canvas made
            // with Slack's built-in editor) — retrying can never succeed.
            guard->setReadOnlyUi(ReadOnlyCause::NotAddressable);
            return;
        }
        // Edits stay in the editor; force the whole-doc fallback next time —
        // a failed sequence can leave the document partially updated.
        guard->_bodyDirty = true;
        guard->_lastHtml  = {};
        guard->_saveTimer->start();
    });
}

void CanvasPage::setReadOnlyUi(ReadOnlyCause cause) {
    _roCause               = cause;
    const bool    readOnly = cause != ReadOnlyCause::None;
    const QString notice =
        cause == ReadOnlyCause::NotAddressable
            ? tr("This canvas was created with Slack's built-in editor and is not editable "
                 "through the Slack API — it is read-only here.")
        : cause == ReadOnlyCause::NoAccess ? tr("You don't have access to this canvas.")
                                           : QString();
    _roNotice->setText(notice);
    _body->applyReadOnly(readOnly);
    _title->setReadOnly(readOnly);
    _roNotice->setVisible(readOnly);
    if (readOnly) {
        _saveTimer->stop();
        _bodyDirty = _titleDirty = false;
    }
}

void CanvasPage::showMenu() {
    if (_fileId.isEmpty())
        return;
    auto *menu = new ContextMenu(this);
    menu->addItem(
        tr("Copy link"),
        [this] {
            if (!_permalink.isEmpty())
                Clipboard::setText(_permalink);
        },
        /*destructive=*/false,
        ":/ui/link.svg"
    );
    menu->addSeparator();
    menu->addItem(tr("Delete canvas"), [this] { confirmDelete(); }, /*destructive=*/true);
    menu->popup(_menuBtn->mapToGlobal(QPoint(_menuBtn->width(), _menuBtn->height() + 4)));
}

void CanvasPage::confirmDelete() {
    auto *dlg = new DeleteCanvasDialog(window());
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::accepted, this, [this] {
        if (!_session || _fileId.isEmpty())
            return;
        // Drop pending edits — they would re-create content on a dead canvas.
        _saveTimer->stop();
        _bodyDirty = _titleDirty = false;
        QPointer<CanvasPage> guard(this);
        _session->deleteCanvas(_fileId, [guard](bool ok) {
            if (!guard || !ok)
                return;
            guard->clear();
            emit guard->canvasDeleted();
        });
    });
    dlg->exec();
}

void CanvasPage::clear() {
    ++_openSeq;
    if (_linkTip)
        _linkTip->hide();
    _saveTimer->stop();
    _conv           = {};
    _fileId         = {};
    _permalink      = {};
    _lastHtml       = {};
    _serverTitle    = {};
    _baseRefetching = false;
    setReadOnlyUi(ReadOnlyCause::None);
    _title->clear();
    setBodyHtml({});
    _titleDirty = false;
    _menuBtn->hide();
}

void CanvasPage::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    _menuBtn->move(width() - _menuBtn->width() - 16, 16);
    _menuBtn->raise();
}

void CanvasPage::applyTheme() {
    const auto &th = Th::c();
    setStyleSheet(
        QString("QWidget#canvasPage { background: %1; }").arg(Th::qss(th.surface.content))
    );
    _title->setStyleSheet(QString(
                              "QLineEdit#canvasTitle { background: transparent; border: none;"
                              " font-size: 28px; font-weight: bold; color: %1; }"
    )
                              .arg(Th::qss(th.text.primary)));
    _roNotice->setStyleSheet(
        QString("color: %1; font-size: %2px;").arg(Th::qss(th.text.warning)).arg(th.fonts.caption)
    );
    _body->setStyleSheet(
        QString(
            "QTextBrowser { background: transparent; border: none;"
            " font-size: %1px; color: %2; }"
            // (color is the softer document-body tone, not near-black primary)
            // Our scrollbar design: thin rounded handle, transparent track, no arrows.
            "QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
            "QScrollBar::handle:vertical { background: %3; border-radius: 4px;"
            " min-height: 28px; }"
            "QScrollBar::handle:vertical:hover { background: %4; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
            " background: transparent; }"
            "QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; }"
            "QScrollBar::handle:horizontal { background: %3; border-radius: 4px;"
            " min-width: 28px; }"
            "QScrollBar::handle:horizontal:hover { background: %4; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
            " background: transparent; }"
        )
            .arg(th.fonts.lg)
            .arg(
                Th::qss(th.text.documentBody),
                Th::qss(th.divider.strong),
                Th::qss(th.text.secondary)
            )
    );
    // Heading sizes are applied per-block in styleHeadings() (Qt's rich-text
    // engine ignores font-size on h1..h6 in the default stylesheet), so this
    // sheet only covers links / code / quotes.
    _body->document()->setDefaultStyleSheet(
        QString(
            "a { color: %1; text-decoration: none; }"
            "code, pre { background-color: %2; }"
            "blockquote { color: %3; }"
        )
            .arg(Th::qss(th.accent.def), Th::qss(th.surface.sunken), Th::qss(th.text.secondary))
    );
    _menuBtn->setStyleSheet(QString(
                                "QPushButton#canvasMenuBtn { background: %1;"
                                " border: 1px solid %2; border-radius: 8px; }"
                                "QPushButton#canvasMenuBtn:hover { background: %3; }"
    )
                                .arg(
                                    Th::qss(th.surface.content),
                                    Th::qss(th.divider.strong),
                                    Th::qss(th.surface.highlight)
                                ));
    _menuBtn->setIcon(svgIcon(":/ui/ellipsis-vertical.svg", QSize(17, 17), th.icon.def));
}
