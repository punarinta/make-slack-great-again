// SettingsDialog: the AI assistance, Storage, System and About pages.
#include "screens/settings/settings_dialog.h"
#include "screens/settings/settings_parts.h"

#include "app/cache/workspace_cache.h"
#include "app/diag/mem_stats.h"
#include "app/llm/discussion_summary.h"
#include "app/llm/service.h"
#include "app/llm/wire.h"
#include "app/screens/common/remote_images.h"
#include "app/spell/spell.h"
#ifdef MSGA_SELF_UPDATE
#include "app/update/updater.h"
#endif
#include "base/i18n.h"
#include "base/str.h"
#include "base/time.h"

#ifdef MSGA_HAVE_MESSAGES
#include "app/screens/messages/image_cache.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#endif

using namespace ui;
using i18n::arg;
using i18n::tr;
using i18n::trn;

namespace settings {

namespace {

#ifndef MSGA_VERSION
#define MSGA_VERSION 0
#endif
#ifndef MSGA_BUILD_TIMESTAMP
#define MSGA_BUILD_TIMESTAMP ""
#endif

constexpr float kAiLabelW = 90; // the editor's field-label column

const char *const kLicenseUrl =
    "https://github.com/punarinta/make-slack-great-again/blob/master/LICENSE";
const char *const kIssuesUrl = "https://github.com/punarinta/make-slack-great-again/issues";
const char *const kSlackAppUrl =
    "https://github.com/punarinta/make-slack-great-again/blob/master/docs/SETUP_SLACK.md";
const char *const kGiphyUrl = "https://developers.giphy.com/dashboard/";

// The presets' fixed parts are the LLM layer's table.
using PresetInfo = llm::Preset;
using llm::kCustomSttModel;

const PresetInfo *presetInfo(std::string_view id) {
    return llm::preset(id);
}

std::string modelOf(const shell::AiProvider &p) {
    if (!p.model.empty())
        return p.model;
    const PresetInfo *pi = presetInfo(p.id);
    return pi ? pi->defaultModel : std::string();
}

// What the row says about a key: its last four characters.
std::string keyLabel(const std::string &key) {
    return key.size() > 4 ? "\xE2\x80\xA6" + key.substr(key.size() - 4) : std::string(tr("key"));
}

// The provider the assistant uses: the default while connected, else the
// first connected one.
const shell::AiProvider *activeProvider(const shell::Settings &s) {
    if (const shell::AiProvider *p = s.provider(s.aiDefault); p && p->connected())
        return p;
    for (const shell::AiProvider &p : s.ai)
        if (p.connected())
            return &p;
    return nullptr;
}

// A server that is not this machine or the local network, reached without
// TLS: the key travels in plain text (checked on the normalized URL).
bool cleartextRemote(std::string_view url) {
    return llm::isCleartextRemote(llm::normalizeOpenAiBaseUrl(url));
}

// The process's private memory, the number each OS's task manager shows; 0
// when unknown.
uint64_t privateBytes() {
#if defined(__linux__)
    if (FILE *f = std::fopen("/proc/self/smaps_rollup", "r")) {
        char               line[256];
        unsigned long long clean = 0, dirty = 0, kb = 0;
        while (std::fgets(line, sizeof line, f)) {
            if (std::sscanf(line, "Private_Clean: %llu", &kb) == 1)
                clean = kb;
            else if (std::sscanf(line, "Private_Dirty: %llu", &kb) == 1)
                dirty = kb;
        }
        std::fclose(f);
        if (clean + dirty)
            return (clean + dirty) * 1024;
    }
    const long kb = diag::rssKb(); // kernels < 4.14
    return kb > 0 ? uint64_t(kb) * 1024 : 0;
#elif defined(__APPLE__)
    task_vm_info_data_t    info;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&info), &n) != KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
        return 0;
    // PagefileUsage is the same commit charge; Wine fills only that one.
    return pmc.PrivateUsage ? pmc.PrivateUsage : pmc.PagefileUsage;
#else
    return 0;
#endif
}

// "412.3 MB", "1.2 GB"; a dash when unknown.
std::string formatRam(uint64_t b) {
    return b ? str::byteSize(int64_t(b), str::ByteSize::Exact) : std::string("\xE2\x80\x94");
}

} // namespace

// ── AI assistance ───────────────────────────────────────────────────────────

void SettingsDialog::buildAi() {
    heading(_content, tr("AI provider"));
    caption(
        _content,
        tr("Connect an AI provider to enable assistant features.\n"
           "API keys are stored on this computer and sent only to the provider you "
           "configure.")
    );
    _p->aiList = group(_content, 4);
    auto *add =
        button(_content, tr("Add OpenAI-compatible server\xE2\x80\xA6"), Button::Kind::Secondary);
    add->onClick = [this] { showAiEditor({}); };

    // The inline editor (hidden until Connect / Edit / Add).
    auto *ed     = group(_content);
    _p->aiEditor = ed;
    _p->aiTitle  = heading(ed, "");
    auto fieldIn = [this](View *parent, const char *label) {
        auto *r = row(parent);
        body(r, label)->style().width(kAiLabelW).noShrink();
        auto *f = r->add<TextField>();
        f->style().flex(1);
        return f;
    };
    auto indented = [](View *v) {
        v->style().margins(kAiLabelW + 8, 0, 0, 0);
        return v;
    };
    _p->aiNameRow = group(ed, 0);
    _p->aiName    = fieldIn(_p->aiNameRow, tr("Name"));
    _p->aiName->edit().setPlaceholder(tr("Company vLLM"));

    _p->aiUrlRow = group(ed, 2);
    _p->aiUrl    = fieldIn(_p->aiUrlRow, tr("Server URL"));
    _p->aiUrl->edit().setPlaceholder("http://localhost:8000/v1");
    indented(caption(
        _p->aiUrlRow,
        tr("Works with vLLM, Ollama, LM Studio, LiteLLM, OpenRouter and "
           "other OpenAI-compatible servers.")
    ));
    _p->aiCleartext = caption(
        _p->aiUrlRow,
        tr("Unencrypted connection \xE2\x80\x94 the API key is sent in plain text."),
        C::FormError
    );
    indented(_p->aiCleartext)->setVisible(false);
    _p->aiUrl->edit().onChange = [this] {
        _p->aiCleartext->setVisible(cleartextRemote(_p->aiUrl->text()));
    };

    _p->aiKey = fieldIn(ed, tr("API key"));
    _p->aiKey->setMasked(true);
    auto *keySub  = indented(row(ed));
    _p->aiKeyLink = link(keySub, "", "");
    _p->aiKeyHint = caption(keySub, {});

    auto *modelRow = row(ed);
    body(modelRow, tr("Model"))->style().width(kAiLabelW).noShrink();
    _p->aiModel = modelRow->add<TextField>(tr("Model name"));
    _p->aiModel->style().flex(1);
    auto *fetch =
        modelRow->add<Button>(tr("Fetch models"), Button::Kind::Secondary, Button::Form::Small);
    _p->aiFetch     = fetch;
    _p->aiModelPick = ed->add<Dropdown>(std::vector<std::string>(), 0);
    indented(_p->aiModelPick);
    _p->aiModelPick->onChange = [this](int i) {
        _p->aiModel->setText(_p->aiModelPick->options()[size_t(i)]);
    };

    _p->aiSttRow = group(ed, 2);
    _p->aiStt    = fieldIn(_p->aiSttRow, tr("Speech model"));
    indented(caption(
        _p->aiSttRow,
        tr("Speech-to-text model for voice input and audio transcripts. "
           "Leave empty for the default.")
    ));
    _p->aiProbe = caption(ed, {});
    _p->aiProbe->setVisible(false);

    auto *actions = row(ed);
    auto *test =
        actions->add<Button>(tr("Test connection"), Button::Kind::Secondary, Button::Form::Small);
    _p->aiTest = test;
    actions->add<View>()->style().flex(1);
    auto *cancel = actions->add<Button>(tr("Cancel"), Button::Kind::Ghost, Button::Form::Small);
    auto *save   = actions->add<Button>(tr("Save"), Button::Kind::Primary, Button::Form::Small);
    // Both ask the server for its models (the key and URL as typed, saved or
    // not); without the LLM layer (tests) they show disabled.
    test->setEnabled(_ctx.ai != nullptr);
    fetch->setEnabled(_ctx.ai != nullptr);
    test->onClick   = [this] { probeAiEditor(false); };
    fetch->onClick  = [this] { probeAiEditor(true); };
    cancel->onClick = [this] {
        _p->aiEditing.clear();
        _p->aiEditor->setVisible(false);
    };
    save->onClick         = [this] { saveAiEditor(); };
    _p->aiKey->onReturn   = [this] { saveAiEditor(); };
    _p->aiModel->onReturn = [this] { saveAiEditor(); };
    _p->aiStt->onReturn   = [this] { saveAiEditor(); };
    ed->setVisible(false);

    _p->aiError               = caption(_content, {}, C::FormError);
    _p->aiError->style().minH = std::ceil(font(Font::Small).size * 1.4f);

    heading(_content, tr("Your language"));
    caption(
        _content,
        tr("AI features address you in this language.\n"
           "It follows the app language until you pick one here.")
    );
    {
        auto *r = row(_content);
        body(r, tr("Native language:"));
        // "Follows the app language"; "system" is the OS locale's.
        const std::string        code = _s.effectiveAiLanguage();
        std::vector<std::string> names;
        int                      sel = -1, en = 0;
        // Language names stay in their own language.
        for (size_t i = 0; i < std::size(llm::kAiLanguages); ++i) {
            names.push_back(spell::nativeName(llm::kAiLanguages[i]));
            if (code == llm::kAiLanguages[i])
                sel = int(i);
            if (std::strcmp(llm::kAiLanguages[i], "en") == 0)
                en = int(i);
        }
        auto *lang = r->add<Dropdown>(std::move(names), sel < 0 ? en : sel);
        lang->style().width(180);
        lang->onChange = [this](int i) {
            _s.aiLanguage = llm::kAiLanguages[i];
            changed();
        };
    }

    heading(_content, tr("Voice input"));
    caption(
        _content,
        tr("Dictate messages with the microphone button in the message box. When you dictate, the "
           "recording and the conversation's recent messages are sent to your AI provider.\n"
           "The speech-to-text model is set in each provider's settings above.")
    );
    _p->voiceWarn = caption(
        _content,
        tr("Voice input needs an OpenAI or OpenAI-compatible provider with speech-to-text."),
        C::FormError
    );
    {
        auto *g = group(_content, 2);
        body(g, tr("Glossary"));
        caption(g, tr("Names, product terms and jargon to spell correctly, one per line."));
        _p->glossary = g->add<TextField>("gRPC\nTerraform\nnginx", true);
        _p->glossary->setText(_s.voiceGlossary);
        _p->glossary->edit().onChange = [this] {
            // Saved half a second after typing stops (and on leaving the page).
            _ctx.app.cancelTimer(_glossaryTimer);
            _glossaryTimer = _ctx.app.addTimer(500, false, [this] {
                _glossaryTimer = 0;
                saveGlossary();
            });
        };
    }
    {
        auto *g = group(_content, 2);
        check(g, tr("Clean up with AI"), &_s.voiceCleanup, false);
        caption(g, tr("Removes filler words and false starts using your AI provider."));
    }
    refreshAiList();
}

void SettingsDialog::refreshAiList() {
    View *list = _p->aiList;
    list->clearChildren();
    const shell::AiProvider *active = activeProvider(_s);
    for (const shell::AiProvider &p : _s.ai) {
        // [radio] Name ……… [buttons], the detail underneath, under the name.
        auto *r   = group(list, 2);
        auto *top = row(r);
        auto *def = top->add<Radio>("", &p == active);
        def->setEnabled(p.connected());
        const std::string id = p.id;
        def->onChange        = [this, id](bool) {
            _s.aiDefault = id;
            changed();
            refreshAiList();
        };
        top->add<Label>(p.name, Font::ControlBold, C::FormText);
        top->add<View>()->style().flex(1);
        if (p.preset() && !p.connected()) {
            auto *c = top->add<Button>(tr("Connect"), Button::Kind::Primary, Button::Form::Small);
            c->onClick = [this, id] { showAiEditor(id); };
        } else {
            auto *e    = top->add<Button>(tr("Edit"), Button::Kind::Secondary, Button::Form::Small);
            e->onClick = [this, id] { showAiEditor(id); };
            auto *drop = top->add<Button>(
                p.preset() ? tr("Disconnect") : tr("Remove"),
                Button::Kind::Danger,
                Button::Form::Small
            );
            drop->onClick = [this, id] {
                _p->aiError->setText({});
                if (_p->aiEditing == id) {
                    _p->aiEditing.clear();
                    _p->aiEditor->setVisible(false);
                }
                shell::AiProvider *q = _s.provider(id);
                if (q && q->preset()) {
                    q->key.clear();
                } else {
                    _s.ai.erase(
                        std::remove_if(
                            _s.ai.begin(),
                            _s.ai.end(),
                            [&](const shell::AiProvider &a) { return a.id == id; }
                        ),
                        _s.ai.end()
                    );
                    // A later server may reuse the id: it must not inherit the
                    // default.
                    if (_s.aiDefault == id)
                        _s.aiDefault.clear();
                }
                changed();
                refreshAiList();
            };
        }
        const std::string detail =
            p.preset()
                ? (p.connected()
                       ? str::concat(
                             {modelOf(p), " \xC2\xB7 ", arg(tr("Connected (%1)"), keyLabel(p.key))}
                         )
                       : std::string(tr("Not connected")))
                : str::concat({modelOf(p), " \xC2\xB7 ", p.url});
        auto *dl = caption(r, detail);
        dl->style().margins(18 + 8, 0, 0, 0);
    }
    refreshVoiceHint();
}

void SettingsDialog::refreshVoiceHint() {
    // Speech-to-text needs an OpenAI-format endpoint (not Anthropic).
    bool stt = false;
    for (const shell::AiProvider &p : _s.ai)
        stt |= p.connected() && p.id != "anthropic";
    if (_p->voiceWarn)
        _p->voiceWarn->setVisible(!stt);
}

void SettingsDialog::showAiEditor(const std::string &id) {
    const shell::AiProvider *p = _s.provider(id);
    ++_p->aiProbeSeq; // drop the answer of a probe started for the previous edit
    _p->aiTest->setEnabled(_ctx.ai != nullptr);
    _p->aiFetch->setEnabled(_ctx.ai != nullptr);
    _p->aiEditing = p ? id : std::string();
    _p->aiError->setText({});
    _p->aiProbe->setVisible(false);
    const bool        preset = p && p->preset();
    const PresetInfo *pi     = p ? presetInfo(p->id) : nullptr;
    _p->aiNameRow->setVisible(!preset);
    _p->aiUrlRow->setVisible(!preset);
    _p->aiKeyLink->setVisible(preset);
    _p->aiModelPick->setVisible(preset); // custom: filled by "Fetch models"
    _p->aiName->setText(p ? p->name : std::string());
    _p->aiUrl->setText(p ? p->url : std::string());
    _p->aiKey->setText({});
    _p->aiModel->setText(p ? modelOf(*p) : std::string());
    _p->aiCleartext->setVisible(!preset && cleartextRemote(_p->aiUrl->text()));
    std::string hint;
    if (!p) {
        _p->aiTitle->setText(tr("Add OpenAI-compatible server"));
        _p->aiKey->edit().setPlaceholder(tr("Optional"));
        hint = tr("Leave empty if the server doesn't need one.");
    } else {
        _p->aiTitle->setText(preset && !p->connected() ? arg(tr("Connect %1"), p->name) : p->name);
        if (!p->key.empty()) {
            _p->aiKey->edit().setPlaceholder(
                arg(tr("Key saved (%1) \xE2\x80\x94 leave empty to keep it"), keyLabel(p->key))
            );
        } else {
            _p->aiKey->edit().setPlaceholder(tr("Paste your API key"));
            if (!preset)
                hint = tr("Leave empty if the server doesn't need one.");
        }
    }
    if (preset) {
        text::AttributedText t;
        t.append(arg(tr("Get an API key from %1\xE2\x80\xA6"), p->name), linkStyle());
        _p->aiKeyLink->setRichText(std::move(t));
        const std::string url = pi->keyUrl;
        _p->aiKeyLink->onLink = [this, url](uint32_t) {
            if (_ctx.openUrl)
                _ctx.openUrl(url);
        };
        std::vector<std::string> models;
        const std::string        cur = modelOf(*p);
        if (std::find(std::begin(pi->models), std::end(pi->models), cur) == std::end(pi->models))
            models.push_back(cur); // a typed model stays listed
        for (const char *m : pi->models)
            models.push_back(m);
        const int sel = int(std::find(models.begin(), models.end(), cur) - models.begin());
        _p->aiModelPick->setOptions(std::move(models), sel);
    }
    _p->aiKeyHint->setText(hint);
    _p->aiKeyHint->setVisible(!hint.empty());
    // Speech-to-text only on OpenAI-format endpoints.
    const bool stt = !p || p->id != "anthropic";
    _p->aiSttRow->setVisible(stt);
    _p->aiStt->setText(p ? p->sttModel : std::string());
    _p->aiStt->edit().setPlaceholder(pi && pi->sttModel ? pi->sttModel : kCustomSttModel);
    _p->aiEditor->setVisible(true);
    (preset ? _p->aiKey : _p->aiName)->edit().focus();
}

void SettingsDialog::probeAiEditor(bool fillModels) {
    // GET /models with what the editor holds.
    _p->aiError->setText({});
    if (!_ctx.ai)
        return;
    const shell::AiProvider *saved = _s.provider(_p->aiEditing);
    // Presets ignore the URL (fromSettings takes theirs).
    const std::string        url   = llm::normalizeOpenAiBaseUrl(_p->aiUrl->text());
    std::string              key(str::trim(_p->aiKey->text()));
    if (key.empty() && saved)
        key = saved->key;
    const std::string   id = saved ? saved->id : std::string("custom-new");
    const llm::Provider cfg =
        llm::fromSettings(id, saved ? saved->name : std::string(), url, key, {}, {});
    if (cfg.baseUrl.empty()) {
        _p->aiError->setText(tr("Enter the server URL first"));
        return;
    }
    const int seq = ++_p->aiProbeSeq;
    _p->aiTest->setEnabled(false);
    _p->aiFetch->setEnabled(false);
    _p->aiProbe->setText(tr("Connecting\xE2\x80\xA6"));
    _p->aiProbe->setVisible(true);
    std::weak_ptr<char> alive = _p->alive;
    _ctx.ai->listModels(cfg, [this, alive, seq, fillModels](llm::ModelsResult r) {
        if (alive.expired() || seq != _p->aiProbeSeq)
            return;
        _p->aiTest->setEnabled(true);
        _p->aiFetch->setEnabled(true);
        if (!r.ok) {
            _p->aiProbe->setVisible(false);
            _p->aiError->setText(r.error);
            return;
        }
        _p->aiProbe->setText(
            trn("Reached the server \xE2\x80\x94 %n model available",
                "Reached the server \xE2\x80\x94 %n models available",
                int64_t(r.models.size()))
        );
        if (!fillModels || r.models.empty())
            return;
        // The picker can't show "nothing selected": an empty field takes the
        // first model, a typed one not on the list is added to it.
        std::string current(str::trim(_p->aiModel->text()));
        if (current.empty()) {
            current = r.models.front();
            _p->aiModel->setText(current);
        }
        std::vector<std::string> models;
        if (std::find(r.models.begin(), r.models.end(), current) == r.models.end())
            models.push_back(current);
        for (std::string &m : r.models)
            models.push_back(std::move(m));
        const int sel = int(std::find(models.begin(), models.end(), current) - models.begin());
        _p->aiModelPick->setOptions(std::move(models), sel);
        _p->aiModelPick->setVisible(true);
    });
}

void SettingsDialog::saveAiEditor() {
    _p->aiError->setText({});
    shell::AiProvider *p = _s.provider(_p->aiEditing);
    const std::string  key(str::trim(_p->aiKey->text()));
    const std::string  url = llm::normalizeOpenAiBaseUrl(_p->aiUrl->text());
    const std::string  model(str::trim(_p->aiModel->text()));
    std::string        stt(str::trim(_p->aiStt->text()));
    if (p && p->preset()) {
        if (key.empty() && p->key.empty()) {
            _p->aiError->setText(tr("Paste your API key"));
            return;
        }
        const PresetInfo *pi = presetInfo(p->id);
        if (!key.empty())
            p->key = key;
        p->model    = model == pi->defaultModel ? std::string() : model;
        p->sttModel = pi->sttModel && stt == pi->sttModel ? std::string() : stt;
    } else {
        if (url.empty()) {
            _p->aiError->setText(tr("Enter the server URL (for example http://localhost:8000/v1)"));
            return;
        }
        if (model.empty()) {
            _p->aiError->setText(tr("Enter a model name, or fetch the list from the server"));
            return;
        }
        if (!p) {
            int n = 1;
            while (_s.provider("custom-" + str::number(n)))
                ++n;
            _s.ai.push_back({"custom-" + str::number(n), {}, {}, {}, {}, {}});
            p = &_s.ai.back();
        }
        std::string name(str::trim(_p->aiName->text()));
        if (name.empty()) { // the host
            std::string_view h = url;
            if (const size_t s = h.find("://"); s != std::string_view::npos)
                h.remove_prefix(s + 3);
            name = std::string(h.substr(0, h.find_first_of(":/")));
        }
        p->name     = name;
        p->url      = url;
        p->model    = model;
        p->sttModel = stt == kCustomSttModel ? std::string() : stt;
        if (!key.empty())
            p->key = key;
    }
    _p->aiKey->setText({}); // never keep a pasted key in the view tree
    _p->aiEditing.clear();
    _p->aiEditor->setVisible(false);
    changed();
    refreshAiList();
}

void SettingsDialog::saveGlossary() {
    std::string out;
    std::string text = _p && _p->glossary ? _p->glossary->text() : _s.voiceGlossary;
    std::vector<std::string_view> seen;
    size_t                        i = 0;
    while (i <= text.size()) {
        size_t j = text.find('\n', i);
        if (j == std::string::npos)
            j = text.size();
        const std::string_view term = str::trim(std::string_view(text).substr(i, j - i));
        if (!term.empty() && std::find(seen.begin(), seen.end(), term) == seen.end()) {
            seen.push_back(term);
            out += (out.empty() ? "" : "\n") + std::string(term);
        }
        i = j + 1;
    }
    if (out != _s.voiceGlossary) {
        _s.voiceGlossary = out;
        changed();
    }
}

// ── Storage ─────────────────────────────────────────────────────────────────

void SettingsDialog::buildStorage() {
    heading(_content, tr("Cache"));
    {
        auto *r = row(_content, 16);
        body(r, tr("Cache size:"), C::FormTextMuted);
        _p->cacheSize = r->add<Label>("", Font::ControlBold, C::FormText);
        refreshCache();
    }
    caption(
        _content,
        tr("Conversations, user names, message history, and image thumbnails\n"
           "stored locally to speed up startup.")
    );
    {
        auto *r = row(_content, 16);
        body(r, tr("Limit cache to"));
        auto *cap = r->add<SpinBox>(_s.cacheLimitMb, 50, 10240, tr(" MB"));
        cap->style().width(110);
        cap->onChange = [this](int mb) {
            _s.cacheLimitMb = mb;
            changed(); // the shell bounds the image cache by it
            refreshCache();
        };
    }
    caption(
        _content,
        tr("When the cache grows past this limit, the least recently\n"
           "viewed images are deleted first.")
    );
    auto *clear    = button(_content, tr("Clear cache"), Button::Kind::Danger);
    clear->onClick = [this, clear] {
        clear->setEnabled(false);
#ifdef MSGA_HAVE_MESSAGES
        // Evicts every decoded image; views ask again as they repaint.
        const size_t budget = _ctx.images.budget();
        _ctx.images.setBudget(0);
        _ctx.images.setBudget(budget);
#endif
        // The whole cache directory — the downloaded
        // pictures and every workspace's conversations, users and messages
        // (the next start is a cold one).
        if (_ctx.remote)
            _ctx.remote->clear();
        cache::WorkspaceCache::clearAllAsync(
            _ctx.app.platform(), [this, alive = std::weak_ptr<char>(_p->alive)] {
                if (!alive.expired())
                    refreshCache();
            }
        );
    };

    // 12 px more between the Cache and State blocks.
    heading(_content, tr("State"))->style().margins(0, 12, 0, 0);
    caption(
        _content,
        tr("Sidebar visit history used to decide which conversations are shown.\n"
           "Clear this to let the app re-analyse activity from scratch on next load.")
    );
    auto *state    = button(_content, tr("Clear state"), Button::Kind::Danger);
    // The visit stamps go; the sidebar re-seeds at once.
    state->onClick = [this, state] {
        state->setEnabled(false);
        _s.visitedAt.clear();
        if (_hooks.clearState)
            _hooks.clearState();
        else
            changed();
    };
}

void SettingsDialog::refreshCache() {
    if (!_p->cacheSize)
        return;
    // The cache directory's size (the downloaded pictures and the
    // workspaces' cached data), walked on a worker. Only the newest walk
    // started while this page exists shows.
    const int seq = ++_p->cacheSeq;
    cache::WorkspaceCache::diskBytesAsync(
        _ctx.app.platform(), [this, seq, alive = std::weak_ptr<char>(_p->alive)](int64_t data) {
            if (alive.expired() || seq != _p->cacheSeq)
                return;
            if (_ctx.remote) {
                _p->cacheSize->setText(str::byteSize(int64_t(_ctx.remote->diskBytes() + data)));
                return;
            }
#ifdef MSGA_HAVE_MESSAGES
            _p->cacheSize->setText(str::byteSize(int64_t(_ctx.images.cachedBytes()) + data));
#else
            _p->cacheSize->setText(str::byteSize(data));
#endif
        }
    );
}

// ── System ──────────────────────────────────────────────────────────────────

void SettingsDialog::buildSystem() {
    heading(_content, tr("Version"));
    {
        body(
            _content,
            arg(tr("Version %1, built %2"), str::number(MSGA_VERSION), MSGA_BUILD_TIMESTAMP),
            C::FormTextMuted
        );
#ifdef MSGA_SELF_UPDATE // without it a package manager updates msga
        auto *g = group(_content);
        check(g, tr("Check for updates automatically"), &_s.autoUpdates, false);
        caption(
            g,
            tr("When off, msga never contacts the update server on its own \xE2\x80\x94 use "
               "the\nbutton below to look for a new version.")
        );
        auto *checkBtn         = button(g, tr("Check for updates"), Button::Kind::Primary);
        _p->updStatus          = caption(g, {});
        auto      *last        = g->add<Label>("", Font::Caption, C::FormTextFaint);
        // "Last checked: 5 minutes ago"; "Never checked" before the first check.
        const auto lastChecked = [this, last] {
            const int64_t at = _s.lastUpdateCheck;
            last->setText(arg(
                tr("Last checked: %1"),
                at <= 0 ? std::string(tr("Never checked")) : base::relativeTime(at, base::nowSecs())
            ));
        };
        lastChecked();
        // The update status now, then again on each of the updater's changes.
        update::Updater *u      = _hooks.updater;
        const auto       status = [this, checkBtn](bool enabled, std::string text) {
            checkBtn->setEnabled(enabled);
            _p->updStatus->setText(text);
            _p->updStatus->setVisible(!text.empty());
        };
        if (!u)
            status(false, tr("Update checks not available."));
        else if (u->busy())
            status(false, tr("Checking for updates\xE2\x80\xA6"));
        else if (u->ready())
            status(true, tr("Update downloaded \xE2\x80\x94 restart the app to apply."));
        else
            status(true, {});
        if (u) {
            checkBtn->onClick = [u] { u->checkNow(); };
            u->unlisten(_updListener);
            std::weak_ptr<char> alive = _p->alive;
            _updListener = u->listen([alive, status, lastChecked](const update::Updater::Event &e) {
                if (alive.expired())
                    return; // another page is up
                using K = update::Updater::Event::Kind;
                switch (e.kind) {
                case K::Started:
                    status(false, tr("Checking for updates\xE2\x80\xA6"));
                    break;
                case K::UpToDate:
                    status(true, tr("msga is up to date."));
                    lastChecked();
                    break;
                case K::Available:
                    status(
                        false,
                        arg(tr("Version %1 available \xE2\x80\x94 downloading\xE2\x80\xA6"),
                            str::number(int64_t(e.version)))
                    );
                    break;
                case K::Progress:
                    status(
                        false,
                        arg(tr("Downloading update\xE2\x80\xA6 %1%"),
                            str::number(int64_t(e.percent)))
                    );
                    break;
                case K::Ready:
                    status(true, tr("Update downloaded \xE2\x80\x94 restart the app to apply."));
                    lastChecked();
                    break;
                case K::Failed:
                    status(true, arg(tr("Check failed: %1"), e.message));
                    lastChecked();
                    break;
                }
            });
        }
#endif
    }

    heading(_content, tr("Window"));
    {
        auto *g       = group(_content);
        auto *useTray = check(g, tr("Use tray"), &_s.useTray, false);
#ifndef __APPLE__ // macOS minimizes to the Dock; hiding the window there would surprise
        // Needs the tray: greyed out and shown off without one, the choice
        // kept for when it is back.
        auto *minimize = check(g, tr("Minimize to tray"), &_s.minimizeToTray, false);
        auto *desc     = caption(
            g,
            tr("When on, minimizing hides the window to the tray instead of the taskbar.\n"
               "Click the tray icon to bring it back.")
        );
        auto sync = [this, minimize, desc](bool tray) {
            minimize->setEnabled(tray);
            minimize->setChecked(tray && _s.minimizeToTray);
            desc->setColor(tray ? C::FormTextMuted : C::FormTextFaint);
        };
        sync(_s.useTray);
        useTray->onChange = [this, sync](bool on) {
            _s.useTray = on;
            sync(on);
            changed();
        };
#else
        (void)useTray;
#endif
    }

    heading(_content, tr("Presence"));
    {
        auto *g  = group(_content);
        auto *rg = radios(
            g,
            {tr("Show me as active while MSGA is running"),
             tr("Show me as active while I use MSGA (away after 30 minutes without "
                "input)"),
             tr("Leave my presence to the official Slack apps")},
            &_s.presence
        );
        andThen(rg, [this](int) { changed(); });
        caption(
            g,
            tr("Slack only shows you as active while a Slack app is connected. MSGA can hold "
               "that connection itself, so you no longer need the official app open to look "
               "online. The Hide button still makes you appear away. Works for workspaces "
               "added with a Slack session.")
        );
    }

    heading(_content, tr("Slack connection"));
    caption(_content, tr("Choose how msga connects to Slack."));
    static const bool startupSession = _s.slackSession;
    {
        auto *box = group(_content);
        box->setBackground(C::None, 4);
        box->setBorder(C::FormDivider);
        box->style().padding(12);
        auto *rg = box->add<RadioGroup>(
            strs(
                {tr("Slack session \xE2\x80\x94 no app keys, uses your own account's limits"),
                 tr("Slack app keys \xE2\x80\x94 OAuth sign-in with live message push")}
            ),
            _s.slackSession ? 0 : 1
        );
        _p->modeRestart = caption(box, tr("Restart msga to apply this change."));
        _p->modeRestart->setVisible(_s.slackSession != startupSession);
        rg->onChange = [this](int i) {
            _s.slackSession = i == 0;
            _p->sessionBox->setVisible(_s.slackSession);
            _p->appKeysBox->setVisible(!_s.slackSession);
            _p->modeRestart->setVisible(_s.slackSession != startupSession);
            changed();
        };
    }
    {
        _p->sessionBox = group(_content, 4);
        caption(
            _p->sessionBox,
            tr("Add a workspace using your existing Slack session. New messages "
               "arrive by polling \xE2\x80\x94 there's no live push in this mode.")
        );
        auto *import =
            button(_p->sessionBox, tr("Import Slack session\xE2\x80\xA6"), Button::Kind::Primary);
        import->setEnabled(bool(_hooks.importSlackSession));
        import->onClick = [this] {
            if (_hooks.importSlackSession)
                _hooks.importSlackSession();
        };
        // Slack workspaces still on app keys: one click converts them, reusing
        // the session cookie.
        if (const int n = _hooks.oauthSlackWorkspaces; n > 0) {
            caption(
                _p->sessionBox,
                trn("You still have %n Slack workspace on app keys. Convert them to session so "
                    "no workspace uses Socket Mode.",
                    "You still have %n Slack workspaces on app keys. Convert them to session so "
                    "no workspace uses Socket Mode.",
                    n)
            );
            auto *conv =
                button(_p->sessionBox, tr("Convert them to session"), Button::Kind::Secondary);
            conv->setEnabled(bool(_hooks.convertToSession));
            conv->onClick = [this] {
                if (_hooks.convertToSession)
                    _hooks.convertToSession();
            };
        }
        _p->sessionBox->setVisible(_s.slackSession);
    }
    {
        _p->appKeysBox = group(_content);
        caption(
            _p->appKeysBox,
            tr("Run your own Slack app so you don't share connection keys with "
               "other devices and users. Leave a field empty to use the "
               "built-in default.")
        );
        link(_p->appKeysBox, tr("How to create your Slack app\xE2\x80\xA6"), kSlackAppUrl);
        auto *box = group(_p->appKeysBox, 4);
        box->setBackground(C::None, 4);
        box->setBorder(C::FormDivider);
        box->style().padding(12);
        _p->credId = fieldRow(box, tr("Client ID"), tr("e.g. 1234567890.1234567890"));
        _p->credId->setText(_s.slackClientId);
        _p->credSecret = fieldRow(box, tr("Client secret"), tr("Paste your client secret"));
        _p->credSecret->setMasked(true);
        _p->credSecret->setText(_s.slackClientSecret);
        _p->credXapp = fieldRow(box, tr("App-level token"), tr("Paste your xapp- token"));
        _p->credXapp->setMasked(true);
        _p->credXapp->setText(_s.slackAppToken);
        _p->credStatus = caption(box, {});
        _p->credStatus->setVisible(false);
        auto *save    = button(box, tr("Save and restart"), Button::Kind::Primary);
        save->onClick = [this] {
            const std::string id(str::trim(_p->credId->text()));
            const std::string secret(str::trim(_p->credSecret->text()));
            const std::string xapp(str::trim(_p->credXapp->text()));
            _p->credStatus->setVisible(true);
            if (id == _s.slackClientId && secret == _s.slackClientSecret &&
                xapp == _s.slackAppToken) {
                _p->credStatus->setText(tr("No changes to save."));
                return;
            }
            _s.slackClientId     = id;
            _s.slackClientSecret = secret;
            _s.slackAppToken     = xapp;
            changed();
            _p->credStatus->setText(tr("Saved. Restarting\xE2\x80\xA6"));
            if (_hooks.restart)
                _hooks.restart();
        };
        _p->appKeysBox->setVisible(!_s.slackSession);
    }

    heading(_content, tr("GIF picker"));
    caption(
        _content,
        tr("Searching GIFs from the message box needs a GIPHY API key. Free keys allow "
           "100 searches an hour, which is plenty for personal use.")
    );
    link(_content, tr("Get a GIPHY API key\xE2\x80\xA6"), kGiphyUrl);
    {
        auto *g = group(_content, 4);
        body(g, tr("API key"));
        // A baked-in key is a working default, so say so rather than leaving
        // an empty box that looks unconfigured.
        _p->giphy = g->add<TextField>(
            shell::Settings::buildGiphyKey().empty()
                ? tr("Paste your GIPHY API key")
                : tr("Using this build's key \xE2\x80\x94 paste one here to override it")
        );
        _p->giphy->setMasked(true, true);
        _p->giphy->setText(_s.giphyKey);
        _p->giphyStatus = caption(g, {});
        _p->giphyStatus->setVisible(false);
        g->add<View>()->style().height(4);
        auto *save    = button(g, tr("Save"), Button::Kind::Primary);
        save->onClick = [this] {
            _s.giphyKey = std::string(str::trim(_p->giphy->text()));
            _p->giphyStatus->setText(_s.giphyKey.empty() ? tr("Key cleared.") : tr("Key saved."));
            _p->giphyStatus->setVisible(true);
            changed();
        };
    }

    heading(_content, tr("Memory"));
    _p->ramLabel = body(_content, {});
    refreshRam();
    _ramTimer = _ctx.app.addTimer(5000, true, [this] { refreshRam(); });
}

void SettingsDialog::refreshRam() {
    if (_p->ramLabel)
        _p->ramLabel->setText(arg(tr("RAM used: %1"), formatRam(privateBytes())));
}

// ── About ───────────────────────────────────────────────────────────────────

void SettingsDialog::buildAbout() {
    heading(_content, tr("License"));
    body(
        _content,
        tr("MSGA \xE2\x80\x94 Make Slack Great Again\n"
           "Copyright \xC2\xA9 2026 Vladimir Osipov\n\n"
           "This program is free software: you can redistribute it and/or modify it under the "
           "terms of the GNU General Public License as published by the Free Software Foundation, "
           "either version 3 of the License, or (at your option) any later version "
           "(GPL-3.0-or-later)."),
        C::FormTextMuted
    );
    link(_content, tr("View full license"), kLicenseUrl);

    heading(_content, tr("Contact"));
    {
        // "%1" is the address, drawn as a link wherever the translation puts it.
        const char            *email = "vladimir@msga.app";
        const std::string_view fmt   = tr("Questions or feedback: %1");
        const size_t           at    = fmt.find("%1");
        text::AttributedText   t;
        t.append(fmt.substr(0, at), font(Font::Control, C::FormText));
        t.append(email, linkStyle());
        if (at != std::string_view::npos)
            t.append(fmt.substr(at + 2), font(Font::Control, C::FormText));
        auto *l = _content->add<Label>(arg(fmt, email), Font::Control);
        l->setRichText(std::move(t));
        l->onLink = [this](uint32_t) {
            if (_ctx.openUrl)
                _ctx.openUrl("mailto:vladimir@msga.app");
        };
    }

    heading(_content, tr("Found a bug?"));
    caption(_content, tr("Report it on GitHub so it can be tracked and fixed."));
    auto *bug    = button(_content, tr("Report a bug"), Button::Kind::Danger);
    bug->onClick = [this] {
        if (_ctx.openUrl)
            _ctx.openUrl(kIssuesUrl);
    };
}

} // namespace settings
