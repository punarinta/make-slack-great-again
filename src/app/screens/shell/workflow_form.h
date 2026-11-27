// A form a workflow (or an app) shows: model::Form as an in-window dialog.
//
// Each field is its label ("(optional)" after it when it may stay empty), its
// input, its hint, and a line for what is wrong with it. Text, paragraphs and
// numbers are text fields; people and channels a search field adding chips;
// check boxes and radio buttons as such; a list's single choice a drop-down
// (several: check boxes); dates and times the date/time fields (an optional
// one has a tick to give it at all). Notes are text between the fields.
//
// Submit checks the required fields (and numbers, minimum lengths) first —
// the service takes an empty required field and posts it — then sends the
// answers once (Backend::submitForm). The service's own refusals show under
// their fields; a next step opens in this one's place; done, it closes. Close,
// × and Escape dismiss it there too (Backend::closeForm).
//
// A required field this app can't fill in (a file upload) can't be submitted
// from here: the dialog says so and offers the conversation in Slack instead.
#pragma once

#include "app/model/types.h"
#include "screens/common/context.h"
#include "ui/ui.h"

#include <functional>
#include <string>
#include <vector>

namespace shell {

// What keeps answers from going: one message per field ("" = fine).
// Required fields left empty, numbers that aren't, text under its minimum.
std::vector<std::string>
formProblems(const model::Form &form, const std::vector<model::FormAnswer> &answers);
// True when a required field can't be filled in here (Unsupported).
bool formNeedsSlack(const model::Form &form);

// Shows `form` from `ctx`'s backend. done() once it is over: submitted to
// the end (sent = true) or dismissed (false).
ui::Popup *showWorkflowForm(
    screens::Context &ctx, ui::Window &w, model::Form form, std::function<void(bool sent)> done
);

} // namespace shell
