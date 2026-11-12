// SAPI 5 directly: the installed Windows voices, with their own rate and volume,
// independent of any screen reader.

#include "core/speech.h"
#include "core/util.h"

#include <windows.h>
#include <sapi.h>

namespace {

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

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
        std::wstring w = Utf8ToWide(text);
        DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML;
        if (interrupt) flags |= SPF_PURGEBEFORESPEAK;
        return SUCCEEDED(m_voice->Speak(w.c_str(), flags, nullptr));
    }
    void Stop() override { m_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr); }

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
        ForEachToken([&](ISpObjectToken*, const std::string& name) {
            if (!name.empty()) names.push_back(name);
            return true;
        });
        return names;
    }
    bool SetVoice(const std::string& wanted) override {
        bool done = false;
        ForEachToken([&](ISpObjectToken* token, const std::string& name) {
            if (name == wanted) {
                done = SUCCEEDED(m_voice->SetVoice(token));
                return false;
            }
            return true;
        });
        return done;
    }
    void SetRate(int rate) override { m_voice->SetRate(static_cast<long>((rate - 50) / 5)); }
    void SetVolume(int volume) override { m_voice->SetVolume(static_cast<USHORT>(volume)); }
    std::string Name() const override { return "SAPI 5"; }

private:
    ISpVoice* m_voice = nullptr;
};

}  // namespace

std::unique_ptr<SpeechBackend> CreateSystemVoiceBackend() {
    auto s = std::make_unique<SapiSpeech>();
    if (!s->Init()) return nullptr;
    return s;
}
