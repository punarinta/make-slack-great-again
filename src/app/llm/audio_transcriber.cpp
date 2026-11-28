#include "app/llm/audio_transcriber.h"

#include "app/llm/service.h"

namespace llm {

const std::string *AudioTranscriber::cached(const std::string &fileId) const {
    const auto it = _done.find(fileId);
    return it == _done.end() ? nullptr : &it->second;
}

bool AudioTranscriber::inFlight(const std::string &fileId) const {
    return _pending.count(fileId) != 0;
}

void AudioTranscriber::transcribe(const std::string &fileId, TranscriptionInput in, Done done) {
    if (const std::string *hit = cached(fileId)) {
        if (done)
            done({true, *hit, {}});
        return;
    }
    const bool running = inFlight(fileId);
    _pending[fileId].push_back(std::move(done));
    if (running)
        return;
    _observers.notify(fileId);
    // Settles every listener registered while the request ran, then forgets
    // the key.
    _service.transcribe(std::move(in), [this, fileId](TranscriptionResult r) {
        auto it = _pending.find(fileId);
        if (it == _pending.end())
            return;
        const std::vector<Done> listeners = std::move(it->second);
        _pending.erase(it);
        if (r.ok)
            _done[fileId] = r.text;
        _observers.notify(fileId);
        for (const Done &l : listeners)
            if (l)
                l(r);
    });
}

} // namespace llm
