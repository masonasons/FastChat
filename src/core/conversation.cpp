#include "core/conversation.h"
#include "core/util.h"

const char* RoleKey(Role role) {
    switch (role) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool: return "tool";
    }
    return "user";
}

Role RoleFromKey(const std::string& key) {
    if (key == "system") return Role::System;
    if (key == "assistant") return Role::Assistant;
    if (key == "tool") return Role::Tool;
    return Role::User;
}

const char* ConversationTypeName(ConversationType t) {
    switch (t) {
        case ConversationType::Code: return "Code";
        case ConversationType::Computer: return "Computer use";
        default: return "Chat";
    }
}

const char* ConversationTypeKey(ConversationType t) {
    switch (t) {
        case ConversationType::Code: return "code";
        case ConversationType::Computer: return "computer";
        default: return "chat";
    }
}

ConversationType ConversationTypeFromKey(const std::string& key) {
    if (key == "code") return ConversationType::Code;
    if (key == "computer") return ConversationType::Computer;
    return ConversationType::Chat;
}

json UsageToJson(const Usage& u) {
    json j = json::object();
    if (u.inputTokens) j["in"] = u.inputTokens;
    if (u.outputTokens) j["out"] = u.outputTokens;
    if (u.cachedInputTokens) j["cached"] = u.cachedInputTokens;
    if (u.cacheWriteTokens) j["cacheWrite"] = u.cacheWriteTokens;
    if (u.reasoningTokens) j["reasoning"] = u.reasoningTokens;
    if (u.cost >= 0) j["cost"] = u.cost;
    return j;
}

Usage UsageFromJson(const json& j) {
    Usage u;
    if (!j.is_object()) return u;
    u.inputTokens = j.value("in", int64_t{0});
    u.outputTokens = j.value("out", int64_t{0});
    u.cachedInputTokens = j.value("cached", int64_t{0});
    u.cacheWriteTokens = j.value("cacheWrite", int64_t{0});
    u.reasoningTokens = j.value("reasoning", int64_t{0});
    u.cost = j.value("cost", -1.0);
    return u;
}

template <typename T>
static void PutOpt(json& j, const char* key, const std::optional<T>& v) {
    if (v) j[key] = *v;
}

template <typename T>
static void GetOpt(const json& j, const char* key, std::optional<T>& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try {
        out = it->get<T>();
    } catch (...) {
    }
}

json ConversationSettingsToJson(const ConversationSettings& s) {
    json j = json::object();
    if (!s.providerId.empty()) j["providerId"] = s.providerId;
    if (!s.model.empty()) j["model"] = s.model;
    PutOpt(j, "systemPrompt", s.systemPrompt);
    PutOpt(j, "temperature", s.temperature);
    PutOpt(j, "maxTokens", s.maxTokens);
    if (!s.reasoningEffort.empty()) j["reasoningEffort"] = s.reasoningEffort;
    if (!s.commandApproval.empty()) j["commandApproval"] = s.commandApproval;
    PutOpt(j, "soundsEnabled", s.soundsEnabled);
    PutOpt(j, "soundPack", s.soundPack);
    json sp = json::object();
    PutOpt(sp, "enabled", s.speech.enabled);
    PutOpt(sp, "speakResponses", s.speech.speakResponses);
    PutOpt(sp, "speakReasoning", s.speech.speakReasoning);
    PutOpt(sp, "speakToolCalls", s.speech.speakToolCalls);
    PutOpt(sp, "speakInBackground", s.speech.speakInBackground);
    PutOpt(sp, "interrupt", s.speech.interrupt);
    PutOpt(sp, "voice", s.speech.voice);
    PutOpt(sp, "rate", s.speech.rate);
    if (!sp.empty()) j["speech"] = sp;
    json ev = json::object();
    for (int i = 0; i < static_cast<int>(EventId::Count); i++) {
        const EventOverride& o = s.events[i];
        if (o.Empty()) continue;
        json e = json::object();
        PutOpt(e, "sound", o.soundEnabled);
        PutOpt(e, "file", o.soundFile);
        PutOpt(e, "speak", o.speakEnabled);
        PutOpt(e, "text", o.speechText);
        ev[GetEventInfo(static_cast<EventId>(i)).key] = e;
    }
    if (!ev.empty()) j["events"] = ev;
    return j;
}

ConversationSettings ConversationSettingsFromJson(const json& j) {
    ConversationSettings s;
    if (!j.is_object()) return s;
    s.providerId = j.value("providerId", std::string());
    s.model = j.value("model", std::string());
    GetOpt(j, "systemPrompt", s.systemPrompt);
    GetOpt(j, "temperature", s.temperature);
    GetOpt(j, "maxTokens", s.maxTokens);
    s.reasoningEffort = j.value("reasoningEffort", std::string());
    s.commandApproval = j.value("commandApproval", std::string());
    GetOpt(j, "soundsEnabled", s.soundsEnabled);
    GetOpt(j, "soundPack", s.soundPack);
    if (auto it = j.find("speech"); it != j.end() && it->is_object()) {
        const json& sp = *it;
        GetOpt(sp, "enabled", s.speech.enabled);
        GetOpt(sp, "speakResponses", s.speech.speakResponses);
        GetOpt(sp, "speakReasoning", s.speech.speakReasoning);
        GetOpt(sp, "speakToolCalls", s.speech.speakToolCalls);
        GetOpt(sp, "speakInBackground", s.speech.speakInBackground);
        GetOpt(sp, "interrupt", s.speech.interrupt);
        GetOpt(sp, "voice", s.speech.voice);
        GetOpt(sp, "rate", s.speech.rate);
    }
    if (auto it = j.find("events"); it != j.end() && it->is_object()) {
        for (auto& [key, e] : it->items()) {
            EventId id = EventFromKey(key);
            if (id == EventId::Count || !e.is_object()) continue;
            EventOverride& o = s.events[static_cast<int>(id)];
            GetOpt(e, "sound", o.soundEnabled);
            GetOpt(e, "file", o.soundFile);
            GetOpt(e, "speak", o.speakEnabled);
            GetOpt(e, "text", o.speechText);
        }
    }
    return s;
}

NoteKind GetNoteKind(const Message& m) {
    if (!m.providerData.is_object()) return NoteKind::None;
    auto it = m.providerData.find("fastchat");
    if (it == m.providerData.end() || !it->is_object()) return NoteKind::None;
    std::string kind = it->value("kind", std::string());
    if (kind == "error") return NoteKind::Error;
    if (kind == "compaction") return NoteKind::Compaction;
    if (kind == "note") return NoteKind::Note;
    return NoteKind::None;
}

Message MakeNote(NoteKind kind, const std::string& content, const json& extra) {
    Message m;
    m.role = Role::System;
    m.content = content;
    json f = extra.is_object() ? extra : json::object();
    f["kind"] = kind == NoteKind::Error ? "error" : kind == NoteKind::Compaction ? "compaction" : "note";
    m.providerData["fastchat"] = f;
    return m;
}

bool IsAgentCompactionNote(const Message& m) {
    if (GetNoteKind(m) != NoteKind::Note) return false;
    const json& f = m.providerData["fastchat"];
    return f.is_object() && f.value("agentCompaction", false);
}

std::vector<bool> BeforeLatestCompaction(const std::vector<Message>& all) {
    std::vector<bool> before(all.size(), false);
    int at = -1;
    for (int i = static_cast<int>(all.size()) - 1; i >= 0 && at < 0; i--) {
        if (GetNoteKind(all[i]) == NoteKind::Compaction || IsAgentCompactionNote(all[i])) at = i;
    }
    if (at < 0) return before;
    const Message& note = all[at];
    const json& f = note.providerData["fastchat"];
    if (GetNoteKind(note) == NoteKind::Compaction) {
        // FastChat's summary covers the messages up to the one it names; one
        // sent while it was being written stays.
        int64_t through = f.value("through", int64_t{0});
        for (int i = 0; i < at; i++) before[i] = GetNoteKind(all[i]) != NoteKind::None || all[i].id <= through;
        return before;
    }
    // A coding agent: /compact replaces everything before it. Compacting by
    // itself before answering keeps the messages it is answering (the user's,
    // right before the note).
    bool manual = f.value("trigger", std::string()) == "manual" || StartsWith(note.content, "Compacted");
    int keepFrom = at;
    if (!manual) {
        while (keepFrom > 0 && all[keepFrom - 1].role == Role::User) keepFrom--;
    }
    for (int i = 0; i < keepFrom; i++) before[i] = true;
    return before;
}

std::vector<Message> ContextMessages(const std::vector<Message>& all, bool forAgent) {
    // The latest summary, and the last message it covers.
    int summary = -1;
    int64_t through = 0;
    for (int i = static_cast<int>(all.size()) - 1; i >= 0; i--) {
        if (GetNoteKind(all[i]) == NoteKind::Compaction) {
            summary = i;
            through = all[i].providerData["fastchat"].value("through", int64_t{0});
            break;
        }
    }
    std::vector<Message> out;
    if (summary >= 0) {
        Message s = all[summary];
        s.providerData = json();
        out.push_back(s);
    }
    for (int i = 0; i < static_cast<int>(all.size()); i++) {
        const Message& m = all[i];
        if (GetNoteKind(m) != NoteKind::None && !(forAgent && IsAgentCompactionNote(m))) continue;
        if (summary >= 0 && m.id != 0 && m.id <= through) continue;
        out.push_back(m);
    }
    return out;
}
