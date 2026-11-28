// The message dialogs, on ui::Dialog:
//
//   Delete message   "This action cannot be undone.", the message, Cancel /
//                    Delete (danger) → Backend::remove
//   Move to thread   pick a thread of this channel (filter, Up/Down, Enter),
//                    optional "Add a note with the original author and time";
//                    posts the text into the thread, deletes the original only
//                    once that succeeded, opens the thread
//   Reminder         "When" (a date, today or later) and "Time" (now + 1 h,
//                    rounded up to 5 min) → Backend::setReminder
//   Table viewer     a CSV (≤ 400 rows) or a message's table as a data
//                    table on the viewer backdrop; Esc or a click outside
//                    closes it
#pragma once

#include "app/screens/messages/context_fwd.h"

#include <string>
#include <vector>

namespace screens {

// The preview card the delete and forward dialogs show: author, time and the
// message body (bodyH high, scrolling).
ui::View *
addMessagePreview(Context &ctx, ui::View *parent, const model::Message &m, float bodyH, bool files);

ui::Popup *showDeleteMessageDialog(Context &ctx, ui::Window &w, ConvRef conv, Ts ts);
// roots: the loaded thread roots of the channel, newest first
// (MessageList::threadRoots), the message itself excluded by the caller.
ui::Popup *
showMoveToThreadDialog(Context &ctx, ui::Window &w, ConvRef conv, Ts ts, std::vector<Ts> roots);
ui::Popup *showReminderDialog(Context &ctx, ui::Window &w, ConvRef conv, Ts ts);
// The table viewer over parsed CSV rows (readCsvFile): "This file is
// empty" for none, the first 400 rows of a longer one.
ui::Popup *showTableViewer(ui::Window &w, std::vector<std::vector<std::string>> rows);
// A Block Kit table's cells (mrkdwn, row 0 the header) in the same viewer,
// every row, rich as in the message.
ui::Popup *
showTableViewer(ui::Window &w, Context &ctx, std::vector<std::vector<std::string>> cells);
// A CSV file read and parsed: disk and CPU work for a worker thread
// (model::runInBackground); no UI text. No rows when unreadable.
std::vector<std::vector<std::string>> readCsvFile(const std::string &path);

// The text of a message moved to a thread (exposed for tests).
std::string movedMessageText(const Context &ctx, const model::Message &m, bool withNote);
// CSV text as rows of cells, row 0 the header (exposed for tests).
std::vector<std::vector<std::string>> parseCsv(std::string_view text);

} // namespace screens
