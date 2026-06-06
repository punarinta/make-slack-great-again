// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "mention_popup.h"
#include "session/session.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {
constexpr int kRowH       = 34;
constexpr int kMaxVisible = 8;
constexpr int kWidth      = 300;
constexpr int kMargins    = 4;

struct AliasInfo { const char *name; const char *insert; const char *desc; };
constexpr AliasInfo kAliases[] = {
    { "@channel",  "@channel",  QT_TR_NOOP("Notify everyone in this channel")   },
    { "@everyone", "@everyone", QT_TR_NOOP("Notify everyone in your workspace") },
    { "@here",     "@here",     QT_TR_NOOP("Notify every online member here")   },
};
}

// ── Internal row widget — no Q_OBJECT, click via std::function ───────────────

class MentionRow : public QWidget {
public:
    std::function<void()> onClick;
    std::function<void()> onHover;

    MentionRow(const QString &name, const QString &desc, QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_StyledBackground, true);
        setFixedHeight(kRowH);
        setCursor(Qt::PointingHandCursor);

        auto *lay = new QHBoxLayout(this);
        lay->setContentsMargins(10, 0, 10, 0);
        lay->setSpacing(8);

        _nameL = new QLabel(name, this);
        lay->addWidget(_nameL);

        if (!desc.isEmpty()) {
            _descL = new QLabel(desc, this);
            lay->addWidget(_descL, 1);
        } else {
            lay->addStretch(1);
        }

        applyStyle(false);
    }

    void setSelected(bool s) { applyStyle(s); }

protected:
    void enterEvent(QEnterEvent *) override { if (onHover) onHover(); }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && onClick) onClick();
        QWidget::mousePressEvent(e);
    }

private:
    QLabel *_nameL = nullptr;
    QLabel *_descL = nullptr;

    void applyStyle(bool sel) {
        if (sel) {
            setStyleSheet("background:#1264A3; border-radius:4px;");
            if (_nameL) _nameL->setStyleSheet("font-size:13px; color:#FFFFFF; background:transparent;");
            if (_descL) _descL->setStyleSheet("font-size:12px; color:#FFFFFF; background:transparent;");
        } else {
            setStyleSheet("background:transparent;");
            if (_nameL) _nameL->setStyleSheet("font-size:13px; color:#1D1C1D; background:transparent;");
            if (_descL) _descL->setStyleSheet("font-size:12px; color:#888888; background:transparent;");
        }
    }
};

// ── MentionPopup ──────────────────────────────────────────────────────────────

MentionPopup::MentionPopup(QWidget *parent)
    : QFrame(parent)
{
    // Plain child widget of the container — no separate window, no focus events.
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("mentionPopup");
    setStyleSheet(
        "QFrame#mentionPopup {"
        "  background:#FFFFFF;"
        "  border:1px solid #D1D1D1;"
        "  border-radius:6px;"
        "}"
    );
    setFixedWidth(kWidth);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(kMargins, kMargins, kMargins, kMargins);
    outer->setSpacing(0);

    _scroll = new QScrollArea(this);
    _scroll->setFrameShape(QFrame::NoFrame);
    _scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _scroll->setStyleSheet("QScrollArea { background:transparent; border:none; }");

    _content = new QWidget(_scroll);
    _content->setStyleSheet("background:transparent;");

    _vbox = new QVBoxLayout(_content);
    _vbox->setContentsMargins(0, 0, 0, 0);
    _vbox->setSpacing(1);

    _scroll->setWidget(_content);
    _scroll->setWidgetResizable(true);
    outer->addWidget(_scroll);

    hide();
}

void MentionPopup::setSession(Session *s) { _session = s; }

void MentionPopup::open(const QPoint &anchor, const QString &query, bool isDm) {
    rebuild(query, isDm);
    if (_rows.isEmpty()) { hide(); return; }

    const int visible = qMin(_rows.size(), kMaxVisible);
    // height = visible rows + spacing between them + top/bottom margins
    setFixedHeight(visible * kRowH + (visible - 1) * 1 + 2 * kMargins);

    if (!isVisible()) {
        // Convert global cursor position to parent-local coordinates, then
        // place the popup so its bottom edge sits just above the cursor.
        const QPoint local = parentWidget()->mapFromGlobal(anchor);
        move(local - QPoint(0, height() + 4));
        show();
        raise();
    }
    // Already visible: list rebuilt in-place, position stays.
}

void MentionPopup::dismiss() {
    hide();
    // Do NOT delete rows here: this may be called from inside a MentionRow's
    // own mousePressEvent (via onClick → confirm → dismiss), and deleting the
    // widget while its event handler is on the call stack is undefined behavior.
    // rebuild() cleans up old rows at the start of the next open().
}

bool MentionPopup::handleKey(int key) {
    if (!isVisible() || _rows.isEmpty()) return false;
    switch (key) {
    case Qt::Key_Escape:
        dismiss();
        return true;
    case Qt::Key_Up:
        selectRow((_sel - 1 + _rows.size()) % _rows.size());
        return true;
    case Qt::Key_Down:
        selectRow((_sel + 1) % _rows.size());
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Return:
        confirm();
        return true;
    default:
        return false;
    }
}

void MentionPopup::rebuild(const QString &query, bool isDm) {
    while (_vbox->count())
        delete _vbox->takeAt(0)->widget();
    _rows.clear();
    _inserts.clear();
    _sel = 0;

    const QString q = query.toLower();

    auto addRow = [&](const QString &name, const QString &insert, const QString &desc = {}) {
        auto *row = new MentionRow(name, desc, _content);
        const int idx = _rows.size();
        row->onClick = [this, idx] { _sel = idx; confirm(); };
        row->onHover = [this, idx] { selectRow(idx); };
        _vbox->addWidget(row);
        _rows.append(row);
        _inserts.append(insert);
    };

    // Aliases — channels and group DMs only, not 1:1 DMs
    if (!isDm) {
        for (const auto &a : kAliases) {
            const QString name(a.name);
            if (q.isEmpty() || name.contains(q, Qt::CaseInsensitive))
                addRow(name, QLatin1String(a.insert), tr(a.desc));
        }
    }

    // Users from session
    if (_session) {
        const auto &users = _session->currentUsers();
        int added = 0;
        for (const auto &u : users) {
            if (u.isDeactivated) continue;
            const QString disp = u.displayName.isEmpty() ? u.name : u.displayName;
            if (!q.isEmpty() && !disp.contains(q, Qt::CaseInsensitive)) continue;
            addRow("@" + disp, "<@" + u.id.value + ">");
            if (++added >= 50) break;
        }
    }

    if (!_rows.isEmpty())
        static_cast<MentionRow *>(_rows[0])->setSelected(true);
}

void MentionPopup::selectRow(int idx) {
    if (_rows.isEmpty()) return;
    if (_sel >= 0 && _sel < _rows.size())
        static_cast<MentionRow *>(_rows[_sel])->setSelected(false);
    _sel = qBound(0, idx, _rows.size() - 1);
    static_cast<MentionRow *>(_rows[_sel])->setSelected(true);
    _scroll->ensureWidgetVisible(_rows[_sel]);
}

void MentionPopup::confirm() {
    if (_sel < 0 || _sel >= _inserts.size()) { dismiss(); return; }
    const QString text = _inserts[_sel];
    dismiss();
    emit selected(text);
}
