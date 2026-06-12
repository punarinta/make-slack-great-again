// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QString>
#include <QUrl>

namespace MailtoLink {

inline bool isMailto(const QUrl &url) {
    return url.scheme().compare(QStringLiteral("mailto"), Qt::CaseInsensitive) == 0;
}

// Bare address part: "mailto:a@b?subject=hi" → "a@b".
inline QString address(const QUrl &url) {
    return url.path();
}

// Opens the URL with the system mail client when one is registered; otherwise
// copies the address to the clipboard. Returns true when the clipboard
// fallback fired (so the caller can tell the user where the address went).
bool openOrCopy(const QUrl &url);

} // namespace MailtoLink
