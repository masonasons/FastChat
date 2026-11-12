#pragma once

// Turning events and responses into sound and speech, following the global
// settings and each conversation's overrides. UI thread only.

#include "core/conversation.h"
#include "core/settings.h"

#include <map>
#include <string>
#include <vector>

struct EventContext {
    std::map<std::string, std::string> values;  // placeholder name -> text
    bool windowActive = true;                   // the conversation's window is in front
    // Sound pack variants of the event's sound, most specific first ("edit",
    // "command-git"...); the pack's file "<event>-<variant>" is played if it has one.
    std::vector<std::string> soundVariants;
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

// The sound pack a conversation uses (its own choice, else the global one);
// "" for none.
std::string ResolveSoundPack(const ConversationSettings* conv);

// The file an event plays: its own file, else the pack's (the most specific
// variant it has), else "" for the built-in tone.
std::string EventSoundFile(EventId id, const EventConfig& config, const std::string& pack,
                           const std::vector<std::string>& variants = {});

// Play just the sound of an event configuration (settings dialog preview).
void PreviewEventSound(EventId id, const EventConfig& config, const std::string& pack);

// Apply the speech and sound settings (after they change).
void ApplyOutputSettings();
