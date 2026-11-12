#pragma once

// FastChat's settings, kept as settings.json in the data folder. API keys are
// stored protected (see ProtectSecret) and held in plain text only in memory.

#include "core/events.h"

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

struct ProviderConfig {
    std::string id;           // stable, random
    std::string kind;         // ProviderKind key: "openai", "anthropic", "openrouter", ...
    std::string name;         // what the user called it
    std::string baseUrl;      // empty: the kind's default
    std::string apiKey;
    std::string adminKey;     // optional: OpenAI / Anthropic admin keys, for spend reports
    std::string defaultModel;
    bool enabled = true;
    json extra = json::object();  // kind-specific (organization id, extra headers...)
};

struct GeneralSettings {
    bool enterSends = true;           // Enter sends, Shift+Enter new line (else Ctrl+Enter sends)
    bool confirmDeletes = true;
    std::string defaultProviderId;
    std::string defaultModel;         // empty: the provider's own default
    std::string chatSystemPrompt;
    std::string codeSystemPrompt;     // added to the built-in coding instructions
    bool autoTitle = true;            // name untitled conversations from their first exchange
    std::string titleProviderId;      // empty: the conversation's own
    std::string titleModel;
    std::string reasoningEffort;      // empty: the model's default
    int maxTokens = 0;                // 0: the provider's default
    int maxToolSteps = 50;            // per message, in code conversations
    std::string commandApproval = "ask";  // "ask", "auto", "deny"
    std::string computerApproval = "ask"; // computer use actions: "ask", "auto", "deny"
    int commandTimeoutSec = 120;
    bool focusResponse = false;           // move focus to a new response when it completes
    int autoCompactPercent = 80;          // compact when the context is this full; 0: never
};

struct SpeechSettings {
    bool enabled = true;
    std::string engine = "prism";     // "prism", "system" (SAPI 5 / AVSpeech)
    std::string prismBackend = "auto";// "auto" or a Prism backend name
    std::string voice;                // empty: the engine's default
    int rate = 50;                    // 0-100
    int volume = 100;                 // 0-100
    int speakResponses = static_cast<int>(SpeakResponses::WhenComplete);
    bool speakReasoning = false;
    bool speakToolCalls = false;      // tool calls, their results, and commands awaiting approval
    bool speakInBackground = false;   // speak responses for conversations not in front
    bool interrupt = true;            // new speech cuts off what is speaking
    bool stripMarkdown = true;
    bool skipCodeBlocks = false;      // say "code block" instead of reading the code
    bool ctrlStops = true;            // the Ctrl key silences speech
    std::string announceWith = "prism";  // interface announcements: "prism" (screen reader) or "speech"
};

struct SoundSettings {
    bool enabled = true;
    int volume = 80;                  // 0-100
};

class Settings {
public:
    GeneralSettings general;
    SpeechSettings speech;
    SoundSettings sounds;
    std::vector<ProviderConfig> providers;
    EventConfig events[static_cast<int>(EventId::Count)];
    json windowState = json::object();

    Settings();
    bool Load();
    bool Save() const;

    const ProviderConfig* FindProvider(const std::string& id) const;
    ProviderConfig* FindProvider(const std::string& id);
    // The provider to use when none is chosen: the default, else the first enabled.
    const ProviderConfig* DefaultProvider() const;

    static std::string FilePath();
};

// The one settings object. UI thread reads and writes it; worker threads take
// copies of what they need before starting.
Settings& AppSettings();
