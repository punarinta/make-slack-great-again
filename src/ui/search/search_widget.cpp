// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "search_widget.h"
#include "session/session.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QLabel>
#include <QDateTime>
#include <QKeyEvent>

namespace {

QString formatTs(const Ts &ts) {
    bool   ok   = false;
    double secs = ts.toDouble(&ok);
    if (!ok)
        return ts;
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(secs)).toString("MMM d, h:mm AP");
}

} // namespace

SearchWidget::SearchWidget(QWidget *parent) : QWidget(parent) {
    setObjectName("searchWidget");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header row: search input + close button
    _header = new QWidget(this);
    _header->setObjectName("searchHeader");
    auto *hRow = new QHBoxLayout(_header);
    hRow->setContentsMargins(12, 8, 8, 8);
    hRow->setSpacing(8);

    auto *icon = new QLabel(_header);
    icon->setFixedSize(20, 20);
    icon->setPixmap(svgPixmap(":/ui/search.svg", QSize(16, 16), Th::c().icon.def));
    icon->setAlignment(Qt::AlignCenter);
    hRow->addWidget(icon);

    _queryEdit = new QLineEdit(_header);
    _queryEdit->setPlaceholderText(tr("Search messages…"));
    connect(_queryEdit, &QLineEdit::returnPressed, this, [this] {
        runSearch(_queryEdit->text().trimmed());
    });
    hRow->addWidget(_queryEdit, 1);

    _closeBtn = new QPushButton(_header);
    _closeBtn->setFixedSize(24, 24);
    _closeBtn->setFlat(true);
    _closeBtn->setCursor(Qt::PointingHandCursor);
    _closeBtn->setIconSize(QSize(14, 14));
    _closeBtn->setIcon(svgIcon(":/ui/x.svg", QSize(14, 14), Th::c().icon.def));
    connect(_closeBtn, &QPushButton::clicked, this, &SearchWidget::closeRequested);
    hRow->addWidget(_closeBtn);

    layout->addWidget(_header);

    // Results list
    _resultList = new QListWidget(this);
    _resultList->setObjectName("searchResultList");
    connect(_resultList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const int idx = item->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < (int)_results.size())
            emit resultSelected(_results[idx].conv, _results[idx].msg.ts);
    });
    layout->addWidget(_resultList, 1);

    applyTheme();
    connect(
        &ThemeManager::instance(), &ThemeManager::themeChanged, this, &SearchWidget::applyTheme
    );
}

void SearchWidget::applyTheme() {
    _header->setStyleSheet(
        QString("QWidget#searchHeader {"
                "  background: %1;"
                "  border-bottom: 1px solid %2;"
                "}")
            .arg(Th::qss(Th::c().surface.highlight), Th::qss(Th::c().divider.def))
    );
    _queryEdit->setStyleSheet(QString("QLineEdit {"
                                      "  border: 1px solid %1;"
                                      "  border-radius: 4px;"
                                      "  padding: 4px 8px;"
                                      "  font-size: %3px;"
                                      "}"
                                      "QLineEdit:focus { border-color: %2; }")
                                  .arg(Th::qss(Th::c().divider.strong), Th::qss(Th::c().text.link))
                                  .arg(Th::c().fonts.base));
    _closeBtn->setStyleSheet(QString("QPushButton { border-radius: 4px; }"
                                     "QPushButton:hover { background: %1; }")
                                 .arg(Th::qss(Th::c().divider.def)));
    _resultList->setStyleSheet(
        QString("QListWidget { border: none; background: %1; }"
                "QListWidget::item { padding: 8px 12px; border-bottom: 1px solid %2; }"
                "QListWidget::item:hover { background: %3; }"
                "QListWidget::item:selected { background: %4; }")
            .arg(
                Th::qss(Th::c().surface.raised),
                Th::qss(Th::c().surface.highlight),
                Th::qss(Th::c().surface.highlight),
                Th::qss(Th::c().accent.subtleBg)
            )
    );
}

void SearchWidget::setSession(Session *session) {
    _session = session;
    _queryEdit->clear();
    _resultList->clear();
    _results.clear();
}

void SearchWidget::runSearch(const QString &query) {
    if (!_session || query.isEmpty())
        return;
    _resultList->clear();
    _results.clear();

    auto *loadingItem = new QListWidgetItem(tr("Searching…"));
    loadingItem->setForeground(Th::c().text.tertiary);
    _resultList->addItem(loadingItem);

    _session->searchMessages(query, [this](std::vector<SearchResult> results) {
        _resultList->clear();
        _results = std::move(results);
        populateResults(_results);
    });
}

void SearchWidget::populateResults(const std::vector<SearchResult> &results) {
    _resultList->clear();
    if (results.empty()) {
        auto *item = new QListWidgetItem(tr("No results found."));
        item->setForeground(Th::c().text.tertiary);
        _resultList->addItem(item);
        return;
    }

    for (int i = 0; i < (int)results.size(); ++i) {
        const auto &r = results[i];

        // "#channel  h:mm AM  • message preview"
        const QString convLabel = r.convName.isEmpty() ? r.conv.value : "#" + r.convName;
        const QString tsLabel   = formatTs(r.msg.ts);
        const QString preview   = r.msg.text.text.left(120).replace('\n', ' ');

        auto *item = new QListWidgetItem(_resultList);
        item->setData(Qt::UserRole, i);

        // Two-line display using setData with custom roles isn't easy —
        // build a simple HTML-like representation as the display text.
        item->setText(convLabel + "  " + tsLabel + "\n" + preview);
        item->setToolTip(r.msg.text.text);
    }
}
