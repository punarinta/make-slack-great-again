// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

// Shared sizing for the web-style form controls (StyledLineEdit / StyledButton),
// so inputs and buttons line up to the same height and corner radius in a row.
namespace Ui {

inline constexpr int kControlHeight       = 38; // default input / button height
inline constexpr int kControlHeightSmall  = 30; // compact controls (toolbars, dense rows)
inline constexpr int kControlHeightXSmall = 22; // banner / inline buttons (update bar, etc.)
inline constexpr int kControlRadius       = 6;  // corner radius on inputs and buttons

} // namespace Ui
