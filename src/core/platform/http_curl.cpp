// HTTP through libcurl (macOS ships it; Linux has it everywhere).

#include "core/platform.h"
#include "core/util.h"
#include "version.h"

#include <curl/curl.h>

#include <mutex>

std::string UserAgent() {
#ifdef __APPLE__
    return std::string(APP_NAME "/" APP_VERSION " (macOS)");
#else
    return std::string(APP_NAME "/" APP_VERSION " (Linux)");
#endif
}

namespace {

std::once_flag g_init;

struct Transfer {
    const HttpRequest* request;
    HttpResponse* response;
    CURL* curl;
    bool statusKnown = false;
};

void LearnStatus(Transfer* t) {
    if (t->statusKnown) return;
    long status = 0;
    curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
    t->response->status = static_cast<int>(status);
    t->statusKnown = true;
}

size_t OnData(char* data, size_t size, size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    size_t bytes = size * count;
    LearnStatus(t);
    if (t->request->onData && t->response->status < 400) {
        if (!t->request->onData(data, bytes)) {
            t->response->cancelled = true;
            return 0;
        }
    } else {
        t->response->body.append(data, bytes);
    }
    return bytes;
}

size_t OnHeader(char* data, size_t size, size_t count, void* user) {
    auto* t = static_cast<Transfer*>(user);
    std::string line(data, size * count);
    if (StartsWith(line, "HTTP/")) {
        t->response->headers.clear();  // a new response (after a redirect or 100 Continue)
        t->statusKnown = false;
    }
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        t->response->headers[ToLower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
    }
    return size * count;
}

int OnProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* t = static_cast<Transfer*>(user);
    return (t->request->cancel && t->request->cancel->load()) ? 1 : 0;
}

}  // namespace

HttpResponse HttpSend(const HttpRequest& request) {
    std::call_once(g_init, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "Could not start a network request";
        return response;
    }
    Transfer t{&request, &response, curl};

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UserAgent().c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    if (request.method == "POST" || request.method == "PUT" || request.method == "PATCH" || !request.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, OnHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.connectTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(request.receiveTimeoutMs / 1000));

    struct curl_slist* headers = nullptr;
    for (const auto& [name, value] : request.headers) {
        headers = curl_slist_append(headers, (name + ": " + value).c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode code = curl_easy_perform(curl);
    LearnStatus(&t);
    if (request.cancel && request.cancel->load()) {
        response.cancelled = true;
        response.error = "Cancelled";
    } else if (code == CURLE_OK || (response.cancelled && code == CURLE_WRITE_ERROR)) {
        response.completed = true;
    } else {
        response.error = curl_easy_strerror(code);
    }
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}
