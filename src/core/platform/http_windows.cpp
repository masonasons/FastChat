// HTTP through WinHTTP: any method, request bodies, streamed responses, and
// cancelling from another thread (the request handle is closed under it).

#include "core/platform.h"
#include "core/util.h"
#include "version.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

std::string UserAgent() {
    return std::string(APP_NAME "/" APP_VERSION " (Windows)");
}

namespace {

std::string ErrorText(DWORD code) {
    wchar_t* buf = nullptr;
    HMODULE mod = GetModuleHandleW(L"winhttp.dll");
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM |
                       (mod ? FORMAT_MESSAGE_FROM_HMODULE : 0),
                   mod, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring text = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) {
        text.pop_back();
    }
    if (text.empty()) return "Network error " + std::to_string(code);
    return WideToUtf8(text) + " (" + std::to_string(code) + ")";
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

// Watches the cancel flag and closes the request handle, which makes a blocked
// WinHttpReceiveResponse / WinHttpReadData return at once.
class CancelWatch {
public:
    CancelWatch(const std::atomic<bool>* cancel, HINTERNET request) : m_cancel(cancel), m_request(request) {
        if (!cancel) return;
        m_thread = std::thread([this]() {
            std::unique_lock<std::mutex> lock(m_mutex);
            while (!m_done) {
                if (m_cancel->load()) {
                    m_fired = true;
                    WinHttpCloseHandle(m_request);
                    return;
                }
                m_cv.wait_for(lock, std::chrono::milliseconds(100));
            }
        });
    }
    ~CancelWatch() { Stop(); }
    // Returns true if the watcher closed the handle (the caller must not).
    bool Stop() {
        if (m_thread.joinable()) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_done = true;
            }
            m_cv.notify_all();
            m_thread.join();
        }
        return m_fired;
    }

private:
    const std::atomic<bool>* m_cancel;
    HINTERNET m_request;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_done = false;
    bool m_fired = false;
};

void ParseHeaders(const std::wstring& raw, HttpResponse& response) {
    std::string text = WideToUtf8(raw);
    for (const auto& line : SplitLines(text)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = ToLower(Trim(line.substr(0, colon)));
        std::string value = Trim(line.substr(colon + 1));
        auto it = response.headers.find(name);
        if (it != response.headers.end()) it->second += ", " + value;
        else response.headers[name] = value;
    }
}

}  // namespace

HttpResponse HttpSend(const HttpRequest& request) {
    HttpResponse response;

    std::wstring url = Utf8ToWide(request.url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[512] = {0};
    wchar_t path[8192] = {0};
    wchar_t extra[8192] = {0};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 512;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 8192;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 8192;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        response.error = "Invalid URL: " + request.url;
        return response;
    }
    bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    Handle session;
    session.h = WinHttpOpen(Utf8ToWide(UserAgent()).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        // Before Windows 8.1 there is no automatic proxy type.
        session.h = WinHttpOpen(Utf8ToWide(UserAgent()).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.h) {
        response.error = ErrorText(GetLastError());
        return response;
    }
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session.h, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    WinHttpSetTimeouts(session.h, static_cast<int>(request.connectTimeoutMs), static_cast<int>(request.connectTimeoutMs),
                       60000, static_cast<int>(request.receiveTimeoutMs));

    Handle connect;
    connect.h = WinHttpConnect(session.h, host, parts.nPort, 0);
    if (!connect.h) {
        response.error = ErrorText(GetLastError());
        return response;
    }

    std::wstring object = std::wstring(path[0] ? path : L"/") + extra;
    std::wstring method = Utf8ToWide(request.method);
    HINTERNET hRequest = WinHttpOpenRequest(connect.h, method.c_str(), object.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        response.error = ErrorText(GetLastError());
        return response;
    }

    std::wstring headers;
    for (const auto& [name, value] : request.headers) {
        headers += Utf8ToWide(name) + L": " + Utf8ToWide(value) + L"\r\n";
    }

    CancelWatch watch(request.cancel, hRequest);
    auto finish = [&]() {
        if (!watch.Stop()) WinHttpCloseHandle(hRequest);
        if (request.cancel && request.cancel->load()) {
            response.cancelled = true;
            if (response.error.empty()) response.error = "Cancelled";
        }
        return response;
    };

    BOOL ok = WinHttpSendRequest(hRequest, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                 headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                 request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data()),
                                 static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        response.error = ErrorText(GetLastError());
        return finish();
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);

    DWORD headerBytes = 0;
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                        &headerBytes, WINHTTP_NO_HEADER_INDEX);
    if (headerBytes > 0) {
        std::wstring raw(headerBytes / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                &headerBytes, WINHTTP_NO_HEADER_INDEX)) {
            raw.resize(headerBytes / sizeof(wchar_t));
            ParseHeaders(raw, response);
        }
    }

    bool streamBody = request.onData && response.status < 400;
    std::vector<char> buffer(16384);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            DWORD err = GetLastError();
            if (!(request.cancel && request.cancel->load())) response.error = ErrorText(err);
            return finish();
        }
        if (read == 0) break;
        if (streamBody) {
            if (!request.onData(buffer.data(), read)) {
                response.cancelled = true;
                break;
            }
        } else {
            response.body.append(buffer.data(), read);
        }
    }
    response.completed = true;
    return finish();
}
