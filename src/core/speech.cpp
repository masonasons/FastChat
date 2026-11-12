#include "core/speech.h"
#include "core/util.h"

#ifdef FASTCHAT_HAVE_PRISM
#include <prism.h>
#endif

Speech& AppSpeech() {
    static Speech speech;
    return speech;
}

// ---------------------------------------------------------------------------
// Prism
// ---------------------------------------------------------------------------

#ifdef FASTCHAT_HAVE_PRISM

namespace {

PrismContext* PrismCtx() {
    static PrismContext* ctx = []() {
        PrismConfig cfg = prism_config_init();
        return prism_init(&cfg);
    }();
    return ctx;
}

class PrismSpeech : public SpeechBackend {
public:
    explicit PrismSpeech(PrismBackend* backend) : m_backend(backend) {
        m_features = prism_backend_get_features(backend);
    }
    ~PrismSpeech() override {
        if (m_backend) prism_backend_free(m_backend);
    }
    bool Speak(const std::string& text, bool interrupt) override {
        if (m_features & PRISM_BACKEND_SUPPORTS_OUTPUT) {
            // Speech and braille together on a screen reader.
            return prism_backend_output(m_backend, text.c_str(), interrupt) == PRISM_OK;
        }
        return prism_backend_speak(m_backend, text.c_str(), interrupt) == PRISM_OK;
    }
    void Stop() override {
        if (m_features & PRISM_BACKEND_SUPPORTS_STOP) prism_backend_stop(m_backend);
    }
    std::vector<std::string> Voices() override {
        std::vector<std::string> voices;
        if (!(m_features & PRISM_BACKEND_SUPPORTS_COUNT_VOICES)) return voices;
        size_t count = 0;
        if (prism_backend_count_voices(m_backend, &count) != PRISM_OK) return voices;
        for (size_t i = 0; i < count; i++) {
            const char* name = nullptr;
            if (prism_backend_get_voice_name(m_backend, i, &name) == PRISM_OK && name) voices.push_back(name);
        }
        return voices;
    }
    bool SetVoice(const std::string& name) override {
        if (!(m_features & PRISM_BACKEND_SUPPORTS_SET_VOICE)) return false;
        auto voices = Voices();
        for (size_t i = 0; i < voices.size(); i++) {
            if (voices[i] == name) return prism_backend_set_voice(m_backend, i) == PRISM_OK;
        }
        return false;
    }
    void SetRate(int rate) override {
        if (m_features & PRISM_BACKEND_SUPPORTS_SET_RATE) prism_backend_set_rate(m_backend, rate / 100.0f);
    }
    void SetVolume(int volume) override {
        if (m_features & PRISM_BACKEND_SUPPORTS_SET_VOLUME) prism_backend_set_volume(m_backend, volume / 100.0f);
    }
    std::string Name() const override {
        const char* n = prism_backend_name(m_backend);
        return n ? n : "Prism";
    }

private:
    PrismBackend* m_backend;
    uint64_t m_features = 0;
};

}  // namespace

std::unique_ptr<SpeechBackend> CreatePrismBackend(const std::string& backendName) {
    PrismContext* ctx = PrismCtx();
    if (!ctx) return nullptr;
    PrismBackend* backend = nullptr;
    if (!backendName.empty() && backendName != "auto") {
        PrismBackendId id = prism_registry_id(ctx, backendName.c_str());
        if (id != PRISM_BACKEND_INVALID) {
            backend = prism_registry_create(ctx, id);
            if (backend && prism_backend_initialize(backend) != PRISM_OK) {
                prism_backend_free(backend);
                backend = nullptr;
            }
        }
    }
    // "auto", or the chosen one is not available right now: the best that works.
    if (!backend) backend = prism_registry_create_best(ctx);
    if (!backend) return nullptr;
    return std::make_unique<PrismSpeech>(backend);
}

std::vector<std::string> ListPrismBackends() {
    std::vector<std::string> names;
    PrismContext* ctx = PrismCtx();
    if (!ctx) return names;
    size_t count = prism_registry_count(ctx);
    for (size_t i = 0; i < count; i++) {
        PrismBackendId id = prism_registry_id_at(ctx, i);
        const char* name = prism_registry_name(ctx, id);
        if (!name) continue;
        // Every backend is registered everywhere; offer the ones made for this
        // system, running or not (a screen reader can be started later).
#ifdef _WIN32
        static const PrismBackendId kOther[] = {PRISM_BACKEND_ORCA, PRISM_BACKEND_SPEECH_DISPATCHER, PRISM_BACKEND_SPIEL,
                                                PRISM_BACKEND_VOICE_OVER, PRISM_BACKEND_AV_SPEECH, PRISM_BACKEND_NO_SPEECH};
#elif defined(__APPLE__)
        static const PrismBackendId kOther[] = {PRISM_BACKEND_ORCA, PRISM_BACKEND_SPEECH_DISPATCHER, PRISM_BACKEND_SPIEL,
                                                PRISM_BACKEND_NVDA, PRISM_BACKEND_JAWS, PRISM_BACKEND_SAPI,
                                                PRISM_BACKEND_ONE_CORE, PRISM_BACKEND_UIA, PRISM_BACKEND_NO_SPEECH};
#else
        static const PrismBackendId kOther[] = {PRISM_BACKEND_NVDA, PRISM_BACKEND_JAWS, PRISM_BACKEND_SAPI,
                                                PRISM_BACKEND_ONE_CORE, PRISM_BACKEND_UIA, PRISM_BACKEND_VOICE_OVER,
                                                PRISM_BACKEND_AV_SPEECH, PRISM_BACKEND_NO_SPEECH};
#endif
        bool other = false;
        for (PrismBackendId o : kOther) other = other || o == id;
        // Windows-only screen readers by name as well (ZDSR, PC-Talker...) off Windows.
#ifndef _WIN32
        static const PrismBackendId kWindowsOnly[] = {PRISM_BACKEND_ZDSR, PRISM_BACKEND_ZOOM_TEXT, PRISM_BACKEND_BOY_PC_READER,
                                                      PRISM_BACKEND_PC_TALKER, PRISM_BACKEND_SENSE_READER};
        for (PrismBackendId o : kWindowsOnly) other = other || o == id;
#endif
        if (!other) names.push_back(name);
    }
    return names;
}

#else

std::unique_ptr<SpeechBackend> CreatePrismBackend(const std::string&) {
    return nullptr;
}

std::vector<std::string> ListPrismBackends() {
    return {};
}

#endif

#ifndef _WIN32
std::unique_ptr<SpeechBackend> CreateSapiBackend() {
    return nullptr;
}
#endif

// ---------------------------------------------------------------------------
// Speech
// ---------------------------------------------------------------------------

void Speech::Configure(const SpeechSettings& settings) {
    m_settings = settings;
    m_announcer.reset();
    m_announcerTried = false;
    m_backend.reset();
    m_currentVoice.clear();
    m_currentRate = -1;
    if (settings.engine == "sapi") m_backend = CreateSapiBackend();
    if (!m_backend) m_backend = CreatePrismBackend(settings.prismBackend);
    if (!m_backend && settings.engine != "sapi") m_backend = CreateSapiBackend();
    if (!m_backend) return;
    if (!settings.voice.empty()) {
        m_backend->SetVoice(settings.voice);
        m_currentVoice = settings.voice;
    }
    m_backend->SetRate(settings.rate);
    m_currentRate = settings.rate;
    m_backend->SetVolume(settings.volume);
}

void Speech::Shutdown() {
    if (m_backend) m_backend->Stop();
    m_backend.reset();
    m_announcer.reset();
}

void Speech::Announce(const std::string& text) {
    if (Trim(text).empty()) return;
    if (m_settings.announceWith != "speech") {
        if (!m_announcerTried) {
            m_announcerTried = true;
            m_announcer = CreatePrismBackend("auto");
        }
        if (m_announcer) {
            m_announcer->Speak(text, true);
            return;
        }
    }
    if (m_backend) m_backend->Speak(text, true);
}

void Speech::Speak(const std::string& text, bool interrupt, const std::optional<std::string>& voice,
                   const std::optional<int>& rate) {
    if (!m_backend || Trim(text).empty()) return;
    std::string wantVoice = voice && !voice->empty() ? *voice : m_settings.voice;
    int wantRate = rate ? *rate : m_settings.rate;
    if (wantVoice != m_currentVoice) {
        if (!wantVoice.empty()) m_backend->SetVoice(wantVoice);
        m_currentVoice = wantVoice;
    }
    if (wantRate != m_currentRate) {
        m_backend->SetRate(wantRate);
        m_currentRate = wantRate;
    }
    m_backend->Speak(text, interrupt);
}

void Speech::Stop() {
    if (m_backend) m_backend->Stop();
    if (m_announcer) m_announcer->Stop();
}

std::string Speech::BackendName() const {
    return m_backend ? m_backend->Name() : std::string("none");
}

bool Speech::PrismAvailable() {
#ifdef FASTCHAT_HAVE_PRISM
    return true;
#else
    return false;
#endif
}

bool Speech::SapiAvailable() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::vector<std::string> Speech::PrismBackendNames() {
    return ListPrismBackends();
}

std::vector<std::string> Speech::ListVoices(const std::string& engine, const std::string& prismBackend) {
    std::unique_ptr<SpeechBackend> b = engine == "sapi" ? CreateSapiBackend() : CreatePrismBackend(prismBackend);
    if (!b) return {};
    return b->Voices();
}
