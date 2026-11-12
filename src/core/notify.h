#pragma once

// Turning events and responses into sound and speech, following the global
// settings and each conversation's overrides. UI thread only.

#include "core/conversation.h"
#include "core/settings.h"

#include <map>
#include <string>

struct EventContext {
    std::map<std::string, std::string> values;  // placeholder name -> text
    bool windowActive = true;                   // the conversation's window is in front
    EventContext& Set(const std::string& key, const std::string& value) {
        values[key] = value;
        return *this;
    }
};

// The effective settings for one conversation (globals with its overrides).
struct EffectiveSpeech {
    bool enabled;
    SpeakResponses speakResponses;
    bool speakReasoning;
    bool speakToolCalls;
    bool speakInBackground;
    bool interrupt;
    std::optional<std::string> voice;
    std::optional<int> rate;
};
EffectiveSpeech ResolveSpeech(const ConversationSettings* conv);
EventConfig ResolveEvent(EventId id, const ConversationSettings* conv);

// Play the event's sound and speak its text, as configured. `conv` may be null.
void NotifyEvent(EventId id, const EventContext& context, const ConversationSettings* conv);

// Speak a response's text (or a piece of it while streaming), cleaned up for
// listening as configured.
void SpeakResponseText(const std::string& text, const ConversationSettings* conv, bool windowActive, bool interrupt);

// Speak something the user asked for (a message read on request): always, if speech is on.
void SpeakOnRequest(const std::string& text, const ConversationSettings* conv);

// Text ready for speech: Markdown removed, code blocks spoken or summarized.
std::string PrepareForSpeech(const std::string& text);

// Fill in {placeholders}.
std::string ExpandTemplate(const std::string& text, const EventContext& context);

// Play just the sound of an event configuration (settings dialog preview).
void PreviewEventSound(EventId id, const EventConfig& config);

// Apply the speech and sound settings (after they change).
void ApplyOutputSettings();
