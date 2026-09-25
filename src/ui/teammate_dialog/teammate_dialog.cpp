// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "teammate_dialog.h"
#include "ui/control_metrics.h"
#include "ui/styled_button/styled_button.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"
#include "util/avatar_glyphs.h"

#include <QAbstractButton>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QSvgRenderer>
#include <QVBoxLayout>

namespace {

constexpr int kPreviewSize = 64;
constexpr int kSwatchSize  = 26;
constexpr int kGlyphCols   = 14;

void paintTile(QPainter &p, const QRect &r, const QString &glyph, const QColor &color) {
    QSvgRenderer svg(AvatarGlyphs::svg(glyph, color));
    svg.render(&p, r);
}

} // namespace

// One pick in the dialog: a glyph tile (in the chosen colour) or a colour
// dot. The chosen one is ringed in the accent colour.
class GlyphSwatch : public QAbstractButton {
public:
    GlyphSwatch(QString glyph, QColor color, bool isColor, QWidget *parent)
        : QAbstractButton(parent), _glyph(std::move(glyph)), _color(std::move(color)),
          _isColor(isColor) {
        setFixedSize(kSwatchSize + 6, kSwatchSize + 6); // room for the ring
        setCursor(Qt::PointingHandCursor);
        setCheckable(true);
        setFocusPolicy(Qt::NoFocus);
    }
    const QString &glyph() const { return _glyph; }
    const QColor  &color() const { return _color; }
    void           setTileColor(const QColor &c) {
        _color = c;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRect inner(3, 3, kSwatchSize, kSwatchSize);
        if (_isColor) {
            p.setPen(Qt::NoPen);
            p.setBrush(_color);
            p.drawEllipse(inner.adjusted(4, 4, -4, -4));
        } else {
            paintTile(p, inner, _glyph, _color);
        }
        if (isChecked() || underMouse()) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(isChecked() ? Th::c().accent.def : Th::c().divider.subtle, 2));
            if (_isColor)
                p.drawEllipse(QRectF(rect()).adjusted(1, 1, -1, -1));
            else
                p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
        }
    }
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    QString _glyph;
    QColor  _color;
    bool    _isColor = false;
};

TeammateDialog::TeammateDialog(const AgentRole &role, QWidget *parent)
    : AppDialog(role.id.isEmpty() ? tr("Add a teammate") : tr("Edit teammate"), parent),
      _role(role) {
    auto       *cl = contentLayout();
    const auto &sp = Th::c().spacing;
    cl->setSpacing(sp.md);

    _glyph = AvatarGlyphs::hasGlyph(role.glyph) ? role.glyph : AvatarGlyphs::glyphs().front().id;
    _color = QColor(role.color).isValid() ? QColor(role.color) : AvatarGlyphs::colors()[1];

    auto label = [this](const QString &text) {
        auto *l = new QLabel(text);
        QFont f = l->font();
        f.setBold(true);
        l->setFont(f);
        _labels.push_back(l);
        return l;
    };
    auto hint = [this](const QString &text) {
        auto *l = new QLabel(text);
        l->setWordWrap(true);
        _hints.push_back(l);
        return l;
    };

    // ── Picture preview beside the name and description ──────────────
    auto *top = new QHBoxLayout();
    top->setSpacing(sp.xl);
    _preview = new QLabel;
    _preview->setFixedSize(kPreviewSize, kPreviewSize);
    top->addWidget(_preview, 0, Qt::AlignTop);
    auto *fields = new QVBoxLayout();
    fields->setSpacing(sp.sm);
    fields->addWidget(label(tr("Name")));
    _name = new StyledLineEdit;
    _name->setMaxLength(40);
    _name->setPlaceholderText(tr("Copywriter"));
    _name->setText(role.name);
    fields->addWidget(_name);
    fields->addSpacing(sp.sm);
    fields->addWidget(label(tr("Description")));
    _desc = new StyledLineEdit;
    _desc->setMaxLength(120);
    _desc->setPlaceholderText(tr("Writes clear, friendly product copy."));
    _desc->setText(role.description);
    fields->addWidget(_desc);
    top->addLayout(fields, 1);
    cl->addLayout(top);

    // ── Picture: a glyph and a colour ────────────────────────────────
    cl->addSpacing(sp.sm);
    cl->addWidget(label(tr("Picture")));
    auto *grid = new QGridLayout();
    grid->setSpacing(0);
    int i = 0;
    for (const auto &g : AvatarGlyphs::glyphs()) {
        auto *b = new GlyphSwatch(g.id, _color, false, this);
        b->setToolTip(g.id);
        connect(b, &QAbstractButton::clicked, this, [this, id = g.id] { pickGlyph(id); });
        grid->addWidget(b, i / kGlyphCols, i % kGlyphCols);
        _glyphs.push_back(b);
        ++i;
    }
    grid->setColumnStretch(kGlyphCols, 1);
    cl->addLayout(grid);
    auto *colors = new QHBoxLayout();
    colors->setSpacing(0);
    for (const QColor &c : AvatarGlyphs::colors()) {
        auto *b = new GlyphSwatch({}, c, true, this);
        connect(b, &QAbstractButton::clicked, this, [this, c] { pickColor(c); });
        colors->addWidget(b);
        _colors.push_back(b);
    }
    colors->addStretch(1);
    cl->addLayout(colors);

    // ── Instructions ─────────────────────────────────────────────────
    cl->addSpacing(sp.sm);
    cl->addWidget(label(tr("Instructions")));
    _prompt = new QPlainTextEdit;
    _prompt->setPlaceholderText(
        tr("You are the team's copywriter. You write short, friendly copy in the product's "
           "voice…")
    );
    _prompt->setPlainText(role.prompt);
    _prompt->setMinimumHeight(170);
    _prompt->setTabChangesFocus(true);
    cl->addWidget(_prompt);
    QString about =
        tr("Added to Claude Code's own instructions: what the teammate focuses on, "
           "how it works, what to avoid.");
    if (!role.id.isEmpty())
        about += QLatin1Char(' ') + tr("Sessions already started keep the instructions they "
                                       "began with; new sessions get these.");
    cl->addWidget(hint(about));

    // ── Buttons ──────────────────────────────────────────────────────
    auto *cancel = new StyledButton(tr("Cancel"), StyledButton::Variant::Secondary);
    _saveBtn     = new StyledButton(
        role.id.isEmpty() ? tr("Add teammate") : tr("Save"), StyledButton::Variant::Primary
    );
    StyledButton *restore = nullptr;
    if (role.builtIn && role.edited) {
        restore = new StyledButton(tr("Restore default"), StyledButton::Variant::Ghost);
        connect(restore, &QPushButton::clicked, this, [this] {
            _restore = true;
            accept();
        });
    }
    addButtonRow(_saveBtn, cancel, restore);
    connect(_saveBtn, &QPushButton::clicked, this, &AppDialog::accept);
    connect(_name, &StyledLineEdit::textChanged, this, [this] { updateSaveEnabled(); });

    pickGlyph(_glyph);
    pickColor(_color);
    updateSaveEnabled();
    applyTheme();
    updateCard();
    _name->setFocus();
}

AgentRole TeammateDialog::role() const {
    AgentRole r   = _role;
    r.name        = _name->text().simplified();
    r.description = _desc->text().simplified();
    r.glyph       = _glyph;
    r.color       = _color.name();
    r.prompt      = _prompt->toPlainText().trimmed();
    return r;
}

void TeammateDialog::pickGlyph(const QString &id) {
    _glyph = id;
    for (auto *b : _glyphs)
        b->setChecked(b->glyph() == id);
    updatePreview();
}

void TeammateDialog::pickColor(const QColor &color) {
    _color = color;
    for (auto *b : _colors)
        b->setChecked(b->color() == color);
    for (auto *b : _glyphs)
        b->setTileColor(color);
    updatePreview();
}

void TeammateDialog::updatePreview() {
    const qreal dpr = devicePixelRatioF();
    QPixmap     px(QSize(kPreviewSize, kPreviewSize) * dpr);
    px.setDevicePixelRatio(dpr);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    paintTile(p, QRect(0, 0, kPreviewSize, kPreviewSize), _glyph, _color);
    p.end();
    _preview->setPixmap(px);
}

void TeammateDialog::updateSaveEnabled() {
    _saveBtn->setEnabled(!_name->text().trimmed().isEmpty());
}

void TeammateDialog::applyTheme() {
    AppDialog::applyTheme();
    const auto &th = Th::c();
    for (auto *l : _labels)
        l->setStyleSheet(QString("color: %1;").arg(Th::qss(th.text.primary)));
    for (auto *l : _hints)
        l->setStyleSheet(
            QString("color: %1; font-size: %2px;").arg(Th::qss(th.text.secondary)).arg(th.fonts.sm)
        );
    if (_prompt)
        _prompt->setStyleSheet(
            QString(
                "QPlainTextEdit { border: 1px solid %1; border-radius: %2px; "
                "background: %3; color: %4; font-size: %5px; padding: 4px; }"
                "QPlainTextEdit:focus { border: 2px solid %6; }"
            )
                .arg(Th::qss(th.composer.border))
                .arg(Ui::kControlRadius)
                .arg(Th::qss(th.surface.raised))
                .arg(Th::qss(th.text.primary))
                .arg(th.fonts.base)
                .arg(Th::qss(th.composer.borderFocus)) +
            Th::scrollBarQss()
        );
}

// ── RemoveTeammateDialog ──────────────────────────────────────────────────────

RemoveTeammateDialog::RemoveTeammateDialog(const QString &name, QWidget *parent)
    : AppDialog(tr("Remove teammate"), parent) {
    _text = new QLabel(
        tr("Remove the %1 from the team? Its sessions stay in the list, with its name and "
           "picture.")
            .arg(name)
    );
    _text->setWordWrap(true);
    contentLayout()->addWidget(_text);
    auto *cancel = new StyledButton(tr("Cancel"), StyledButton::Variant::Secondary);
    auto *remove = new StyledButton(tr("Remove"), StyledButton::Variant::Danger);
    addButtonRow(remove, cancel);
    connect(remove, &QPushButton::clicked, this, &AppDialog::accept);
    applyTheme();
    updateCard();
}

void RemoveTeammateDialog::applyTheme() {
    AppDialog::applyTheme();
    if (_text)
        _text->setStyleSheet(QString("color: %1;").arg(Th::qss(Th::c().text.primary)));
}
