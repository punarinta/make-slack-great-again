// Windows audio:
//   playback  MFPlay (Media Foundation's ready-made player). Deprecated in the
//             docs but shipped by every Windows 7+ install, and the only MF
//             surface with play/pause/seek/position in a few calls. Decodes
//             MP3, AAC/M4A, WAV, WMA, FLAC natively. Its events arrive on an
//             MF worker thread; each is posted to the App's thread with a
//             generation stamp so a late event from replaced media can't
//             touch the new one.
//   capture   waveIn through WAVE_MAPPER, which resamples the default
//             microphone to 16 kHz mono s16 for us. Opened with CALLBACK_NULL:
//             a timer on the App's thread polls the rotating buffers for
//             WHDR_DONE, so no waveIn call runs on the driver's callback
//             thread (which the docs forbid).
//   sounds    PlaySound plays named system events (SND_ALIAS, the AppEvents
//             scheme) and files; the list is read from the registry, the
//             same source the Sound control panel uses.
#include "audio/audio_internal.h"

#include "win32/win32.h"

// Media Foundation headers must follow <windows.h>.
#include <mfapi.h>
#include <mferror.h>
#include <mfplay.h>
#include <mmsystem.h>
#include <propidl.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace plat::audio {

namespace {

using win32::toUtf8;
using win32::toWide;

// $PLAT_AUDIO_HELPERS set (tests): no device is ever opened, nothing plays.
bool nullAudio() {
    const char *v = std::getenv("PLAT_AUDIO_HELPERS");
    return v && *v;
}

// ── Playback ────────────────────────────────────────────────────────────────

class WinPlayer final : public Player, public IMFPMediaPlayerCallback {
public:
    explicit WinPlayer(App &app) : _app(app) {}
    ~WinPlayer() override {
        *_alive = false;
        stop();
    }

    void load(const std::string &path) override {
        stop();
        const int gen = ++_generation;
        if (nullAudio()) {
            post(gen, [this] { failed({Error::Unavailable, {}}); });
            return;
        }
        static const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        HRESULT           hr =
            mf ? MFPCreateMediaPlayer(
                     nullptr, FALSE, MFP_OPTION_FREE_THREADED_CALLBACK, this, nullptr, &_player
                 )
               : E_FAIL;
        if (FAILED(hr) || !_player) {
            _player = nullptr;
            post(gen, [this] { failed({Error::Unavailable, {}}); });
            return;
        }
        std::wstring w = toWide(path);
        std::replace(w.begin(), w.end(), L'/', L'\\');
        hr = _player->CreateMediaItemFromURL(w.c_str(), FALSE, DWORD_PTR(gen), nullptr);
        if (FAILED(hr))
            post(gen, [this] { failed({Error::Unsupported, {}}); });
    }

    void play() override {
        if (_player)
            _player->Play();
    }
    void pause() override {
        if (_player)
            _player->Pause();
    }
    void seek(int64_t ms) override {
        if (!_player)
            return;
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt            = VT_I8;
        v.hVal.QuadPart = ms * 10000;
        _player->SetPosition(MFP_POSITIONTYPE_100NS, &v);
        PropVariantClear(&v);
    }
    void stop() override {
        ++_generation;
        if (!_player)
            return;
        IMFPMediaPlayer *p = _player;
        _player            = nullptr;
        p->Shutdown();
        p->Release();
    }

    int64_t positionMs() const override { return query(&IMFPMediaPlayer::GetPosition); }
    int64_t durationMs() const override { return query(&IMFPMediaPlayer::GetDuration); }

    // ── IUnknown: lifetime is the Player's, so Release never deletes ────────
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *ppv = static_cast<IMFPMediaPlayerCallback *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&_ref); }
    STDMETHODIMP_(ULONG) Release() override { return InterlockedDecrement(&_ref); }

    // ── IMFPMediaPlayerCallback (MF worker thread) ──────────────────────────
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER *h) override {
        if (!h)
            return;
        const int gen = _generation.load();
        switch (h->eEventType) {
        case MFP_EVENT_TYPE_MEDIAITEM_CREATED: {
            if (FAILED(h->hrEvent)) {
                post(gen, [this] { failed({Error::Unsupported, {}}); });
                return;
            }
            auto *e = MFP_GET_MEDIAITEM_CREATED_EVENT(h);
            if (e->pMediaItem && h->pMediaPlayer && int(e->dwUserData) == gen)
                h->pMediaPlayer->SetMediaItem(e->pMediaItem);
            break;
        }
        case MFP_EVENT_TYPE_MEDIAITEM_SET:
            post(gen, [this, ok = SUCCEEDED(h->hrEvent)] {
                if (!ok)
                    failed({Error::Unsupported, {}});
                else if (onLoaded)
                    onLoaded();
            });
            break;
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            post(gen, [this] {
                if (onEnded)
                    onEnded();
            });
            break;
        case MFP_EVENT_TYPE_ERROR:
            post(gen, [this] { failed({Error::PlaybackFailed, {}}); });
            break;
        default:
            break;
        }
    }

private:
    void failed(const Failure &f) {
        if (onFailed)
            onFailed(f);
    }

    // Runs fn on the App's thread unless the media changed (or we are gone).
    template <class F>
    void post(int gen, F &&f) {
        _app.post([this, alive = _alive, gen, fn = std::forward<F>(f)] {
            if (*alive && gen == _generation.load())
                fn();
        });
    }

    int64_t query(HRESULT (STDMETHODCALLTYPE IMFPMediaPlayer::*fn)(REFGUID, PROPVARIANT *)) const {
        if (!_player)
            return 0;
        PROPVARIANT v;
        PropVariantInit(&v);
        int64_t ms = 0;
        if (SUCCEEDED((_player->*fn)(MFP_POSITIONTYPE_100NS, &v)) && v.vt == VT_I8)
            ms = v.hVal.QuadPart / 10000;
        PropVariantClear(&v);
        return ms;
    }

    App                  &_app;
    std::shared_ptr<bool> _alive  = std::make_shared<bool>(true); // posted closures check it
    IMFPMediaPlayer      *_player = nullptr;
    LONG                  _ref    = 1;
    std::atomic<int>      _generation{0};
};

// ── Capture ─────────────────────────────────────────────────────────────────

constexpr int kBuffers     = 4;
constexpr int kBufferMs    = 100;
constexpr int kBufferBytes = Recorder::kSampleRate * Recorder::kChannels * 2 * kBufferMs / 1000;
constexpr int kPollMs      = 50;   // also the onLevel cadence
constexpr int kFirstDataMs = 4000; // device open but silent this long = broken

class WinRecorder final : public Recorder {
public:
    explicit WinRecorder(App &app) : _app(app) {}
    ~WinRecorder() override {
        *_alive = false;
        close();
    }

    void start() override {
        if (_recording)
            return;
        if (nullAudio() || waveInGetNumDevs() == 0) {
            post([](WinRecorder &r) { r.fail({Error::NoMicrophone, {}}); });
            return;
        }
        WAVEFORMATEX fmt{};
        fmt.wFormatTag      = WAVE_FORMAT_PCM;
        fmt.nChannels       = kChannels;
        fmt.nSamplesPerSec  = kSampleRate;
        fmt.wBitsPerSample  = 16;
        fmt.nBlockAlign     = WORD(fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        MMRESULT r          = waveInOpen(&_in, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
        if (r != MMSYSERR_NOERROR) {
            _in = nullptr;
            post([f = openError(r)](WinRecorder &rec) { rec.fail(f); });
            return;
        }
        for (int i = 0; i < kBuffers; ++i) {
            WAVEHDR &h       = _hdr[i];
            h                = WAVEHDR{};
            h.lpData         = _data[i];
            h.dwBufferLength = kBufferBytes;
            r                = waveInPrepareHeader(_in, &h, sizeof h);
            if (r == MMSYSERR_NOERROR)
                r = waveInAddBuffer(_in, &h, sizeof h);
            if (r != MMSYSERR_NOERROR)
                break;
        }
        if (r == MMSYSERR_NOERROR)
            r = waveInStart(_in);
        if (r != MMSYSERR_NOERROR) {
            close();
            post([f = openError(r)](WinRecorder &rec) { rec.fail(f); });
            return;
        }
        _pcm.assign(kWavHeader, '\0'); // room for the WAV header
        _next      = 0;
        _gotData   = false;
        _recording = true;
        _startTick = GetTickCount64();
        _poll      = _app.addTimer(kPollMs, true, [this] { poll(); });
    }

    void stop() override {
        if (!_recording)
            return;
        // waveInReset returns every queued buffer marked done (the one being
        // filled with what it has so far); collect them in order.
        waveInReset(_in);
        collect(false);
        close();
        std::string wav;
        wav.swap(_pcm);
        wavInPlace(wav, kSampleRate, kChannels);
        post([wav = std::move(wav)](WinRecorder &r) mutable {
            if (r.onFinished)
                r.onFinished(std::move(wav));
        });
    }

    void cancel() override {
        ++_generation;
        if (_in)
            waveInReset(_in);
        close();
        _pcm.clear();
    }

    bool isRecording() const override { return _recording; }

private:
    void fail(const Failure &f) {
        if (onFailed)
            onFailed(f);
    }

    template <class F>
    void post(F fn) {
        _app.post([this, alive = _alive, gen = _generation, fn = std::move(fn)]() mutable {
            if (*alive && gen == _generation)
                fn(*this);
        });
    }

    void poll() {
        const float peak = collect(true);
        if (!_gotData) {
            if (_pcm.size() <= kWavHeader) {
                if (GetTickCount64() - _startTick > kFirstDataMs) {
                    cancel();
                    fail({Error::NoAudio, {}});
                }
                return;
            }
            _gotData = true;
            if (onStarted)
                onStarted();
            if (!_recording)
                return; // cancelled from onStarted
        }
        if (onLevel)
            onLevel(peak);
    }

    // Appends every finished buffer (in ring order) to _pcm; re-queues them
    // when `requeue`. Returns the peak of what was appended.
    float collect(bool requeue) {
        float peak = 0;
        for (int n = 0; n < kBuffers; ++n) {
            WAVEHDR &h = _hdr[_next];
            if (!(h.dwFlags & WHDR_DONE))
                break;
            if (h.dwBytesRecorded > 0) {
                _pcm.append(h.lpData, h.dwBytesRecorded);
                peak = std::max(peak, peakOf(h.lpData, h.dwBytesRecorded));
            }
            h.dwBytesRecorded = 0;
            if (requeue) {
                h.dwFlags &= ~WHDR_DONE;
                waveInAddBuffer(_in, &h, sizeof h);
            }
            _next = (_next + 1) % kBuffers;
        }
        return peak;
    }

    // Unprepares the buffers and closes the device.
    void close() {
        _recording = false;
        if (_poll)
            _app.cancelTimer(_poll);
        _poll = 0;
        if (!_in)
            return;
        waveInReset(_in);
        for (WAVEHDR &h : _hdr)
            if (h.dwFlags & WHDR_PREPARED)
                waveInUnprepareHeader(_in, &h, sizeof h);
        waveInClose(_in);
        _in = nullptr;
    }

    static Failure openError(MMRESULT r) {
        switch (r) {
        case MMSYSERR_BADDEVICEID:
        case MMSYSERR_NODRIVER:
            return {Error::NoMicrophone, {}};
        case MMSYSERR_ALLOCATED:
            return {Error::MicrophoneBusy, {}};
        default: {
            wchar_t text[MAXERRORLENGTH] = {};
            waveInGetErrorTextW(r, text, MAXERRORLENGTH);
            std::string detail = toUtf8(text);
            while (!detail.empty() && std::strchr(" \t\r\n", detail.back()))
                detail.pop_back();
            return {Error::MicrophoneOpen, detail};
        }
        }
    }

    App                  &_app;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    HWAVEIN               _in    = nullptr;
    WAVEHDR               _hdr[kBuffers]{};
    char                  _data[kBuffers][kBufferBytes]{};
    std::string           _pcm; // kWavHeader bytes of room, then the take
    TimerId               _poll       = 0;
    ULONGLONG             _startTick  = 0;
    uint64_t              _generation = 0;
    int                   _next       = 0;
    bool                  _recording  = false;
    bool                  _gotData    = false;
};

// ── Notification sounds ─────────────────────────────────────────────────────

constexpr wchar_t kApps[]   = L"AppEvents\\Schemes\\Apps\\.Default";
constexpr wchar_t kLabels[] = L"AppEvents\\EventLabels";

// The unnamed (default) value of HKCU\<key>, environment-expanded.
std::wstring defaultValue(const std::wstring &key) {
    wchar_t buf[1024];
    DWORD   size = sizeof buf;
    DWORD   type = 0;
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            key.c_str(),
            nullptr,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
            &type,
            buf,
            &size
        ) != ERROR_SUCCESS)
        return {};
    std::wstring v = buf;
    if (type == REG_EXPAND_SZ || v.find(L'%') != std::wstring::npos) {
        wchar_t     out[1024];
        const DWORD n = ExpandEnvironmentStringsW(v.c_str(), out, 1024);
        if (n > 0 && n <= 1024)
            v = out;
    }
    return v;
}

// The .wav assigned to an event ("" = "(None)").
std::wstring currentWav(const std::wstring &event) {
    return defaultValue(std::wstring(kApps) + L"\\" + event + L"\\.Current");
}

bool playFile(const std::string &path) {
    std::wstring w = toWide(path);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return PlaySoundW(w.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

bool playSystem(const std::string &name) {
    const std::wstring alias = toWide(name);
    if (PlaySoundW(alias.c_str(), nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT))
        return true;
    // Fall back to the resolved file path.
    const std::wstring wav = currentWav(alias);
    return !wav.empty() &&
           PlaySoundW(wav.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

} // namespace

std::unique_ptr<Player> Player::create(App &app) {
    return std::make_unique<WinPlayer>(app);
}

bool canPlayExtension(std::string_view ext) {
    constexpr const char *k[] = {
        "mp3", "m4a", "mp4", "aac", "adts", "wav", "wma", "flac", "aif", "aiff"
    };
    for (const char *e : k)
        if (ext == e)
            return true;
    return false;
}

std::unique_ptr<Recorder> createNativeRecorder(App &app) {
    return std::make_unique<WinRecorder>(app);
}

std::vector<SystemSound> systemSounds() {
    std::vector<SystemSound> out;
    HKEY                     apps = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApps, 0, KEY_READ, &apps) != ERROR_SUCCESS)
        return out;
    std::vector<std::wstring> events;
    wchar_t                   name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(apps, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        events.emplace_back(name, len);
    }
    RegCloseKey(apps);
    std::sort(events.begin(), events.end());
    for (const std::wstring &event : events) {
        if (currentWav(event).empty())
            continue; // the event has "(None)" assigned: nothing to play
        const std::wstring label = defaultValue(std::wstring(kLabels) + L"\\" + event);
        out.push_back({toUtf8(event), toUtf8(label.empty() ? event : label)});
    }
    return out;
}

void playSound(const std::string &name, const std::string &fallbackFile) {
    if (nullAudio())
        return;
    if (!name.empty() && playSystem(name))
        return;
    if (!fallbackFile.empty())
        playFile(fallbackFile);
}

} // namespace plat::audio
