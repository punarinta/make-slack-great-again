// Speech-to-text of audio attachments: the
// "Transcribe with AI" button on the inline audio player. Sits between the
// message lists (the channel and the thread panel show the same file) and
// Service::transcribe: one request per file however many cards ask, and the
// result is kept for the session so a second look costs nothing.
//
// It never touches Slack: the caller has the bytes (the same download the
// player uses) and hands them over. UI thread only; callbacks run later on
// it (a cached transcript answers at once, inside the call).
#pragma once

#include "app/llm/wire.h"
#include "base/observers.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace llm {

class Service;

class AudioTranscriber {
public:
    explicit AudioTranscriber(Service &service) : _service(service) {}
    AudioTranscriber(const AudioTranscriber &)            = delete;
    AudioTranscriber &operator=(const AudioTranscriber &) = delete;

    // The finished transcript of `fileId`, if this session produced one.
    const std::string *cached(const std::string &fileId) const;
    // A request for `fileId` is on the wire (the card paints its button busy).
    bool               inFlight(const std::string &fileId) const;

    // Transcribes `in.audio` as `fileId`. A cached transcript answers at once
    // (synchronously); an in-flight request just gains another listener.
    // Listeners run as long as the Service lives: one that may be gone by
    // then guards itself (a weak alive flag).
    using Done = std::function<void(const TranscriptionResult &)>;
    void transcribe(const std::string &fileId, TranscriptionInput in, Done done);

    // `fileId` started or finished (either way): repaint its cards.
    using ObserverId = base::Observers::Id;
    ObserverId observe(std::function<void(const std::string &fileId)> fn) {
        return _observers.add(std::move(fn));
    }
    void unobserve(ObserverId id) { _observers.remove(id); }

private:
    Service                                           &_service;
    std::unordered_map<std::string, std::string>       _done;
    std::unordered_map<std::string, std::vector<Done>> _pending; // keys = in flight
    base::Observers                                    _observers;
};

} // namespace llm
