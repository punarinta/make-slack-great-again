// The Activity page's calls (Backend::loadActivity, setActivityRead), the
// web client's own and session tokens only: activity.feed lists what
// notified me — references, no text — messages.list brings their messages
// for several conversations in one call, and activity.markRead /
// markUnread move an item's read state.
#include "app/slack/slack_backend.h"
#include "app/slack/slack_json.h"
#include "base/json.h"
#include "base/log.h"
#include "plat/plat.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace slack {

using model::ConvRef;
using model::Ts;
using Item = model::Backend::ActivityItem;

namespace {

constexpr const char *kActivityPage  = "20";
// messages.list's conversations per call: more is refused
// (too_many_channels; 9 went through, 39 didn't).
constexpr size_t      kMessagesBatch = 8;

} // namespace

void SlackBackend::loadActivity(
    ActivityFilter filter, bool unreadOnly, std::string cursor, ActivityDone done
) {
    if (!_creds.sessionAuth() || _activityUnavailable) {
        _app.post([alive = _alive, error = _activityError, done = std::move(done)] {
            ActivityFeed failed;
            failed.error = error;
            if (*alive && done)
                done(false, std::move(failed));
        });
        return;
    }
    std::string form = net::formEncode(
        {{"types", mapjson::activityTypes(filter)},
         {"mode", "chrono_v1"},
         {"limit", kActivityPage},
         {"archive_only", "false"},
         {"unread_only", unreadOnly ? "true" : "false"},
         {"priority_only", "false"},
         {"exclude_automations", "false"}}
    );
    if (!cursor.empty())
        form += "&" + net::formEncode({{"cursor", cursor}});
    if (_activityTeamId)
        form += "&" + net::formEncode({{"team_id", _store.workspaceId}});
    readCall(
        "activity.feed",
        std::move(form),
        [this, filter, unreadOnly, cursor, sentTeam = _activityTeamId, done = std::move(done)](
            const json::Document &doc, const std::string &err
        ) mutable {
            if (err == "cancelled")
                return;
            if (!err.empty()) {
                LOG_WARN("slack", "activity.feed: %s", err.c_str());
                // On Grid a session token may resolve in the org: once
                // more with team_id, as the thread feed does.
                if (_grid && !sentTeam && isMethodUnavailable(err)) {
                    _activityTeamId = true;
                    loadActivity(filter, unreadOnly, std::move(cursor), std::move(done));
                    return;
                }
                if (isMethodUnavailable(err)) {
                    _activityUnavailable = true;
                    _activityError       = err;
                }
                ActivityFeed failed;
                failed.error = err;
                if (done)
                    done(false, std::move(failed));
                return;
            }
            ActivityFeed page;
            page.nextCursor = std::string(doc.root()["response_metadata"]["next_cursor"].str());
            for (const json::Value v : doc.root()["items"]) {
                Item item;
                if (mapjson::toActivityItem(v, _store, item))
                    page.items.push_back(std::move(item));
            }
            fillActivity(std::move(page), std::move(done));
        }
    );
}

// The items' messages: the Store's where it holds them, the rest from
// messages.list — or, where that is refused, one loadMessage each. One
// that can't be had keeps ts 0 (the page shows the item without it).
void SlackBackend::fillActivity(ActivityFeed page, ActivityDone done) {
    struct Acc {
        ActivityFeed page;
        ActivityDone done;
        size_t       pending = 1; // this function's own share, dropped at its end
        void         finish() {
            if (--pending == 0 && done)
                done(true, std::move(page));
        }
        void take(ConvRef conv, model::Message m) {
            for (Item &it : page.items)
                if (it.conv == conv && it.ts == m.ts && !it.message.ts)
                    it.message = m.clone();
        }
    };
    auto acc  = std::make_shared<Acc>();
    acc->page = std::move(page);
    acc->done = std::move(done);
    // By conversation: the timestamps still wanted.
    std::vector<std::pair<ConvRef, std::vector<Ts>>> wanted;
    for (Item &it : acc->page.items) {
        if (const model::Message *m = _store.findMessage(it.conv, it.ts)) {
            it.message = m->clone();
            continue;
        }
        auto w = std::find_if(wanted.begin(), wanted.end(), [&](const auto &x) {
            return x.first == it.conv;
        });
        if (w == wanted.end())
            w = wanted.insert(wanted.end(), {it.conv, {}});
        if (std::find(w->second.begin(), w->second.end(), it.ts) == w->second.end())
            w->second.push_back(it.ts);
    }
    const auto oneByOne = [this, acc](const std::vector<std::pair<ConvRef, std::vector<Ts>>> &b) {
        for (const auto &[conv, tss] : b)
            for (Ts ts : tss) {
                ++acc->pending;
                loadMessage(conv, ts, [acc, conv](bool ok, model::Message m) {
                    if (ok)
                        acc->take(conv, std::move(m));
                    acc->finish();
                });
            }
    };
    for (size_t at = 0; at < wanted.size(); at += kMessagesBatch) {
        std::vector<std::pair<ConvRef, std::vector<Ts>>> batch(
            wanted.begin() + at, wanted.begin() + std::min(at + kMessagesBatch, wanted.size())
        );
        if (_messagesListUnavailable) {
            oneByOne(batch);
            continue;
        }
        json::Writer ids;
        ids.beginArray();
        for (const auto &[conv, tss] : batch) {
            ids.beginObject().key("channel").value(convId(conv)).key("timestamps").beginArray();
            for (Ts ts : tss)
                ids.value(model::formatTs(ts));
            ids.endArray().endObject();
        }
        ids.endArray();
        ++acc->pending;
        readCall(
            "messages.list",
            net::formEncode(
                {{"message_ids", ids.str()},
                 {"org_wide_aware", "true"},
                 {"cached_latest_updates", "{}"}}
            ),
            [this, acc, batch, oneByOne](const json::Document &doc, const std::string &err) {
                if (err == "cancelled")
                    return;
                if (!err.empty()) {
                    LOG_WARN("slack", "messages.list: %s", err.c_str());
                    if (isMethodUnavailable(err))
                        _messagesListUnavailable = true;
                    oneByOne(batch);
                } else {
                    const json::Value data = doc.root()["messages_data"];
                    for (const auto &[conv, tss] : batch)
                        for (const json::Value v : data[convId(conv)]["messages"])
                            acc->take(conv, mapjson::toMessage(v, _store));
                }
                acc->finish();
            }
        );
    }
    acc->finish();
}

void SlackBackend::setActivityRead(const ActivityItem &item, bool read) {
    if (!_creds.sessionAuth() || item.key.empty())
        return;
    json::Writer entries;
    entries.beginArray().beginObject();
    entries.key("type").value(item.type).key("key").value(item.key);
    entries.key("feed_ts").value(item.feedTs).endObject().endArray();
    std::string form = net::formEncode({{"entries", entries.str()}});
    if (_activityTeamId)
        form += "&" + net::formEncode({{"team_id", _store.workspaceId}});
    api(read ? "activity.markRead" : "activity.markUnread",
        std::move(form),
        std::function<void(const json::Document &)>(),
        Done());
}

} // namespace slack
