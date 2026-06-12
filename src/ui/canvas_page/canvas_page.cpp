// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "canvas_page.h"
#include "canvas_diff.h"
#include "session/session.h"
#include "ui/app_dialog/app_dialog.h"
#include "ui/context_menu/context_menu.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "util/clipboard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kColumnMaxW  = 700;  // editor column width, Notion-like measure
constexpr int kSaveDelayMs = 2500; // autosave this long after typing stops
} // namespace

// Editable canvas body. QTextBrowser rather than QTextEdit purely for the
// loadResource hook: files.slack.com images need the auth header, so they are
// fetched through the Session and patched into the document as they arrive.
class CanvasEdit : public QTextBrowser {
public:
    explicit CanvasEdit(QWidget *parent) : QTextBrowser(parent) {
        setReadOnly(false);
        setOpenLinks(false);
        setOpenExternalLinks(false);
        setFrameShape(QFrame::NoFrame);
    }

    Session *session = nullptr;

    QVariant loadResource(int type, const QUrl &url) override {
        if (type != QTextDocument::ImageResource || !url.scheme().startsWith("http"))
            return QTextBrowser::loadResource(type, url);
        const QString key = url.toString();
        if (const auto it = _images.constFind(key); it != _images.constEnd())
            return *it;
        if (session && !_pending.contains(key)) {
            _pending.insert(key);
            QPointer<CanvasEdit> guard(this);
            session->downloadFile(
                key,
                [guard, key, url](QByteArray data) {
                    if (!guard)
                        return;
                    QImage img;
                    if (!img.loadFromData(data))
                        return;
                    guard->_images.insert(key, img);
                    guard->document()->addResource(QTextDocument::ImageResource, url, img);
                    guard->document()->markContentsDirty(0, guard->document()->characterCount());
                },
                [](const QString &) {} // broken image is fine; don't spam the error banner
            );
        }
        return QImage();
    }

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

    _roNotice = new QLabel(
        tr("This canvas was created with Slack's built-in editor and is not editable "
           "through the Slack API — it is read-only here."),
        _column
    );
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
    setReadOnlyUi(false);
    _title->setText(knownTitle);
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
    _session->loadCanvasMeta(_fileId, [guard, seq](QString title, QString permalink, bool exists) {
        if (!guard || guard->_openSeq != seq)
            return;
        if (!exists) {
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
    });
}

void CanvasPage::applyRemoteHtml(const QString &html) {
    _baseRefetching = false;
    if (html == _lastHtml)
        return; // unchanged remote; don't reset the view
    _lastHtml = html;

    // Local edits win — the refreshed base is enough; the view keeps them.
    if (_bodyDirty || _titleDirty || _saving)
        return;

    const auto [title, bodyHtml] =
        CanvasDiff::splitTitleH1(html, {_serverTitle, _title->text().trimmed()});
    _title->setText(title);
    emit titleChanged(title);
    setBodyHtml(bodyHtml);
}

void CanvasPage::setBodyHtml(const QString &html) {
    _loading = true;
    if (html.isEmpty())
        _body->clear();
    else
        _body->setHtml(html);
    _loading   = false;
    _bodyDirty = false;
}

void CanvasPage::flushPendingSave() {
    if (!_session || _conv.value.isEmpty() || _readOnly)
        return;
    if (!_bodyDirty && !_titleDirty)
        return;
    if (_saving || _baseRefetching) { // wait for the in-flight save / base refresh
        _saveTimer->start();
        return;
    }
    _saveTimer->stop();

    const QString bodyMd =
        _body->document()->toMarkdown(QTextDocument::MarkdownDialectGitHub).trimmed();
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
            guard->setReadOnlyUi(true);
            return;
        }
        // Edits stay in the editor; force the whole-doc fallback next time —
        // a failed sequence can leave the document partially updated.
        guard->_bodyDirty = true;
        guard->_lastHtml  = {};
        guard->_saveTimer->start();
    });
}

void CanvasPage::setReadOnlyUi(bool readOnly) {
    _readOnly = readOnly;
    _body->setReadOnly(readOnly);
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
    _saveTimer->stop();
    _conv           = {};
    _fileId         = {};
    _permalink      = {};
    _lastHtml       = {};
    _serverTitle    = {};
    _baseRefetching = false;
    setReadOnlyUi(false);
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
    _body->setStyleSheet(QString(
                             "QTextBrowser { background: transparent; border: none;"
                             " font-size: %1px; color: %2; }"
    )
                             .arg(th.fonts.lg)
                             .arg(Th::qss(th.text.primary)));
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
