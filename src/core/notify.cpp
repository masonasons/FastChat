#include "core/notify.h"
#include "core/sound.h"
#include "core/soundpack.h"
#include "core/speech.h"
#include "core/util.h"

EffectiveSpeech ResolveSpeech(const ConversationSettings* conv) {
    const SpeechSettings& g = AppSettings().speech;
    EffectiveSpeech e;
    e.enabled = g.enabled;
    e.speakResponses = static_cast<SpeakResponses>(g.speakResponses);
    e.speakReasoning = g.speakReasoning;
    e.speakToolCalls = g.speakToolCalls;
    e.speakInBackground = g.speakInBackground;
    e.interrupt = g.interrupt;
    if (conv) {
        const SpeechOverrides& o = conv->speech;
        if (o.enabled) e.enabled = *o.enabled;
        if (o.speakResponses) e.speakResponses = static_cast<SpeakResponses>(*o.speakResponses);
        if (o.speakReasoning) e.speakReasoning = *o.speakReasoning;
        if (o.speakToolCalls) e.speakToolCalls = *o.speakToolCalls;
        if (o.speakInBackground) e.speakInBackground = *o.speakInBackground;
        if (o.interrupt) e.interrupt = *o.interrupt;
        e.voice = o.voice;
        e.rate = o.rate;
    }
    return e;
}

EventConfig ResolveEvent(EventId id, const ConversationSettings* conv) {
    EventConfig c = AppSettings().events[static_cast<int>(id)];
    if (conv) {
        const EventOverride& o = conv->events[static_cast<int>(id)];
        if (o.soundEnabled) c.soundEnabled = *o.soundEnabled;
        if (o.soundFile) c.soundFile = *o.soundFile;
        if (o.speakEnabled) c.speakEnabled = *o.speakEnabled;
        if (o.speechText) c.speechText = *o.speechText;
    }
    return c;
}

std::string ExpandTemplate(const std::string& text, const EventContext& context) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '{') {
            size_t close = text.find('}', i);
            if (close != std::string::npos) {
                std::string key = text.substr(i + 1, close - i - 1);
                auto it = context.values.find(key);
                if (it != context.values.end()) {
                    out += it->second;
                    i = close;
                    continue;
                }
                if (key.find(' ') == std::string::npos && key.size() < 20) {
                    i = close;  // an unknown or empty placeholder says nothing
                    continue;
                }
            }
        }
        out.push_back(text[i]);
    }
    // "Approve command: " with nothing after it reads better without the colon.
    out = Trim(out);
    while (!out.empty() && (out.back() == ':' || out.back() == ',' || out.back() == '-')) out = Trim(out.substr(0, out.size() - 1));
    return out;
}

std::string ResolveSoundPack(const ConversationSettings* conv) {
    if (conv && conv->soundPack) return *conv->soundPack;
    return AppSettings().sounds.pack;
}

std::string EventSoundFile(EventId id, const EventConfig& config, const std::string& pack,
                           const std::vector<std::string>& variants) {
    if (!config.soundFile.empty()) return config.soundFile;
    std::string folder = FindSoundPackFolder(pack);
    if (folder.empty()) return std::string();
    std::string key = GetEventInfo(id).key;
    std::vector<std::string> names;
    for (const auto& v : variants) {
        if (!v.empty()) names.push_back(key + "-" + v);
    }
    names.push_back(key);
    return FindPackSound(folder, names);
}

static void PlaySoundFor(EventId id, const EventConfig& c, const std::string& pack, const std::vector<std::string>& variants) {
    std::string file = EventSoundFile(id, c, pack, variants);
    if (!file.empty() && AppSound().PlayFile(file)) return;
    // A file of its own that would not play: the pack's sound before the tone.
    if (!c.soundFile.empty()) {
        EventConfig fromPack = c;
        fromPack.soundFile.clear();
        file = EventSoundFile(id, fromPack, pack, variants);
        if (!file.empty() && AppSound().PlayFile(file)) return;
    }
    const EventInfo& info = GetEventInfo(id);
    AppSound().PlayTone(info.tone1, info.tone2, info.toneMs);
}

void PreviewEventSound(EventId id, const EventConfig& config, const std::string& pack) {
    PlaySoundFor(id, config, pack, {});
}

void NotifyEvent(EventId id, const EventContext& context, const ConversationSettings* conv) {
    EventConfig c = ResolveEvent(id, conv);
    bool sounds = AppSettings().sounds.enabled;
    if (conv && conv->soundsEnabled) sounds = *conv->soundsEnabled;
    if (sounds && c.soundEnabled) PlaySoundFor(id, c, ResolveSoundPack(conv), context.soundVariants);

    EffectiveSpeech s = ResolveSpeech(conv);
    // Tool calls, their results and commands are only spoken when asked for;
    // a command waiting for approval is still announced, without the command.
    if (!s.speakToolCalls && (id == EventId::ToolCall || id == EventId::ToolResult || id == EventId::ToolFailed)) return;
    EventContext spoken = context;
    if (!s.speakToolCalls && id == EventId::ApprovalNeeded) spoken.Set("summary", "").Set("tool", "");
    if (s.enabled && c.speakEnabled && !c.speechText.empty()) {
        std::string text = ExpandTemplate(c.speechText, spoken);
        if (!text.empty()) AppSpeech().Speak(text, s.interrupt, s.voice, s.rate);
    }
}

std::string PrepareForSpeech(const std::string& text) {
    const SpeechSettings& g = AppSettings().speech;
    std::string t = text;
    if (g.skipCodeBlocks) {
        std::string out;
        bool in = false;
        for (const auto& line : SplitLines(t)) {
            if (StartsWith(Trim(line), "```")) {
                if (!in) out += "Code block.\n";
                in = !in;
                continue;
            }
            if (!in) out += line + "\n";
        }
        t = out;
    }
    if (g.stripMarkdown) t = StripMarkdownForSpeech(t);
    return t;
}

void SpeakResponseText(const std::string& text, const ConversationSettings* conv, bool windowActive, bool interrupt) {
    EffectiveSpeech s = ResolveSpeech(conv);
    if (!s.enabled || s.speakResponses == SpeakResponses::Off) return;
    if (!windowActive && !s.speakInBackground) return;
    std::string t = PrepareForSpeech(text);
    if (!t.empty()) AppSpeech().Speak(t, interrupt, s.voice, s.rate);
}

void SpeakOnRequest(const std::string& text, const ConversationSettings* conv) {
    EffectiveSpeech s = ResolveSpeech(conv);
    AppSpeech().Speak(PrepareForSpeech(text), true, s.voice, s.rate);
}

void ApplyOutputSettings() {
    AppSpeech().Configure(AppSettings().speech);
    // Listed ahead, so the settings have them at hand.
    Speech::LoadVoices(AppSettings().speech.engine, AppSettings().speech.prismBackend, nullptr);
    AppSound().SetVolume(AppSettings().sounds.volume);
    ForgetSoundPackFiles();  // pick up packs added or changed meanwhile
}
