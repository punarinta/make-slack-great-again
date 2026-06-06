// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "search_widget.h"
#include "session/session.h"
#include "ui/icon_utils.h"

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
    bool ok = false;
    double secs = ts.toDouble(&ok);
    if (!ok) return ts;
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(secs))
               .toString("MMM d, h:mm AP");
}

} // namespace

SearchWidget::SearchWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("searchWidget");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header row: search input + close button
    auto *header = new QWidget(this);
    header->setObjectName("searchHeader");
    header->setStyleSheet(
        "QWidget#searchHeader {"
        "  background: #F8F8F8;"
        "  border-bottom: 1px solid #E0E0E0;"
        "}"
    );
    auto *hRow = new QHBoxLayout(header);
    hRow->setContentsMargins(12, 8, 8, 8);
    hRow->setSpacing(8);

    auto *icon = new QLabel(header);
    icon->setFixedSize(20, 20);
    icon->setPixmap(svgPixmap(":/ui/search.svg", QSize(16, 16), QColor("#888888")));
    icon->setAlignment(Qt::AlignCenter);
    hRow->addWidget(icon);

    _queryEdit = new QLineEdit(header);
    _queryEdit->setPlaceholderText("Search messages…");
    _queryEdit->setStyleSheet(
        "QLineEdit {"
        "  border: 1px solid #CCC;"
        "  border-radius: 4px;"
        "  padding: 4px 8px;"
        "  font-size: 14px;"
        "}"
        "QLineEdit:focus { border-color: #1164A3; }"
    );
    connect(_queryEdit, &QLineEdit::returnPressed, this, [this] {
        runSearch(_queryEdit->text().trimmed());
    });
    hRow->addWidget(_queryEdit, 1);

    auto *closeBtn = new QPushButton(header);
    closeBtn->setFixedSize(24, 24);
    closeBtn->setFlat(true);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setIconSize(QSize(14, 14));
    closeBtn->setIcon(svgIcon(":/ui/x.svg", QSize(14, 14), QColor("#888888")));
    closeBtn->setStyleSheet(
        "QPushButton { border-radius: 4px; }"
        "QPushButton:hover { background: #E8E8E8; }");
    connect(closeBtn, &QPushButton::clicked, this, &SearchWidget::closeRequested);
    hRow->addWidget(closeBtn);

    layout->addWidget(header);

    // Results list
    _resultList = new QListWidget(this);
    _resultList->setObjectName("searchResultList");
    _resultList->setStyleSheet(
        "QListWidget { border: none; background: #FFFFFF; }"
        "QListWidget::item { padding: 8px 12px; border-bottom: 1px solid #F0F0F0; }"
        "QListWidget::item:hover { background: #F5F5F5; }"
        "QListWidget::item:selected { background: #E8F0FA; }"
    );
    connect(_resultList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *item) {
        const int idx = item->data(Qt::UserRole).toInt();
        if (idx >= 0 && idx < (int)_results.size())
            emit resultSelected(_results[idx].conv, _results[idx].msg.ts);
    });
    layout->addWidget(_resultList, 1);
}

void SearchWidget::setSession(Session *session) {
    _session = session;
    _queryEdit->clear();
    _resultList->clear();
    _results.clear();
}

void SearchWidget::runSearch(const QString &query) {
    if (!_session || query.isEmpty()) return;
    _resultList->clear();
    _results.clear();

    auto *loadingItem = new QListWidgetItem("Searching…");
    loadingItem->setForeground(QColor("#888"));
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
        auto *item = new QListWidgetItem("No results found.");
        item->setForeground(QColor("#888"));
        _resultList->addItem(item);
        return;
    }

    for (int i = 0; i < (int)results.size(); ++i) {
        const auto &r = results[i];

        // "#channel  h:mm AM  • message preview"
        const QString convLabel = r.convName.isEmpty()
            ? r.conv.value : "#" + r.convName;
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
