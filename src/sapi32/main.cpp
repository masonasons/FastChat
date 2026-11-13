// FastChatSapi32: speaks with the 32-bit SAPI 5 voices for FastChat, a 64-bit
// program, which cannot load them itself (a 32-bit COM voice loads only into a
// 32-bit process). It also keeps an old voice that crashes away from FastChat:
// only this process ends, and FastChat starts it again when next needed.
//
// FastChat starts it hidden and talks to it over standard input and output, a
// line per command, UTF-8, fields separated by tabs, with \\, \n and \t escaped
// in text. It ends when its input closes.
//
//   voices                     "voice\t<name>" for each, then "end"
//   voice\t<name>              "ok", or "error\t<why>"
//   rate\t<-10..10>            (no reply)
//   volume\t<0..100>           (no reply)
//   speak\t<0|1>\t<text>       speaks; 1 interrupts what is being said (no reply)
//   stop                       (no reply)
//   file\t<path>\t<text>       says it into a WAV file: "ok", or "error\t<why>"

#include <windows.h>
#include <mmreg.h>
#include <sapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

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

std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t tab = line.find('\t', start);
        parts.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) return parts;
        start = tab + 1;
    }
}

void Reply(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Every voice token, with its name.
template <typename Fn>
void ForEachVoice(Fn fn) {
    ISpObjectTokenCategory* category = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory,
                                reinterpret_cast<void**>(&category)))) {
        return;
    }
    IEnumSpObjectTokens* tokens = nullptr;
    if (SUCCEEDED(category->SetId(SPCAT_VOICES, FALSE)) && SUCCEEDED(category->EnumTokens(nullptr, nullptr, &tokens))) {
        ULONG count = 0;
        tokens->GetCount(&count);
        for (ULONG i = 0; i < count; i++) {
            ISpObjectToken* token = nullptr;
            if (FAILED(tokens->Item(i, &token)) || !token) continue;
            wchar_t* name = nullptr;
            std::string n;
            if (SUCCEEDED(token->GetStringValue(nullptr, &name)) && name) {
                n = Utf8(name);
                CoTaskMemFree(name);
            }
            bool keep = n.empty() || fn(token, n);
            token->Release();
            if (!keep) break;
        }
        tokens->Release();
    }
    category->Release();
}

// A crash ends this process quietly (no error dialog), and FastChat notices.
LONG WINAPI OnCrash(EXCEPTION_POINTERS* info) {
    TerminateProcess(GetCurrentProcess(), info->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(OnCrash);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    ISpVoice* voice = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&voice)))) {
        return 1;
    }
    std::string pending;
    char chunk[4096];
    for (;;) {
        size_t newline;
        while ((newline = pending.find('\n')) == std::string::npos) {
            DWORD got = 0;
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), chunk, sizeof chunk, &got, nullptr) || got == 0) {
                voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
                voice->Release();
                CoUninitialize();
                return 0;
            }
            pending.append(chunk, got);
        }
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f = Split(line);
        const std::string& cmd = f[0];

        if (cmd == "voices") {
            ForEachVoice([](ISpObjectToken*, const std::string& name) {
                Reply("voice\t" + Escape(name));
                return true;
            });
            Reply("end");
        } else if (cmd == "voice" && f.size() >= 2) {
            std::string wanted = Unescape(f[1]);
            bool done = false;
            ForEachVoice([&](ISpObjectToken* token, const std::string& name) {
                if (name != wanted) return true;
                done = SUCCEEDED(voice->SetVoice(token));
                return false;
            });
            Reply(done ? "ok" : "error\tThere is no 32-bit voice called " + Escape(wanted) + ".");
        } else if (cmd == "rate" && f.size() >= 2) {
            voice->SetRate(atoi(f[1].c_str()));
        } else if (cmd == "volume" && f.size() >= 2) {
            voice->SetVolume(static_cast<USHORT>(atoi(f[1].c_str())));
        } else if (cmd == "speak" && f.size() >= 3) {
            DWORD flags = SPF_ASYNC | SPF_IS_NOT_XML;
            if (f[1] == "1") flags |= SPF_PURGEBEFORESPEAK;
            voice->Speak(Wide(Unescape(f[2])).c_str(), flags, nullptr);
        } else if (cmd == "stop") {
            voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
        } else if (cmd == "file" && f.size() >= 3) {
            // Said into a WAV file instead of aloud (FastChat's tests).
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 1;
            format.nSamplesPerSec = 22050;
            format.wBitsPerSample = 16;
            format.nBlockAlign = 2;
            format.nAvgBytesPerSec = 44100;
            ISpStream* stream = nullptr;
            HRESULT hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream, reinterpret_cast<void**>(&stream));
            if (SUCCEEDED(hr)) {
                hr = stream->BindToFile(Wide(Unescape(f[1])).c_str(), SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &format,
                                        SPFEI_ALL_EVENTS);
                if (FAILED(hr)) {
                    stream->Release();
                    stream = nullptr;
                }
            }
            if (SUCCEEDED(hr)) {
                voice->SetOutput(stream, TRUE);
                hr = voice->Speak(Wide(Unescape(f[2])).c_str(), SPF_IS_NOT_XML, nullptr);
                voice->SetOutput(nullptr, TRUE);
                stream->Close();
                stream->Release();
            }
            Reply(SUCCEEDED(hr) ? std::string("ok") : "error\tSAPI error " + std::to_string(static_cast<long>(hr)));
        } else {
            Reply("error\tUnknown command");
        }
    }
}
