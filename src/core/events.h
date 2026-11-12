#pragma once

// The things that happen in a conversation that can make a sound and/or be
// spoken. Each has a global configuration (Settings > Sounds and speech), and
// each conversation can override any part of it.

#include <string>

enum class EventId {
    MessageSent,
    ResponseStarted,     // the first words (or thinking) arrived
    ReasoningStarted,
    ResponseComplete,
    ResponseError,
    GenerationStopped,   // the user stopped it
    ToolCall,            // code conversations: the model runs a tool
    ToolResult,
    ApprovalNeeded,      // a command waits for the user's OK
    ConversationOpened,
    ConversationCreated,
    TitleGenerated,
    MessageDeleted,
    Compacted,           // the conversation was summarized to save context
    TaskStarted,         // a subagent or background shell started
    TaskFinished,        // ...and ended
    Count
};

struct EventInfo {
    const char* key;          // settings key
    const char* label;        // shown in the settings list
    bool defaultSound;
    bool defaultSpeak;
    const char* defaultText;  // spoken text template
    // The built-in tone: two notes (Hz) and the length of each (ms). 0 = no second note.
    float tone1, tone2;
    int toneMs;
};

const EventInfo& GetEventInfo(EventId id);
EventId EventFromKey(const std::string& key);  // EventId::Count when unknown

// Placeholders a spoken text template can use.
const char* EventPlaceholderHelp();

struct EventConfig {
    bool soundEnabled = true;
    std::string soundFile;   // empty: the built-in tone
    bool speakEnabled = false;
    std::string speechText;
};

// What gets spoken of a response's text.
enum class SpeakResponses { Off = 0, WhenComplete = 1, WhileStreaming = 2 };
