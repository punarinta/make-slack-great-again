// Transcript items → the messages the Store keeps.
//
// Claude writes plain Markdown, never Slack tokens; the Store keeps mrkdwn.
// So an answer is escaped (& < >), bare URLs and teammate mentions become
// <url> / <@claude:role:x> tokens (Slack's server does that for Slack text),
// headings turn bold (mrkdwn has none) and tables are fenced so they stay
// aligned, then the composer's Markdown → mrkdwn conversion runs.
#pragma once

#include "app/model/types.h"

#include <string>
#include <string_view>
#include <vector>

namespace claude {

struct TranscriptItem;

// The subtype of an agent's mid-task updates: a remark before tool calls, a
// tool card, a subagent's start — shown, never announced (no notification,
// no red badge; Store::answersAreMentions).
inline constexpr char kProgressSubtype[] = "progress";

std::string renderMarkdown(std::string_view markdown);

// A text with a table ("| a | b |" over "|---|---|")
// as blocks — the text around it rendered, the table a Table block of
// rendered cells (row 0 the header). Empty when there is no table: `text`
// says it all.
std::vector<model::Block> markdownBlocks(std::string_view markdown);

// `text` as literal mrkdwn: & < > escaped, and the marks (* _ ~ `) and a
// ":name:" colon as "&#42;" references — plain text, nothing parsed.
std::string escapeMrkdwn(std::string_view text);

// The message msga shows for `item`: prompts are `me`'s, the rest `claude`'s
// (the session's teammate, or the subagent's).
model::Message toMessage(const TranscriptItem &item, model::UserRef me, model::UserRef claude);
// What its text is made of: Markdown to render (*markdown; renderMarkdown
// makes the text of it), else the text itself. Translated here: on the UI
// thread, while renderMarkdown may run anywhere.
std::string    messageSource(const TranscriptItem &item, bool *markdown);

} // namespace claude
