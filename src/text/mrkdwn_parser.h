// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
// Slack mrkdwn → TextWithEntities.
// Does NOT use cmark-gfm — Slack mrkdwn is its own grammar (see PLAN2.md §8).
#pragma once

#include "backend/domain.h"

namespace MrkdwnParser {

// Parse Slack mrkdwn into plain text + entity offsets.
// Entity offsets are into the returned .text (not the original input).
TextWithEntities parse(const QString &mrkdwn);

} // namespace MrkdwnParser
