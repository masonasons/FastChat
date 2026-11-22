// Speech Dispatcher directly: its voices (from every output module: espeak-ng,
// RHVoice, Piper...) with their own voice, rate and volume, independent of Orca.
// The counterpart of SAPI 5 on Windows and AVSpeech on the Mac.
//
// libspeechd.so.2 is loaded at run time, as libcurl is, so FastChat still starts
// where Speech Dispatcher is not installed (it then offers no system voices).

#include "core/speech.h"

#include <libspeechd.h>

#include <dlfcn.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <type_traits>

namespace {

struct Speechd {
    decltype(&::spd_open2) open2 = nullptr;
    decltype(&::spd_close) close = nullptr;
    decltype(&::spd_say) say = nullptr;
    decltype(&::spd_cancel) cancel = nullptr;
    decltype(&::spd_set_voice_rate) setRate = nullptr;
    decltype(&::spd_set_volume) setVolume = nullptr;
    decltype(&::spd_list_modules) listModules = nullptr;
    decltype(&::free_spd_modules) freeModules = nullptr;
    decltype(&::spd_get_output_module) getModule = nullptr;
    decltype(&::spd_set_output_module) setModule = nullptr;
    decltype(&::spd_list_synthesis_voices) listVoices = nullptr;
    decltype(&::free_spd_voices) freeVoices = nullptr;
    decltype(&::spd_set_synthesis_voice) setVoice = nullptr;
};

// Opened once and kept for the life of the process.
const Speechd* Api() {
    static const Speechd* api = []() -> const Speechd* {
        void* lib = dlopen("libspeechd.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return nullptr;
        static Speechd sd;
        bool ok = true;
        auto bind = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            ok = ok && fn;
        };
        bind(sd.open2, "spd_open2");
        bind(sd.close, "spd_close");
        bind(sd.say, "spd_say");
        bind(sd.cancel, "spd_cancel");
        bind(sd.setRate, "spd_set_voice_rate");
        bind(sd.setVolume, "spd_set_volume");
        bind(sd.listModules, "spd_list_modules");
        bind(sd.freeModules, "free_spd_modules");
        bind(sd.getModule, "spd_get_output_module");
        bind(sd.setModule, "spd_set_output_module");
        bind(sd.listVoices, "spd_list_synthesis_voices");
        bind(sd.freeVoices, "free_spd_voices");
        bind(sd.setVoice, "spd_set_synthesis_voice");
        return ok ? &sd : nullptr;
    }();
    return api;
}

std::string Str(const char* s) {
    return s ? std::string(s) : std::string();
}

// "en" from "en_US.UTF-8" or "en-US".
std::string PrimaryLanguage(std::string tag) {
    size_t end = tag.find_first_of("-_.@");
    if (end != std::string::npos) tag.resize(end);
    std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return tag;
}

std::string UserLanguage() {
    for (const char* var : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* v = std::getenv(var);
        if (v && *v) {
            std::string lang = PrimaryLanguage(std::string(v).substr(0, std::string(v).find(':')));
            if (!lang.empty() && lang != "c" && lang != "posix") return lang;
        }
    }
    return "en";
}

// "English (America)+Max (espeak-ng, en-US)": names repeat across modules, so
// the module is part of what is shown and saved. Module names and language
// codes have no ", " or " (", so the label splits again from the end.
const char* const kSep = " (";

std::string VoiceLabel(const std::string& name, const std::string& module, const std::string& language) {
    return name + kSep + module + (language.empty() ? "" : ", " + language) + ")";
}

bool ParseLabel(const std::string& label, std::string& name, std::string& module) {
    if (label.size() < 4 || label.back() != ')') return false;
    size_t open = label.rfind(kSep);
    if (open == std::string::npos || open == 0) return false;
    std::string inside = label.substr(open + 2, label.size() - open - 3);
    module = inside.substr(0, inside.find(", "));
    name = label.substr(0, open);
    return !module.empty();
}

class SpeechdSpeech : public SpeechBackend {
public:
    explicit SpeechdSpeech(const Speechd* sd) : m_sd(sd) {}
    ~SpeechdSpeech() override {
        if (m_conn) {
            m_sd->cancel(m_conn);
            m_sd->close(m_conn);
        }
    }

    bool Open() {
        char* error = nullptr;
        // Started for this user if it is not running (autospawn), as Orca does.
        m_conn = m_sd->open2("FastChat", "main", nullptr, SPD_MODE_SINGLE, nullptr, 1, &error);
        std::free(error);
        return m_conn != nullptr;
    }

    bool Speak(const std::string& text, bool interrupt) override {
        if (interrupt) m_sd->cancel(m_conn);
        // Message priority: queued after what is being said rather than
        // replacing it (sentence by sentence), and not cut off by other clients'
        // text (Orca's typing echo).
        return m_sd->say(m_conn, SPD_MESSAGE, text.c_str()) >= 0;
    }
    void Stop() override { m_sd->cancel(m_conn); }

    // Every module's voices, the user's language first, then the rest, each part
    // by name. espeak-ng lists each voice with each of its ~140 variants
    // ("English (America)+Max"): those are kept only in the user's language, or
    // there would be some fifteen thousand.
    std::vector<std::string> Voices() override {
        std::string own = UserLanguage();
        std::vector<std::string> mine, others;
        char* saved = m_sd->getModule(m_conn);
        char** modules = m_sd->listModules(m_conn);
        for (char** m = modules; m && *m; ++m) {
            std::string module = *m;
            if (module == "dummy" || m_sd->setModule(m_conn, *m) != 0) continue;
            std::vector<std::string> names;
            SPDVoice** voices = m_sd->listVoices(m_conn);
            for (SPDVoice** v = voices; v && *v; ++v) {
                std::string name = Str((*v)->name), language = Str((*v)->language), variant = Str((*v)->variant);
                if (name.empty()) continue;
                names.push_back(name);
                bool mineLanguage = PrimaryLanguage(language) == own;
                bool combination = !variant.empty() && variant != "none" && name.find('+') != std::string::npos;
                if (combination && !mineLanguage) continue;
                (mineLanguage ? mine : others).push_back(VoiceLabel(name, module, language));
            }
            if (voices) m_sd->freeVoices(voices);
            Remember(module, std::move(names));
        }
        if (modules) m_sd->freeModules(modules);
        if (saved) {
            m_sd->setModule(m_conn, saved);
            std::free(saved);
        }
        for (auto* list : {&mine, &others}) {
            std::sort(list->begin(), list->end());
            list->erase(std::unique(list->begin(), list->end()), list->end());
        }
        mine.insert(mine.end(), others.begin(), others.end());
        return mine;
    }

    bool SetVoice(const std::string& label) override {
        std::string name, module;
        if (!ParseLabel(label, name, module)) return false;
        if (!HasVoice(module, name)) return false;
        if (m_sd->setModule(m_conn, module.c_str()) != 0) return false;
        return m_sd->setVoice(m_conn, name.c_str()) == 0;
    }

    // 0-100, with 50 the voice's normal speed; Speech Dispatcher's -100 to 100.
    void SetRate(int rate) override { m_sd->setRate(m_conn, (std::clamp(rate, 0, 100) - 50) * 2); }
    // Speech Dispatcher's volume is -100 (silent) to 100 (full).
    void SetVolume(int volume) override { m_sd->setVolume(m_conn, std::clamp(volume, 0, 100) * 2 - 100); }
    std::string Name() const override { return "Speech Dispatcher"; }

private:
    // Whether a module has the voice: Speech Dispatcher accepts any name and
    // falls back to the module's default. Asked of the module once per run
    // (espeak-ng takes most of a second), or known from Voices().
    bool HasVoice(const std::string& module, const std::string& name) {
        ModuleVoices& known = Known();
        {
            std::lock_guard<std::mutex> lock(known.mutex);
            auto it = known.names.find(module);
            if (it != known.names.end()) return std::binary_search(it->second.begin(), it->second.end(), name);
        }
        char* saved = m_sd->getModule(m_conn);
        if (m_sd->setModule(m_conn, module.c_str()) != 0) {
            std::free(saved);
            return false;
        }
        std::vector<std::string> names;
        SPDVoice** voices = m_sd->listVoices(m_conn);
        for (SPDVoice** v = voices; v && *v; ++v) {
            if ((*v)->name) names.push_back((*v)->name);
        }
        if (voices) m_sd->freeVoices(voices);
        if (saved) {
            m_sd->setModule(m_conn, saved);
            std::free(saved);
        }
        bool has = std::find(names.begin(), names.end(), name) != names.end();
        Remember(module, std::move(names));
        return has;
    }

    struct ModuleVoices {
        std::mutex mutex;
        std::map<std::string, std::vector<std::string>> names;  // sorted
    };
    static ModuleVoices& Known() {
        static ModuleVoices known;
        return known;
    }
    static void Remember(const std::string& module, std::vector<std::string> names) {
        std::sort(names.begin(), names.end());
        ModuleVoices& known = Known();
        std::lock_guard<std::mutex> lock(known.mutex);
        known.names[module] = std::move(names);
    }

    const Speechd* m_sd;
    SPDConnection* m_conn = nullptr;
};

}  // namespace

bool SpeechDispatcherInstalled() {
    return Api() != nullptr;
}

std::unique_ptr<SpeechBackend> CreateSystemVoiceBackend() {
    const Speechd* sd = Api();
    if (!sd) return nullptr;
    auto s = std::make_unique<SpeechdSpeech>(sd);
    if (!s->Open()) return nullptr;
    return s;
}
