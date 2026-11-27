// Workflows (Slack's Workflow Builder) and the forms they show, for
// SlackBackend (see the Workflows section of slack_backend.h).
//
// A workflow starts from its link ("https://slack.com/shortcuts/Ft0…/<hash>",
// workflows.triggers.info gives it for a trigger id: a channel's featured
// workflows name only the id). workflows.triggers.trip answers ok and
// nothing more: the form, if the workflow has one, arrives as a view_opened
// frame on the RTM socket carrying the trip's client_token — so the presence
// link is held up (even in Native mode) from before the trip until then. A
// run that ends first (workflow_execution_outcome_changed for the trip's
// trigger_execution_id) had no form; so does one that sends nothing in
// kFormWaitMs.
//
// views.submit: Slack doesn't check the required fields (an empty one is
// taken, and posted); the shell does. Its answer carries the next form step
// (response_action "update"/"push" with the view), field errors ("errors"),
// or "clear" (done). Never retried: a repeat would run the next step twice.
#include "app/slack/rtm_presence.h"
#include "app/slack/slack_backend.h"
#include "app/slack/slack_json.h"

#include "base/log.h"
#include "base/str.h"
#include "base/time.h"
#include "plat/plat.h"

#include <unordered_map>
#include <utility>

namespace slack {

using model::ConvRef;

namespace {

// How long a start waits for the presence link to come up.
constexpr int kLinkWaitMs = 10'000;

// A run's outcome that means it is over (it no longer waits for anyone).
bool finished(std::string_view status) {
    return status == "SUCCESS" || status == "FAILED" || status == "ERROR" || status == "CANCELED" ||
           status == "CANCELLED" || status == "TIMED_OUT";
}

} // namespace

struct SlackBackend::Flows {
    explicit Flows(SlackBackend &b) : b(b) {}
    ~Flows() {
        for (Start &s : starts) {
            if (s.timer)
                b._app.cancelTimer(s.timer);
            if (s.held)
                if (RtmPresence *rtm = b.rtmLink())
                    rtm->release();
        }
    }

    SlackBackend &b;
    // Per trigger id: its link, title and description (workflows.triggers.info).
    struct Known {
        std::string         url;
        model::WorkflowLink link;
    };
    std::unordered_map<std::string, Known> known;

    // A start waiting for its form.
    struct Start {
        std::string   token;     // the trip's client_token
        std::string   execution; // the trip's trigger_execution_id, once answered
        ConvRef       conv = model::kNoConv;
        FormDone      done;
        plat::TimerId timer = 0;
        bool          held  = false; // a hold of the presence link is out
    };
    std::vector<Start> starts;
    uint64_t           seq = 0;

    std::string token() {
        return str::concat({"msga-", str::number(base::monotonicMs()), "-", str::number(++seq)});
    }

    // Ends a start (by its token): its timer and hold go, then done.
    void finish(const std::string &token, model::Form form, std::string error) {
        for (size_t i = 0; i < starts.size(); ++i) {
            if (starts[i].token != token)
                continue;
            Start s = std::move(starts[i]);
            starts.erase(starts.begin() + long(i));
            if (s.timer)
                b._app.cancelTimer(s.timer);
            if (s.held)
                if (RtmPresence *rtm = b.rtmLink())
                    rtm->release();
            if (!form.id.empty() && s.conv != model::kNoConv &&
                s.conv < b._store.conversationCount())
                form.fallbackUrl = b._store.conversationLink(s.conv);
            if (s.done)
                s.done(std::move(form), std::move(error));
            return;
        }
    }

    Start *find(const std::string &token) {
        for (Start &s : starts)
            if (s.token == token)
                return &s;
        return nullptr;
    }

    // The trip itself, once the link is up.
    void trip(const std::string &token, const std::string &url, const std::string &channel) {
        json::Writer ctx;
        ctx.beginObject().key("location").value("slash_command");
        ctx.key("channel_id").value(channel).endObject();
        b.api(
            "workflows.triggers.trip",
            net::formEncode({
                {"client_token", token},
                {"url", url},
                {"context", ctx.str()},
                {"run_precheck", "true"},
            }),
            [this, token](const json::Document &doc, const std::string &err) {
                if (err == "cancelled")
                    return;
                if (!err.empty()) {
                    LOG_WARN("slack", "workflows.triggers.trip: %s", err.c_str());
                    finish(token, {}, err);
                    return;
                }
                if (Start *s = find(token))
                    s->execution = std::string(doc.root()["trigger_execution_id"].str());
            }
        );
    }
};

SlackBackend::Flows *SlackBackend::newFlows(SlackBackend &b) {
    return new Flows(b);
}

void SlackBackend::deleteFlows(Flows *f) {
    delete f;
}

void SlackBackend::lookUpWorkflow(
    const std::string &trigger, std::function<void(model::WorkflowLink)> done
) {
    if (const auto it = _flows->known.find(trigger);
        it != _flows->known.end() && !it->second.link.title.empty()) {
        if (done)
            done(it->second.link);
        return;
    }
    readCall(
        "workflows.triggers.info",
        net::formEncode({{"trigger_id", trigger}}),
        [this, trigger, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            model::WorkflowLink link{trigger, {}, {}};
            if (err.empty()) {
                const json::Value t = doc.root()["trigger"];
                Flows::Known     &k = _flows->known[trigger];
                k.url               = std::string(t["shortcut_url"].str());
                const json::Value w = t["workflow"];
                link.title          = std::string(w["title"].str());
                if (link.title.empty())
                    link.title = std::string(t["name"].str());
                link.description = std::string(w["description"].str());
                k.link           = link;
            } else {
                LOG_WARN("slack", "workflows.triggers.info: %s", err.c_str());
            }
            if (done)
                done(std::move(link));
        },
        true // the menu's labels: the paced lane
    );
}

void SlackBackend::startWorkflow(ConvRef conv, const std::string &trigger, FormDone done) {
    const std::string &channel = convId(conv);
    RtmPresence       *rtm     = rtmLink();
    if (!_creds.sessionAuth() || !rtm || channel.empty() || trigger.empty()) {
        if (done)
            done({}, "not_supported");
        return;
    }
    Flows::Start s;
    s.token                 = _flows->token();
    s.conv                  = conv;
    s.done                  = std::move(done);
    s.held                  = true;
    const std::string token = s.token;
    // Nothing by then: it ran without a form (or the frame never came).
    s.timer                 = _app.addTimer(kFormWaitMs, false, [this, token] {
        if (Flows::Start *x = _flows->find(token))
            x->timer = 0;
        _flows->finish(token, {}, {});
    });
    _flows->starts.push_back(std::move(s));
    rtm->hold();

    // The link up, then the trip (the frame comes only while it is).
    auto go = [this, token, channel, alive = _alive](const std::string &url) {
        if (!*alive || !_flows->find(token))
            return;
        RtmPresence *link = rtmLink();
        if (!link) {
            _flows->finish(token, {}, "not_supported");
            return;
        }
        link->whenActive(kLinkWaitMs, [this, token, channel, url, alive](bool up) {
            if (!*alive || !_flows->find(token))
                return;
            if (!up) {
                LOG_WARN("slack", "workflow: the RTM link didn't come up");
                _flows->finish(token, {}, "no_connection");
                return;
            }
            _flows->trip(token, url, channel);
        });
    };
    if (const auto it = _flows->known.find(trigger);
        it != _flows->known.end() && !it->second.url.empty()) {
        go(it->second.url);
        return;
    }
    lookUpWorkflow(trigger, [this, trigger, token, go](model::WorkflowLink) {
        const auto it = _flows->known.find(trigger);
        if (it == _flows->known.end() || it->second.url.empty()) {
            _flows->finish(token, {}, "trigger_not_found");
            return;
        }
        go(it->second.url);
    });
}

void SlackBackend::workflowFrame(const json::Value &frame) {
    const std::string_view type = frame["type"].str();
    if (type == "view_opened") {
        // Ours only when it carries a start's token (others are another
        // client's, or an app's we didn't ask).
        const std::string token(frame["client_token"].str());
        if (token.empty() || !_flows->find(token))
            return;
        model::Form form = mapjson::toForm(frame["view"]);
        if (form.id.empty())
            form.id = std::string(frame["view_id"].str());
        _flows->finish(token, std::move(form), {});
        return;
    }
    if (type == "workflow_execution_outcome_changed") {
        const std::string_view status = frame["workflow_instance_status"].str();
        const std::string_view exec   = frame["trigger_execution_id"].str();
        if (exec.empty() || !finished(status))
            return;
        for (const Flows::Start &s : _flows->starts)
            if (s.execution == exec) {
                // Over before a form came: it had none (or it failed).
                const std::string token = s.token;
                _flows->finish(
                    token,
                    {},
                    status == "SUCCESS" ? std::string() : str::asciiLower(std::string(status))
                );
                return;
            }
    }
}

void SlackBackend::submitForm(
    const model::Form              &form,
    std::vector<model::FormAnswer>  answers,
    std::function<void(FormResult)> done
) {
    if (form.id.empty()) {
        if (done)
            done({{}, {}, "no_form"});
        return;
    }
    // api(), not readCall(): never repeated.
    api("views.submit",
        net::formEncode({
            {"client_token", _flows->token()},
            {"view_id", form.id},
            {"state", mapjson::formState(form, answers)},
        }),
        [this, form, done = std::move(done)](const json::Document &doc, const std::string &err) {
            if (err == "cancelled")
                return;
            FormResult r;
            if (!err.empty()) {
                LOG_WARN("slack", "views.submit: %s", err.c_str());
                r.error = err;
            } else {
                const json::Value      root   = doc.root();
                const std::string_view action = root["response_action"].str();
                if (action == "errors") {
                    r.fieldErrors = mapjson::formErrors(form, root["errors"]);
                } else if ((action == "update" || action == "push") && root["view"].isObject()) {
                    r.next = mapjson::toForm(root["view"]);
                    if (!r.next.id.empty())
                        r.next.fallbackUrl = form.fallbackUrl;
                }
            }
            if (done)
                done(std::move(r));
        });
}

void SlackBackend::closeForm(const model::Form &form) {
    if (form.id.empty())
        return;
    api("views.close",
        net::formEncode({{"client_token", _flows->token()}, {"view_id", form.id}}),
        [](const json::Document &, const std::string &err) {
            if (!err.empty() && err != "cancelled")
                LOG_INFO("slack", "views.close: %s", err.c_str());
        });
}

} // namespace slack
