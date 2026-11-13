// SAPI 5 directly: the installed Windows voices, with their own rate and volume,
// independent of any screen reader. The 64-bit voices load into FastChat; the
// 32-bit ones (older voices installed only for 32-bit programs) cannot, so they
// speak through FastChatSapi32.exe, a 32-bit helper beside FastChat. They are
// listed as "Name (32-bit)" when no 64-bit voice has their name.

#include "core/speech.h"
#include "core/platform.h"
#include "core/util.h"

#include <windows.h>
#include <sapi.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>

namespace {

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

const char* const k32BitSuffix = " (32-bit)";

// The helper's line protocol (see src/sapi32/main.cpp): tabs between fields,
// with \\, \n and \t escaped in text.
std::string Escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c != '\r') out += c;
    }
    return out;
}

std::string Unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

// FastChatSapi32.exe, started when first needed and again if it has ended (an
// old voice that crashes takes only the helper down).
class Sapi32Helper {
public:
    static std::string Path() { return JoinPath(GetExecutableDir(), "FastChatSapi32.exe"); }
    static bool Available() {
#ifdef _WIN64
        std::error_code ec;
        return std::filesystem::exists(PathFromUtf8(Path()), ec);
#else
        return false;  // a 32-bit FastChat loads 32-bit voices itself
#endif
    }

    // Sends a command that has no reply. False if the helper is not running.
    bool Send(const std::string& line) {
        if (!Ensure()) return false;
        if (m_child->Write(line + "\n")) return true;
        m_child.reset();  // ended: next time it starts again
        return false;
    }

    // Sends a command and collects its reply lines, up to and including the one
    // that starts with one of `last` (or the first line when `last` is empty).
    bool Ask(const std::string& line, const std::vector<std::string>& last, std::vector<std::string>& replies,
             unsigned timeoutMs = 15000) {
        replies.clear();
        if (!Send(line)) return false;
        for (;;) {
            std::string reply;
            ChildProcess::Read r = m_child->ReadLine(reply, timeoutMs);
            if (r != ChildProcess::Read::Line) {
                m_child.reset();
                return false;
            }
            if (!reply.empty() && reply.back() == '\r') reply.pop_back();
            replies.push_back(reply);
            if (last.empty()) return true;
            for (const auto& l : last) {
                if (StartsWith(reply, l)) return true;
            }
        }
    }

    bool Running() const { return m_child != nullptr; }

private:
    bool Ensure() {
        if (m_child) return true;
        if (!Available()) return false;
        auto child = std::make_unique<ChildProcess>();
        std::string error;
        if (!child->Start({Path()}, GetExecutableDir(), {}, error)) {
            AppendLog("FastChatSapi32 did not start: " + error);
            return false;
        }
        m_child = std::move(child);
        return true;
    }

    std::unique_ptr<ChildProcess> m_child;
};

std::vector<std::string> HelperVoices(Sapi32Helper& helper) {
    std::vector<std::string> names, replies;
    if (!helper.Ask("voices", {"end"}, replies)) return names;
    for (const auto& r : replies) {
        if (StartsWith(r, "voice\t")) names.push_back(Unescape(r.substr(6)));
    }
    return names;
}

class SapiSpeech : public SpeechBackend {
public:
    bool Init() {
        return SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                          reinterpret_cast<void**>(&m_voice)));
    }
    ~SapiSpeech() override {
        if (m_voice) {
            m_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
            m_voice->Release();
        }
    }
    bool Speak(const std::string& text, bool interrupt) override {
        if (m_use32) {
            Reapply32();
            return m_helper.Send(std::string("speak\t") + (interrupt ? "1" : "0") + "\t" + Escape(text));
        }
        std::wstring w = Utf8ToWide(text);
        DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML;
        if (interrupt) flags |= SPF_PURGEBEFORESPEAK;
        return SUCCEEDED(m_voice->Speak(w.c_str(), flags, nullptr));
    }
    void Stop() override {
        if (m_use32) m_helper.Send("stop");
        else m_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
    }

    template <typename Fn>
    void ForEachToken(Fn fn) {
        ComPtr<ISpObjectTokenCategory> category;
        if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory,
                                    reinterpret_cast<void**>(&category)))) {
            return;
        }
        if (FAILED(category->SetId(SPCAT_VOICES, FALSE))) return;
        ComPtr<IEnumSpObjectTokens> tokens;
        if (FAILED(category->EnumTokens(nullptr, nullptr, &tokens))) return;
        ULONG count = 0;
        tokens->GetCount(&count);
        for (ULONG i = 0; i < count; i++) {
            ISpObjectToken* token = nullptr;
            if (FAILED(tokens->Item(i, &token)) || !token) continue;
            wchar_t* name = nullptr;
            std::string n;
            if (SUCCEEDED(token->GetStringValue(nullptr, &name)) && name) {
                n = WideToUtf8(name);
                CoTaskMemFree(name);
            }
            bool keep = fn(token, n);
            token->Release();
            if (!keep) break;
        }
    }

    std::vector<std::string> Voices() override {
        std::vector<std::string> names;
        std::set<std::string> have;
        ForEachToken([&](ISpObjectToken*, const std::string& name) {
            if (!name.empty() && have.insert(name).second) names.push_back(name);
            return true;
        });
        // 32-bit voices no 64-bit voice stands in for.
        if (Sapi32Helper::Available()) {
            for (const auto& name : HelperVoices(m_helper)) {
                if (have.insert(name).second) names.push_back(name + k32BitSuffix);
            }
        }
        return names;
    }
    bool SetVoice(const std::string& wanted) override {
        if (EndsWith(wanted, k32BitSuffix)) {
            std::string name = wanted.substr(0, wanted.size() - strlen(k32BitSuffix));
            std::vector<std::string> replies;
            if (!m_helper.Ask("voice\t" + Escape(name), {}, replies) || replies.empty() || replies[0] != "ok") return false;
            m_use32 = true;
            m_voice32 = name;
            m_helper.Send("rate\t" + std::to_string(m_rate));
            m_helper.Send("volume\t" + std::to_string(m_volume));
            m_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
            return true;
        }
        bool done = false;
        ForEachToken([&](ISpObjectToken* token, const std::string& name) {
            if (name == wanted) {
                done = SUCCEEDED(m_voice->SetVoice(token));
                return false;
            }
            return true;
        });
        if (done && m_use32) {
            m_helper.Send("stop");
            m_use32 = false;
        }
        return done;
    }
    void SetRate(int rate) override {
        m_rate = static_cast<long>((rate - 50) / 5);
        m_voice->SetRate(m_rate);
        if (m_use32) m_helper.Send("rate\t" + std::to_string(m_rate));
    }
    void SetVolume(int volume) override {
        m_volume = static_cast<USHORT>(std::clamp(volume, 0, 100));
        m_voice->SetVolume(m_volume);
        if (m_use32) m_helper.Send("volume\t" + std::to_string(m_volume));
    }
    std::string Name() const override { return m_use32 ? "SAPI 5 (32-bit)" : "SAPI 5"; }

private:
    // When the helper has ended (an old voice crashing it): start it again with
    // its voice, rate and volume.
    void Reapply32() {
        if (m_helper.Running()) return;
        std::vector<std::string> replies;
        if (!m_helper.Ask("voice\t" + Escape(m_voice32), {}, replies)) return;
        m_helper.Send("rate\t" + std::to_string(m_rate));
        m_helper.Send("volume\t" + std::to_string(m_volume));
    }

    ISpVoice* m_voice = nullptr;
    Sapi32Helper m_helper;
    bool m_use32 = false;
    std::string m_voice32;
    long m_rate = 0;
    USHORT m_volume = 100;
};

}  // namespace

std::unique_ptr<SpeechBackend> CreateSystemVoiceBackend() {
    auto s = std::make_unique<SapiSpeech>();
    if (!s->Init()) return nullptr;
    return s;
}

std::vector<std::string> Sapi32Voices() {
    Sapi32Helper helper;
    return HelperVoices(helper);
}

bool Sapi32SpeakToFile(const std::string& voice, const std::string& wavPath, const std::string& text, std::string& error) {
    Sapi32Helper helper;
    std::vector<std::string> replies;
    if (!helper.Ask("voice\t" + Escape(voice), {}, replies) || replies.empty() || replies[0] != "ok") {
        error = replies.empty() ? "FastChatSapi32 did not answer." : Unescape(replies[0]);
        return false;
    }
    if (!helper.Ask("file\t" + Escape(wavPath) + "\t" + Escape(text), {}, replies, 60000) || replies.empty() ||
        replies[0] != "ok") {
        error = replies.empty() ? "FastChatSapi32 did not answer." : Unescape(replies[0]);
        return false;
    }
    return true;
}
