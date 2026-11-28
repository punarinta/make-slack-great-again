#include "app/llm/wire.h"

#include "base/crypto.h"
#include "base/i18n.h"
#include "base/json.h"
#include "base/mime.h"
#include "base/str.h"
#include "base/utf8.h"

#include <algorithm>

namespace llm {

using i18n::arg;
using i18n::tr;

namespace {

constexpr const char *kAnthropicVersion = "2023-06-01";

std::string joinUrl(std::string_view base, std::string_view path) {
    while (!base.empty() && base.back() == '/')
        base.remove_suffix(1);
    return str::concat({base, path});
}

void writeMessages(json::Writer &w, const Request &req, bool systemAsMessage) {
    w.key("messages").beginArray();
    if (systemAsMessage && !req.system.empty())
        w.beginObject().key("role").value("system").key("content").value(req.system).endObject();
    for (const Message &m : req.messages)
        w.beginObject()
            .key("role")
            .value(m.role == Message::Role::User ? "user" : "assistant")
            .key("content")
            .value(m.text)
            .endObject();
    w.endArray();
}

void auth(net::Request &r, const Endpoint &ep) {
    if (ep.format == Format::AnthropicMessages) {
        r.headers.push_back({"anthropic-version", kAnthropicVersion});
        r.headers.push_back({"x-api-key", ep.apiKey});
    } else if (!ep.apiKey.empty()) {
        r.headers.push_back({"Authorization", "Bearer " + ep.apiKey});
    }
}

// Text of a `content` that is a string or an array of {type:"text", text}
// parts (some compat servers send the latter).
std::string contentText(json::Value content) {
    if (content.isString())
        return std::string(content.str());
    std::string out;
    for (json::Value part : content)
        if (part["type"].str() == "text" || part.has("text"))
            out += part["text"].str();
    return out;
}

// The error a JSON body carries, in the shapes seen in the wild:
//   {"error":{"message":…}}          OpenAI, Anthropic, Ollama, newer vLLM
//   {"error":"…"}                    some gateways
//   {"object":"error","message":…}   older vLLM
//   {"message":…} on a 4xx/5xx       generic
std::string jsonError(json::Value o, int status) {
    const json::Value err = o["error"];
    if (err.isObject()) {
        const std::string_view m = err["message"].str();
        return m.empty() ? std::string(tr("unknown error")) : std::string(m);
    }
    if (err.isString())
        return std::string(err.str());
    if (o["object"].str() == "error" || status >= 400)
        return std::string(o["message"].str());
    return {};
}

// What a 404 means for the endpoint called: the chat URL is the one users
// mistype; a missing transcription route is a server without STT.
enum class Route : uint8_t { Chat, Transcription };

std::string httpFailure(const net::Response &r, Route route = Route::Chat) {
    if (r.status == 0)
        return transportFailure(r.error);
    if (r.status == 404 && route == Route::Transcription)
        return tr("This server has no speech-to-text endpoint (HTTP 404)");
    if (r.status == 404)
        return tr(
            "No chat endpoint at this URL (HTTP 404) \xE2\x80\x94 most servers expect it to "
            "end in /v1"
        );
    const std::string snippet = utf8::ellipsize(str::simplified(r.body), 200);
    const std::string status  = str::number(r.status);
    return snippet.empty() ? arg(tr("HTTP %1"), status) : arg(tr("HTTP %1: %2"), status, snippet);
}

// Transport, HTTP and JSON-carried errors; true with `doc` parsed otherwise.
bool preflight(
    const net::Response &r, json::Document &doc, std::string &error, Route route = Route::Chat
) {
    if (r.status == 0) {
        error = httpFailure(r, route);
        return false;
    }
    if (!doc.parse(std::string_view(r.body), nullptr) || !doc.root().isObject()) {
        error = r.status >= 400 ? httpFailure(r, route)
                                : std::string(tr("Unexpected response from server (not JSON)"));
        return false;
    }
    error = jsonError(doc.root(), r.status);
    if (!error.empty())
        return false;
    if (r.status >= 400) {
        error = httpFailure(r, route);
        return false;
    }
    return true;
}

bool hasLineBreak(std::string_view s) {
    return s.find('\r') != std::string_view::npos || s.find('\n') != std::string_view::npos;
}

} // namespace

net::Request buildChat(const Endpoint &ep, const Request &req) {
    net::Request out;
    out.method    = "POST";
    out.timeoutMs = kTimeoutMs;
    out.headers.push_back({"Content-Type", "application/json"});
    auth(out, ep);
    json::Writer w;
    w.beginObject().key("model").value(req.model);
    writeMessages(w, req, ep.format == Format::OpenAiChat);
    if (ep.format == Format::AnthropicMessages) {
        out.url = joinUrl(ep.baseUrl, "/v1/messages");
        w.key("max_tokens").value(req.maxTokens);
        if (!req.system.empty())
            w.key("system").value(req.system);
        if (req.temperature)
            w.key("temperature").value(std::clamp(*req.temperature, 0.0, 1.0));
    } else {
        out.url = joinUrl(ep.baseUrl, "/chat/completions");
        w.key(ep.maxCompletionTokens ? "max_completion_tokens" : "max_tokens").value(req.maxTokens);
        const bool reasoning = !ep.reasoningEffort.empty() && ep.reasoningEffort != "none";
        if (!ep.reasoningEffort.empty())
            w.key("reasoning_effort").value(ep.reasoningEffort);
        if (req.temperature && !reasoning)
            w.key("temperature").value(*req.temperature);
    }
    w.endObject();
    out.body = w.take();
    return out;
}

net::Request buildListModels(const Endpoint &ep) {
    net::Request out;
    out.timeoutMs = kTimeoutMs;
    out.url =
        joinUrl(ep.baseUrl, ep.format == Format::AnthropicMessages ? "/v1/models" : "/models");
    auth(out, ep);
    return out;
}

ChatResult parseChat(Format format, const net::Response &r) {
    ChatResult     out;
    json::Document doc;
    if (!preflight(r, doc, out.error))
        return out;
    const json::Value o = doc.root();
    out.response.model  = std::string(o["model"].str());
    if (format == Format::AnthropicMessages) {
        out.response.stopReason = std::string(o["stop_reason"].str());
        // A safety refusal can come with an empty content array: an error,
        // not an empty completion.
        if (out.response.stopReason == "refusal") {
            out.error = "refusal";
            return out;
        }
        for (json::Value block : o["content"])
            if (block["type"].str() == "text")
                out.response.text += block["text"].str();
    } else {
        const json::Value choices = o["choices"];
        if (choices.size() == 0) {
            out.error = tr("Unexpected response from server (no choices)");
            return out;
        }
        out.response.stopReason = std::string(choices[0]["finish_reason"].str());
        out.response.text       = contentText(choices[0]["message"]["content"]);
    }
    out.ok = true;
    return out;
}

ModelsResult parseModels(const net::Response &r) {
    ModelsResult   out;
    json::Document doc;
    if (!preflight(r, doc, out.error))
        return out;
    // OpenAI and Anthropic agree on {"data":[{"id":…},…]}.
    for (json::Value m : doc.root()["data"])
        if (!m["id"].str().empty())
            out.models.emplace_back(m["id"].str());
    out.ok = true;
    return out;
}

bool supportsTranscription(Format format) {
    return format == Format::OpenAiChat;
}

net::Request
buildTranscription(const Endpoint &ep, const TranscriptionInput &in, std::string_view boundaryIn) {
    net::Request out;
    out.method    = "POST";
    out.timeoutMs = kTimeoutMs;
    out.url       = joinUrl(ep.baseUrl, "/audio/transcriptions");
    auth(out, ep);
    // Random bytes never occur as this text inside compressed audio; no need
    // to scan the payload.
    std::string boundary(boundaryIn);
    if (boundary.empty())
        boundary = "msga-" + crypto::randomHex(16);
    net::Multipart form(boundary);
    out.headers.push_back({"Content-Type", form.contentType()});
    form.reserve(in.audio.size() + 1024);
    form.field("model", in.model);
    form.field("response_format", "json");
    if (!str::trim(in.prompt).empty())
        form.field("prompt", in.prompt);
    if (str::startsWith(in.model, "gpt-")) {
        for (const std::string &k : sanitizeTranscriptionKeywords(in.keywords))
            form.field("keywords[]", k);
        std::vector<std::string> seen;
        for (const std::string &raw : in.languages) {
            const std::string lang = str::asciiLower(str::trim(raw));
            if (lang.empty() || hasLineBreak(lang) ||
                std::find(seen.begin(), seen.end(), lang) != seen.end())
                continue;
            seen.push_back(lang);
            form.field("languages[]", lang);
        }
    }
    // Quotes in the file name would break the header; the id-based names
    // callers pass never carry any, but Multipart makes it safe anyway.
    form.file("file", in.fileName, in.mimeType, in.audio);
    out.body = form.body();
    return out;
}

std::vector<std::string> sanitizeTranscriptionKeywords(const std::vector<std::string> &keywords) {
    std::vector<std::string> out, seenFolded;
    for (const std::string &raw : keywords) {
        if (raw.find_first_of("<>\r\n") != std::string::npos)
            continue;
        const std::string_view k = str::trim(raw);
        if (k.empty())
            continue;
        std::string folded = utf8::foldCase(k);
        if (std::find(seenFolded.begin(), seenFolded.end(), folded) != seenFolded.end())
            continue;
        seenFolded.push_back(std::move(folded));
        out.emplace_back(k);
    }
    return out;
}

TranscriptionResult parseTranscription(const net::Response &r) {
    TranscriptionResult out;
    json::Document      doc;
    if (!preflight(r, doc, out.error, Route::Transcription)) {
        // Some servers answer a plain-text transcript whatever
        // response_format says: a 2xx that isn't JSON is that, not a failure.
        const std::string_view body = str::trim(r.body);
        if (r.ok() && !body.empty() && body.front() != '{') {
            out.ok = true;
            out.error.clear();
            out.text = std::string(body);
        }
        return out;
    }
    if (!doc.root().has("text")) {
        out.error = tr("Unexpected response from server (no text)");
        return out;
    }
    out.text = std::string(str::trim(doc.root()["text"].str()));
    out.ok   = true;
    return out;
}

std::string audioMimeForExtension(std::string_view extIn) {
    // What the shared table (mime::fromName) lacks or names as video: an
    // upload for transcription is audio either way.
    static const char *const kMime[][2] = {
        {"mpga", "audio/mpeg"},
        {"mpeg", "audio/mpeg"},
        {"mp4", "audio/mp4"},
        {"oga", "audio/ogg"},
        {"opus", "audio/opus"},
        {"webm", "audio/webm"},
        {"aac", "audio/aac"},
    };
    const std::string ext = str::asciiLower(extIn);
    for (const auto &m : kMime)
        if (ext == m[0])
            return m[1];
    const std::string_view known = mime::fromName(str::concat({"a.", ext}));
    return str::startsWith(known, "audio/") ? std::string(known) : std::string();
}

// ── Custom servers ──────────────────────────────────────────────────────────

std::string normalizeOpenAiBaseUrl(std::string_view raw) {
    std::string s(str::trim(raw));
    if (s.empty())
        return {};
    if (s.find("://") == std::string::npos)
        s.insert(0, "http://");
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    if (str::endsWith(s, "/chat/completions"))
        s.resize(s.size() - 17);
    net::Url url;
    if (!url.parse(s) || url.host.empty() || (url.scheme != "http" && url.scheme != "https"))
        return {};
    if (url.target == "/")
        s += "/v1";
    return s;
}

namespace {

// Dotted-quad IPv4 → its 32 bits; false when `h` is not one.
bool parseIpv4(std::string_view h, uint32_t &out) {
    uint32_t v     = 0;
    int      parts = 0;
    size_t   i     = 0;
    while (parts < 4) {
        uint32_t n      = 0;
        size_t   digits = 0;
        while (i < h.size() && h[i] >= '0' && h[i] <= '9' && digits < 4)
            n = n * 10 + uint32_t(h[i++] - '0'), ++digits;
        if (digits == 0 || n > 255)
            return false;
        v = v << 8 | n;
        if (++parts < 4 && (i >= h.size() || h[i++] != '.'))
            return false;
    }
    out = v;
    return i == h.size();
}

// An IPv6 literal → its eight groups; false when `h` is not one. An
// embedded IPv4 tail ("::ffff:10.0.0.1") fills the last two.
bool parseIpv6(std::string_view h, uint16_t out[8]) {
    uint16_t head[8], tail[8];
    int      nh = 0, nt = 0;
    bool     gap = false;
    if (str::startsWith(h, "::")) {
        gap = true;
        h.remove_prefix(2);
    }
    while (!h.empty()) {
        const size_t           colon = h.find(':');
        const std::string_view g     = h.substr(0, colon);
        uint16_t              *dst   = gap ? tail : head;
        int                   &n     = gap ? nt : nh;
        uint32_t               v4    = 0;
        if (colon == std::string_view::npos && g.find('.') != std::string_view::npos) {
            if (n > 6 || !parseIpv4(g, v4))
                return false;
            dst[n++] = uint16_t(v4 >> 16);
            dst[n++] = uint16_t(v4);
            h        = {};
            break;
        }
        if (g.empty() || g.size() > 4 || n >= 8)
            return false;
        uint16_t v = 0;
        for (char c : g) {
            const int d = str::hexDigit(c);
            if (d < 0)
                return false;
            v = uint16_t(v << 4 | d);
        }
        dst[n++] = v;
        if (colon == std::string_view::npos)
            break;
        h.remove_prefix(colon + 1);
        if (str::startsWith(h, ":")) {
            if (gap)
                return false;
            gap = true;
            h.remove_prefix(1);
        } else if (h.empty()) {
            return false; // a trailing single ':'
        }
    }
    if (gap ? nh + nt > 7 : nh != 8)
        return false;
    for (int i = 0; i < 8; ++i)
        out[i] = 0;
    for (int i = 0; i < nh; ++i)
        out[i] = head[i];
    for (int i = 0; i < nt; ++i)
        out[8 - nt + i] = tail[i];
    return true;
}

bool localIpv4(uint32_t v) {
    return (v >> 24) == 127 || (v >> 16) == 0xA9FE                          // loopback, link-local
           || (v >> 24) == 10 || (v >> 20) == 0xAC1 || (v >> 16) == 0xC0A8; // RFC 1918
}

} // namespace

bool isCleartextRemote(std::string_view urlStr) {
    net::Url url;
    if (!url.parse(urlStr) || url.scheme != "http")
        return false;
    const std::string &host = url.host; // lower case, no brackets
    if (host == "localhost" || str::endsWith(host, ".localhost") || str::endsWith(host, ".local"))
        return false;
    uint32_t v4 = 0;
    if (parseIpv4(host, v4))
        return !localIpv4(v4);
    uint16_t v6[8];
    if (!parseIpv6(host, v6))
        return true; // a DNS name over plain http: assume remote
    bool loopback = v6[7] == 1;
    for (int i = 0; i < 7; ++i)
        loopback &= v6[i] == 0;
    const bool mapped =
        v6[0] == 0 && v6[1] == 0 && v6[2] == 0 && v6[3] == 0 && v6[4] == 0 && v6[5] == 0xFFFF;
    if (mapped)
        return !localIpv4(uint32_t(v6[6]) << 16 | v6[7]);
    return !(loopback || (v6[0] & 0xFFC0) == 0xFE80 || (v6[0] & 0xFE00) == 0xFC00);
}

std::string transportFailure(std::string_view netError) {
    const std::string_view head = netError.substr(0, netError.find(':'));
    if (head == "dns")
        return tr("Server not found");
    if (head == "connect")
        return tr("Couldn't connect to the server");
    if (head == "tls")
        return tr("The secure connection to the server failed");
    if (head == "timeout")
        return tr("The server didn't answer in time");
    if (head == "protocol")
        return tr("The server's answer couldn't be read");
    if (head == "too_many_redirects")
        return tr("Too many redirects");
    if (head == "url")
        return tr("Invalid server URL");
    return tr("network error");
}

} // namespace llm
