#include "core/settings.h"
#include "core/platform.h"
#include "core/util.h"

#include <filesystem>

namespace fs = std::filesystem;

Settings& AppSettings() {
    static Settings settings;
    return settings;
}

Settings::Settings() {
    for (int i = 0; i < static_cast<int>(EventId::Count); i++) {
        const EventInfo& info = GetEventInfo(static_cast<EventId>(i));
        events[i].soundEnabled = info.defaultSound;
        events[i].speakEnabled = info.defaultSpeak;
        events[i].speechText = info.defaultText;
    }
}

std::string Settings::FilePath() {
    return JoinPath(GetDataDir(), "settings.json");
}

template <typename T>
static void Get(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try {
        out = it->get<T>();
    } catch (...) {
    }
}

bool Settings::Load() {
    std::string text;
    if (!ReadFileText(FilePath(), text)) return false;
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return false;

    if (auto it = root.find("general"); it != root.end()) {
        const json& g = *it;
        Get(g, "enterSends", general.enterSends);
        Get(g, "confirmDeletes", general.confirmDeletes);
        Get(g, "defaultProviderId", general.defaultProviderId);
        Get(g, "defaultModel", general.defaultModel);
        Get(g, "chatSystemPrompt", general.chatSystemPrompt);
        Get(g, "codeSystemPrompt", general.codeSystemPrompt);
        Get(g, "autoTitle", general.autoTitle);
        Get(g, "titleProviderId", general.titleProviderId);
        Get(g, "titleModel", general.titleModel);
        Get(g, "reasoningEffort", general.reasoningEffort);
        Get(g, "maxTokens", general.maxTokens);
        Get(g, "maxToolSteps", general.maxToolSteps);
        Get(g, "commandApproval", general.commandApproval);
        Get(g, "computerApproval", general.computerApproval);
        Get(g, "commandTimeoutSec", general.commandTimeoutSec);
        Get(g, "focusResponse", general.focusResponse);
        Get(g, "autoCompactPercent", general.autoCompactPercent);
    }
    if (auto it = root.find("speech"); it != root.end()) {
        const json& s = *it;
        Get(s, "enabled", speech.enabled);
        Get(s, "engine", speech.engine);
        if (speech.engine == "sapi") speech.engine = "system";  // its name before the Mac had one
        Get(s, "prismBackend", speech.prismBackend);
        Get(s, "voice", speech.voice);
        Get(s, "rate", speech.rate);
        Get(s, "volume", speech.volume);
        Get(s, "speakResponses", speech.speakResponses);
        Get(s, "speakReasoning", speech.speakReasoning);
        Get(s, "speakToolCalls", speech.speakToolCalls);
        Get(s, "speakInBackground", speech.speakInBackground);
        Get(s, "interrupt", speech.interrupt);
        Get(s, "stripMarkdown", speech.stripMarkdown);
        Get(s, "skipCodeBlocks", speech.skipCodeBlocks);
        Get(s, "ctrlStops", speech.ctrlStops);
        Get(s, "announceWith", speech.announceWith);
    }
    if (auto it = root.find("sounds"); it != root.end()) {
        Get(*it, "enabled", sounds.enabled);
        Get(*it, "volume", sounds.volume);
        Get(*it, "pack", sounds.pack);
    }
    if (auto it = root.find("updates"); it != root.end()) {
        Get(*it, "checkAutomatically", updates.checkAutomatically);
        Get(*it, "checkEveryHours", updates.checkEveryHours);
        Get(*it, "installWithoutAsking", updates.installWithoutAsking);
    }
    if (auto it = root.find("events"); it != root.end() && it->is_object()) {
        for (auto& [key, value] : it->items()) {
            EventId id = EventFromKey(key);
            if (id == EventId::Count || !value.is_object()) continue;
            EventConfig& e = events[static_cast<int>(id)];
            Get(value, "sound", e.soundEnabled);
            Get(value, "file", e.soundFile);
            Get(value, "speak", e.speakEnabled);
            Get(value, "text", e.speechText);
        }
    }
    providers.clear();
    if (auto it = root.find("providers"); it != root.end() && it->is_array()) {
        for (const auto& p : *it) {
            ProviderConfig c;
            Get(p, "id", c.id);
            Get(p, "kind", c.kind);
            Get(p, "name", c.name);
            Get(p, "baseUrl", c.baseUrl);
            Get(p, "defaultModel", c.defaultModel);
            Get(p, "enabled", c.enabled);
            std::string key, admin;
            Get(p, "apiKey", key);
            Get(p, "adminKey", admin);
            c.apiKey = UnprotectSecret(key);
            c.adminKey = UnprotectSecret(admin);
            if (auto e = p.find("extra"); e != p.end() && e->is_object()) c.extra = *e;
            if (c.id.empty()) c.id = NewId();
            if (!c.kind.empty()) providers.push_back(std::move(c));
        }
    }
    if (auto it = root.find("windowState"); it != root.end() && it->is_object()) windowState = *it;
    return true;
}

bool Settings::Save() const {
    json root;
    root["version"] = 1;
    root["general"] = {
        {"enterSends", general.enterSends},
        {"confirmDeletes", general.confirmDeletes},
        {"defaultProviderId", general.defaultProviderId},
        {"defaultModel", general.defaultModel},
        {"chatSystemPrompt", general.chatSystemPrompt},
        {"codeSystemPrompt", general.codeSystemPrompt},
        {"autoTitle", general.autoTitle},
        {"titleProviderId", general.titleProviderId},
        {"titleModel", general.titleModel},
        {"reasoningEffort", general.reasoningEffort},
        {"maxTokens", general.maxTokens},
        {"maxToolSteps", general.maxToolSteps},
        {"commandApproval", general.commandApproval},
        {"computerApproval", general.computerApproval},
        {"commandTimeoutSec", general.commandTimeoutSec},
        {"focusResponse", general.focusResponse},
        {"autoCompactPercent", general.autoCompactPercent},
    };
    root["speech"] = {
        {"enabled", speech.enabled},
        {"engine", speech.engine},
        {"prismBackend", speech.prismBackend},
        {"voice", speech.voice},
        {"rate", speech.rate},
        {"volume", speech.volume},
        {"speakResponses", speech.speakResponses},
        {"speakReasoning", speech.speakReasoning},
        {"speakToolCalls", speech.speakToolCalls},
        {"speakInBackground", speech.speakInBackground},
        {"interrupt", speech.interrupt},
        {"stripMarkdown", speech.stripMarkdown},
        {"skipCodeBlocks", speech.skipCodeBlocks},
        {"ctrlStops", speech.ctrlStops},
        {"announceWith", speech.announceWith},
    };
    root["sounds"] = {{"enabled", sounds.enabled}, {"volume", sounds.volume}, {"pack", sounds.pack}};
    root["updates"] = {{"checkAutomatically", updates.checkAutomatically},
                       {"checkEveryHours", updates.checkEveryHours},
                       {"installWithoutAsking", updates.installWithoutAsking}};
    json ev = json::object();
    for (int i = 0; i < static_cast<int>(EventId::Count); i++) {
        const EventConfig& e = events[i];
        ev[GetEventInfo(static_cast<EventId>(i)).key] = {
            {"sound", e.soundEnabled}, {"file", e.soundFile}, {"speak", e.speakEnabled}, {"text", e.speechText}};
    }
    root["events"] = ev;
    json list = json::array();
    for (const auto& p : providers) {
        list.push_back({
            {"id", p.id},
            {"kind", p.kind},
            {"name", p.name},
            {"baseUrl", p.baseUrl},
            {"apiKey", ProtectSecret(p.apiKey)},
            {"adminKey", ProtectSecret(p.adminKey)},
            {"defaultModel", p.defaultModel},
            {"enabled", p.enabled},
            {"extra", p.extra},
        });
    }
    root["providers"] = list;
    root["windowState"] = windowState;

    // Write beside, then replace, so a crash mid-write cannot lose the settings.
    std::string path = FilePath();
    std::string temp = path + ".tmp";
    if (!WriteFileText(temp, root.dump(2, ' ', false, json::error_handler_t::replace))) return false;
    std::error_code ec;
    fs::rename(PathFromUtf8(temp), PathFromUtf8(path), ec);
    if (ec) {
        fs::remove(PathFromUtf8(path), ec);
        fs::rename(PathFromUtf8(temp), PathFromUtf8(path), ec);
    }
    return !ec;
}

const ProviderConfig* Settings::FindProvider(const std::string& id) const {
    for (const auto& p : providers) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

ProviderConfig* Settings::FindProvider(const std::string& id) {
    for (auto& p : providers) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const ProviderConfig* Settings::DefaultProvider() const {
    if (const ProviderConfig* p = FindProvider(general.defaultProviderId); p && p->enabled) return p;
    for (const auto& p : providers) {
        if (p.enabled) return &p;
    }
    return nullptr;
}
