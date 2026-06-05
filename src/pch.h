// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Precompiled header — included before everything else.
// Covers the missing <utility>/<functional> that lib_rpl normally gets from tdesktop's PCH.
#pragma once

#include <utility>
#include <functional>
#include <vector>
#include <optional>
#include <variant>
#include <memory>
#include <unordered_map>
#include <string>

#include <QString>
#include <QObject>
#include <QDebug>

#include "rpl/rpl.h"
