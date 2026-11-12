#pragma once

// Speech output: through Prism (whatever screen reader is running, or a speech
// engine it supports) or directly through the system's own voices: SAPI 5 on
// Windows, AVSpeech on the Mac. UI thread only.

#include "core/settings.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class SpeechBackend {
public:
    virtual ~SpeechBackend() = default;
    virtual bool Speak(const std::string& text, bool interrupt) = 0;
    virtual void Stop() = 0;
    virtual std::vector<std::string> Voices() = 0;
    virtual bool SetVoice(const std::string& name) = 0;
    virtual void SetRate(int rate0to100) = 0;
    virtual void SetVolume(int volume0to100) = 0;
    virtual std::string Name() const = 0;
};

class Speech {
public:
    // (Re)create the backend from the settings.
    void Configure(const SpeechSettings& settings);
    void Shutdown();

    // Speak with the configured voice, or a conversation's own voice and rate.
    void Speak(const std::string& text, bool interrupt, const std::optional<std::string>& voice = std::nullopt,
               const std::optional<int>& rate = std::nullopt);
    void Stop();
    // Interface announcements (buffer names...): through the screen reader
    // (Prism, its best output) unless the settings send them to the speech
    // engine, whichever engine reads the replies.
    void Announce(const std::string& text);
    std::string BackendName() const;

    // For the settings dialog.
    static bool PrismAvailable();
    static bool SystemVoicesAvailable();
    // "SAPI 5 voices", "macOS voices (AVSpeech)".
    static std::string SystemVoicesName();
    static std::vector<std::string> PrismBackendNames();
    // Voices of an engine ("prism" with a backend name, or "system").
    static std::vector<std::string> ListVoices(const std::string& engine, const std::string& prismBackend);

private:
    std::unique_ptr<SpeechBackend> m_backend;
    std::unique_ptr<SpeechBackend> m_announcer;  // Prism, for announcements
    bool m_announcerTried = false;
    SpeechSettings m_settings;
    std::string m_currentVoice;
    int m_currentRate = -1;
};

Speech& AppSpeech();

// Implementations
std::unique_ptr<SpeechBackend> CreatePrismBackend(const std::string& backendName);
// SAPI 5 on Windows, AVSpeech on the Mac; null elsewhere.
std::unique_ptr<SpeechBackend> CreateSystemVoiceBackend();
std::vector<std::string> ListPrismBackends();
