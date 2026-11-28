// "Summarize down": an AI recap of a span of a conversation, from the chosen
// message to the newest loaded one.
//
// The whole wait runs in the background as a job ("Summarizing
// discussion…", model::jobs()): in a channel the threads rooted in the span
// are fetched first (at most 20, one call each, given up on after 15 s) and
// their replies inlined; then the request goes to the active AI provider
// (Context::ai). The answer opens the summary dialog over the window —
// whatever the message list shows by then.
#pragma once

#include "app/screens/messages/context_fwd.h"

#include <string>
#include <vector>

namespace screens {

// span: the messages to summarize, oldest first (no pending copies, no
// system lines). threadMode: they are a thread's (no replies to fetch).
// No connected provider: the "Summaries need an AI provider" notice at once.
void summarizeDown(Context &ctx, ConvRef conv, std::vector<Ts> span, bool threadMode);

// The result card: the Markdown report scrolling, with Copy (Report); a
// short notice without buttons (Failure); the notice with "Open settings" →
// Context::openAiSettings (NoProvider). Escape / × close it.
enum class SummaryKind : uint8_t { Report, Failure, NoProvider };
ui::Popup *
showSummaryDialog(Context &ctx, ui::Window &w, const std::string &markdown, SummaryKind kind);

} // namespace screens
