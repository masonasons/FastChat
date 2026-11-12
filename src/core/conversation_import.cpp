#include "core/conversation_import.h"
#include "core/platform.h"
#include "core/settings.h"
#include "core/util.h"
#include "core/zip.h"

#include <cstdio>
#include <ctime>
#include <set>

namespace {

std::string Str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string BaseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string Lower(const std::string& s) { return ToLower(s); }

// A MIME type from a file name, for attachments that do not say.
std::string GuessMime(const std::string& name) {
    std::string n = Lower(name);
    size_t dot = n.find_last_of('.');
    std::string ext = dot == std::string::npos ? std::string() : n.substr(dot + 1);
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "pdf") return "application/pdf";
    if (ext == "json") return "application/json";
    if (ext == "txt" || ext == "md" || ext == "csv" || ext == "log" || ext == "py" || ext == "c" || ext == "cpp" ||
        ext == "h" || ext == "js" || ext == "ts" || ext == "html" || ext == "xml" || ext == "yaml" || ext == "yml" ||
        ext == "ini" || ext == "toml") {
        return "text/plain";
    }
    return std::string();
}

bool IsTextMime(const std::string& mime) {
    std::string m = Lower(mime);
    return StartsWith(m, "text/") || m == "application/json" || m == "application/xml" || m == "application/javascript" ||
           m == "application/x-yaml" || m == "application/yaml" || m == "application/toml" || m == "application/x-sh";
}

// A provider of the kind Basilisk used, if FastChat has one turned on.
const ProviderConfig* MatchProvider(const std::string& basiliskId) {
    std::string kind = Lower(basiliskId);
    if (kind == "mistralai") kind = "mistral";
    if (kind == "google") kind = "gemini";
    for (const auto& p : AppSettings().providers) {
        if (p.enabled && p.kind == kind) return &p;
    }
    return nullptr;
}

// A citation as a line: its title and address, or whatever it has.
std::string CitationLine(const json& c) {
    if (!c.is_object()) return OneLine(c.is_string() ? c.get<std::string>() : JsonDump(c), 300);
    std::string title = Str(c, "title"), url = Str(c, "url");
    if (url.empty()) url = Str(c, "source");
    std::string text = Str(c, "cited_text");
    if (text.empty()) text = Str(c, "text");
    std::string line = title;
    if (!url.empty()) line += (line.empty() ? "" : ": ") + url;
    if (!text.empty()) line += (line.empty() ? "" : " - ") + OneLine(text, 300);
    return line.empty() ? OneLine(JsonDump(c), 300) : line;
}

}  // namespace

const char* ImportFileFilter() {
    return "Basilisk conversations (*.bskc)|*.bskc|All files (*.*)|*.*";
}

int64_t ParseIsoTime(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3 &&
        std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 3) {
        return 0;
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    // A zone after the time (Z, +05:00, -0800) makes it UTC plus an offset; none is local time.
    size_t t = text.find('T');
    if (t == std::string::npos) t = text.find(' ');
    std::string zone = t == std::string::npos ? std::string() : text.substr(t + 1);
    size_t at = zone.find_first_of("Z+-");
    if (at == std::string::npos) {
        tm.tm_isdst = -1;
        std::time_t local = std::mktime(&tm);
        return local == -1 ? 0 : static_cast<int64_t>(local);
    }
#ifdef _WIN32
    int64_t utc = _mkgmtime(&tm);
#else
    int64_t utc = timegm(&tm);
#endif
    if (zone[at] != 'Z') {
        int oh = 0, om = 0;
        std::string off = zone.substr(at + 1);
        if (std::sscanf(off.c_str(), "%2d:%2d", &oh, &om) < 1) std::sscanf(off.c_str(), "%2d%2d", &oh, &om);
        int64_t offset = oh * 3600 + om * 60;
        utc += zone[at] == '+' ? -offset : offset;
    }
    return utc;
}

bool ImportConversationFile(const std::string& path, ImportedConversation& out, std::string& error) {
    ZipFile zip;
    std::string zipError;
    std::string name = PathToUtf8(PathFromUtf8(path).stem());
    if (zip.Open(path, zipError)) {
        int main = zip.Find("conversation.json");
        if (main >= 0) {
            std::string text;
            if (!zip.Read(main, text, error)) return false;
            return ImportBasiliskJson(text, &zip, name, out, error);
        }
    }
    error = "FastChat cannot read this file as a conversation. It imports Basilisk conversations (.bskc files).";
    return false;
}

bool ImportBasiliskJson(const std::string& jsonText, const ZipFile* zip, const std::string& name,
                        ImportedConversation& out, std::string& error) {
    out = ImportedConversation();
    out.source = "Basilisk";
    json root = json::parse(jsonText, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "The Basilisk conversation could not be read: its conversation.json is not valid JSON.";
        return false;
    }
    std::vector<std::string> notes;  // what did not come over as it was
    int version = root.value("version", 0);
    if (version > 3) notes.push_back("It was saved by a newer Basilisk (format " + std::to_string(version) + "); some of it may be missing.");

    // System prompts: a list each exchange points into (format 2 and later),
    // or one for the whole conversation (earlier).
    std::vector<std::string> systems;
    auto systemText = [](const json& s) { return s.is_string() ? s.get<std::string>() : s.is_object() ? Str(s, "content") : std::string(); };
    if (auto it = root.find("systems"); it != root.end() && it->is_array()) {
        for (const auto& s : *it) systems.push_back(systemText(s));
    }
    bool legacySystem = false;
    if (systems.empty()) {
        if (auto it = root.find("system"); it != root.end() && !it->is_null()) {
            systems.push_back(systemText(*it));
            legacySystem = true;
        }
    }

    const json& blocks = root.value("messages", json::array());
    if (!blocks.is_array()) {
        error = "The Basilisk conversation could not be read: it has no list of messages.";
        return false;
    }
    // Which system prompt each exchange went with (-1: none).
    std::vector<int> systemOf;
    for (const auto& b : blocks) {
        int index = legacySystem ? 0 : -1;
        if (b.is_object() && b.contains("system_index") && b["system_index"].is_number_integer()) index = b["system_index"].get<int>();
        if (index >= static_cast<int>(systems.size())) index = -1;
        systemOf.push_back(index);
    }
    int finalSystem = systemOf.empty() ? (systems.empty() ? -1 : 0) : systemOf.back();

    Conversation& c = out.conversation;
    c.type = ConversationType::Chat;
    c.title = Trim(Str(root, "title"));
    if (c.title.empty()) c.title = name;
    if (!systems.empty()) c.settings.systemPrompt = finalSystem >= 0 ? systems[finalSystem] : std::string();

    std::set<std::string> unmatched;
    bool usedTopP = false;
    std::string lastProvider, lastModel;
    double lastTemperature = 1;
    int lastMaxTokens = 0;
    int previousSystem = finalSystem;  // a first exchange on another prompt gets a note too
    for (size_t i = 0; i < blocks.size(); i++) {
        const json& b = blocks[i];
        if (!b.is_object()) continue;
        int64_t created = ParseIsoTime(Str(b, "created_at"));
        int64_t updated = ParseIsoTime(Str(b, "updated_at"));
        if (!c.createdAt && created) c.createdAt = created;

        // Where the system prompt changed, a note (notes are never sent).
        if (systemOf[i] != previousSystem && !systems.empty()) {
            std::string text;
            if (systemOf[i] < 0) text = "In Basilisk, the messages from here on had no system prompt.";
            else if (systemOf[i] == finalSystem) text = "In Basilisk, the system prompt changed here to the one this conversation uses now.";
            else text = "In Basilisk, the messages from here on had this system prompt:\n\n" + systems[systemOf[i]];
            Message note = MakeNote(NoteKind::Note, text, {{"import", "basilisk"}});
            note.createdAt = created;
            out.messages.push_back(note);
        }
        previousSystem = systemOf[i];

        const json& model = b.value("model", json::object());
        std::string providerKey = Str(model, "provider_id"), modelId = Str(model, "model_id");
        const ProviderConfig* provider = providerKey.empty() ? nullptr : MatchProvider(providerKey);
        if (!provider && !providerKey.empty()) unmatched.insert(providerKey);
        lastProvider = providerKey;
        lastModel = modelId;
        if (b.contains("temperature") && b["temperature"].is_number()) lastTemperature = b["temperature"].get<double>();
        if (b.contains("max_tokens") && b["max_tokens"].is_number_integer()) lastMaxTokens = b["max_tokens"].get<int>();
        if (b.contains("top_p") && b["top_p"].is_number() && b["top_p"].get<double>() != 1.0) usedTopP = true;

        // The user's message, with its attachments.
        const json& request = b.value("request", json::object());
        Message user;
        user.role = Role::User;
        user.content = Str(request, "content");
        user.createdAt = created;
        if (const json& attachments = request.value("attachments", json()); attachments.is_array()) {
            for (const auto& a : attachments) {
                if (!a.is_object()) continue;
                std::string location, protocol;
                if (a.contains("location") && a["location"].is_string()) {
                    location = a["location"].get<std::string>();
                    size_t p = location.find("://");
                    if (StartsWith(location, "data:")) protocol = "data";
                    else if (p != std::string::npos) protocol = Lower(location.substr(0, p));
                } else if (a.contains("location") && a["location"].is_object()) {
                    location = Str(a["location"], "path");
                    protocol = Lower(Str(a["location"], "protocol"));
                }
                std::string fileName = Str(a, "name");
                if (fileName.empty()) fileName = BaseName(location);
                std::string mime = Str(a, "mime_type");
                if (mime.empty()) mime = GuessMime(fileName);
                std::string description = Str(a, "description");
                std::string label = fileName + (description.empty() ? "" : " (" + description + ")");

                std::string data;
                bool have = false;
                if (protocol == "data") {
                    // data:image/png;base64,....
                    size_t comma = location.find(',');
                    size_t semi = location.find(';');
                    if (comma != std::string::npos) {
                        if (mime.empty() && semi != std::string::npos && semi < comma) mime = location.substr(5, semi - 5);
                        if (StartsWith(Lower(mime), "image/")) {
                            user.images.push_back({mime, location.substr(comma + 1)});
                            continue;
                        }
                        data = Base64Decode(location.substr(comma + 1));
                        have = true;
                    }
                } else if (protocol == "http" || protocol == "https") {
                    user.content += "\n\n[Attached: " + (fileName.empty() || fileName == BaseName(location) ? location : label + " " + location) + "]";
                    continue;
                } else if (zip) {
                    // In the .bskc: attachments/<name>.
                    std::string inZip = ReplaceAll(location, "\\", "/");
                    while (StartsWith(inZip, "/")) inZip.erase(0, 1);
                    int entry = zip->Find(inZip);
                    if (entry < 0) entry = zip->Find("attachments/" + BaseName(inZip));
                    std::string readError;
                    have = entry >= 0 && zip->Read(entry, data, readError);
                }
                if (!have) {
                    user.content += "\n\n[Attached file, not in the Basilisk file: " + label + "]";
                    notes.push_back("An attachment was missing: " + fileName + ".");
                } else if (StartsWith(Lower(mime), "image/")) {
                    user.images.push_back({mime, Base64Encode(data)});
                } else if (IsTextMime(mime) && IsValidUtf8(data) && data.size() <= 1024 * 1024) {
                    user.content += "\n\n[Attached file: " + label + "]\n" + data;
                } else {
                    user.content += "\n\n[Attached file, not imported: " + label + (mime.empty() ? "" : ", " + mime) + "]";
                    notes.push_back("An attachment FastChat cannot send was left out: " + fileName + ".");
                }
            }
        }
        out.messages.push_back(user);
        out.exchanges++;

        // The reply, if there was one.
        const json& response = b.value("response", json());
        if (response.is_object()) {
            Message reply;
            reply.role = Role::Assistant;
            reply.content = Str(response, "content");
            reply.model = modelId;
            if (provider) reply.providerId = provider->id;
            reply.createdAt = updated ? updated : created;
            if (const json& citations = response.value("citations", json()); citations.is_array() && !citations.empty()) {
                reply.content += "\n\nSources:";
                for (const auto& cite : citations) reply.content += "\n- " + CitationLine(cite);
            }
            reply.providerData["import"] = {{"from", "basilisk"}, {"provider", providerKey}};
            out.messages.push_back(reply);
        }
    }

    // The conversation goes on where Basilisk left it: its last model and settings.
    if (const ProviderConfig* p = lastProvider.empty() ? nullptr : MatchProvider(lastProvider)) {
        c.settings.providerId = p->id;
        c.settings.model = lastModel;
    }
    if (!blocks.empty() && lastTemperature != 1.0) c.settings.temperature = lastTemperature;
    if (lastMaxTokens > 0) c.settings.maxTokens = lastMaxTokens;
    for (const auto& p : unmatched) {
        notes.push_back("Basilisk used " + p + (p == lastProvider ? " (" + lastModel + ")" : std::string()) +
                        ", and FastChat has no " + p + " provider turned on, so " +
                        (p == lastProvider ? "this conversation uses your default model." : "those replies keep only their model's name."));
    }
    if (usedTopP) notes.push_back("Basilisk's top P setting has no FastChat equivalent and was left out.");

    std::string first = "Imported from Basilisk: " + std::to_string(out.exchanges) +
                        (out.exchanges == 1 ? " message" : " messages") + " and their replies";
    if (!systems.empty() && finalSystem >= 0) first += ", with its system prompt (in the conversation's settings)";
    first += ".";
    for (const auto& n : notes) first += "\n" + n;
    Message intro = MakeNote(NoteKind::Note, first, {{"import", "basilisk"}});
    intro.createdAt = c.createdAt ? c.createdAt : NowSeconds();
    out.messages.insert(out.messages.begin(), intro);
    if (!c.createdAt) c.createdAt = NowSeconds();
    return true;
}
