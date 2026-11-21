// HTTP through libcurl (macOS ships it; Linux has it everywhere).
//
// On Linux it is loaded when first needed rather than linked: Debian and
// Ubuntu's libcurl versions its symbols (CURL_OPENSSL_4) and Fedora's does not,
// so a FastChat linked on one would not start on the other.

#include "core/platform.h"
#include "core/util.h"
#include "version.h"

#define CURL_DISABLE_TYPECHECK  // plain function declarations, which can be pointed to
#include <curl/curl.h>

#include <mutex>

#ifdef __APPLE__
namespace lc {
using ::curl_easy_cleanup;
using ::curl_easy_getinfo;
using ::curl_easy_init;
using ::curl_easy_perform;
using ::curl_easy_setopt;
using ::curl_easy_strerror;
using ::curl_global_init;
using ::curl_slist_append;
using ::curl_slist_free_all;
bool Load(std::string&) { return true; }
}  // namespace lc
#else
#include <dlfcn.h>

#include <type_traits>
namespace lc {
decltype(&::curl_easy_cleanup) curl_easy_cleanup;
decltype(&::curl_easy_getinfo) curl_easy_getinfo;
decltype(&::curl_easy_init) curl_easy_init;
decltype(&::curl_easy_perform) curl_easy_perform;
decltype(&::curl_easy_setopt) curl_easy_setopt;
decltype(&::curl_easy_strerror) curl_easy_strerror;
decltype(&::curl_global_init) curl_global_init;
decltype(&::curl_slist_append) curl_slist_append;
decltype(&::curl_slist_free_all) curl_slist_free_all;

bool Load(std::string& error) {
    void* lib = nullptr;
    for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"}) {
        if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL))) break;
    }
    if (!lib) {
        error = "libcurl was not found; install it (it is the curl package's library) and start FastChat again.";
        return false;
    }
    bool ok = true;
    auto get = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
        ok = ok && fn;
    };
    get(curl_easy_cleanup, "curl_easy_cleanup");
    get(curl_easy_getinfo, "curl_easy_getinfo");
    get(curl_easy_init, "curl_easy_init");
    get(curl_easy_perform, "curl_easy_perform");
    get(curl_easy_setopt, "curl_easy_setopt");
    get(curl_easy_strerror, "curl_easy_strerror");
    get(curl_global_init, "curl_global_init");
    get(curl_slist_append, "curl_slist_append");
    get(curl_slist_free_all, "curl_slist_free_all");
    if (!ok) error = "The libcurl found is missing functions FastChat needs.";
    return ok;
}
}  // namespace lc
#endif

std::string UserAgent() {
#ifdef __APPLE__
    return std::string(APP_NAME "/" APP_VERSION " (macOS)");
#else
    return std::string(APP_NAME "/" APP_VERSION " (Linux)");
#endif
}

namespace {

std::once_flag g_init;
bool g_loaded = false;
std::string g_loadError;

struct Transfer {
    const HttpRequest* request;
    HttpResponse* response;
    CURL* curl;
    bool statusKnown = false;
};

void LearnStatus(Transfer* t) {
    if (t->statusKnown) return;
    long status = 0;
    lc::curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
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
    std::call_once(g_init, []() {
        g_loaded = lc::Load(g_loadError);
        if (g_loaded) lc::curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    HttpResponse response;
    if (!g_loaded) {
        response.error = g_loadError;
        return response;
    }
    CURL* curl = lc::curl_easy_init();
    if (!curl) {
        response.error = "Could not start a network request";
        return response;
    }
    Transfer t{&request, &response, curl};

    lc::curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    lc::curl_easy_setopt(curl, CURLOPT_USERAGENT, UserAgent().c_str());
    lc::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    lc::curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    lc::curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    lc::curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    lc::curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    if (request.method == "POST" || request.method == "PUT" || request.method == "PATCH" || !request.body.empty()) {
        lc::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        lc::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }
    lc::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnData);
    lc::curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    lc::curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, OnHeader);
    lc::curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
    lc::curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
    lc::curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    lc::curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    lc::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(request.connectTimeoutMs));
    lc::curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    lc::curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(request.receiveTimeoutMs / 1000));

    struct curl_slist* headers = nullptr;
    for (const auto& [name, value] : request.headers) {
        headers = lc::curl_slist_append(headers, (name + ": " + value).c_str());
    }
    if (headers) lc::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode code = lc::curl_easy_perform(curl);
    LearnStatus(&t);
    if (request.cancel && request.cancel->load()) {
        response.cancelled = true;
        response.error = "Cancelled";
    } else if (code == CURLE_OK || (response.cancelled && code == CURLE_WRITE_ERROR)) {
        response.completed = true;
    } else {
        response.error = lc::curl_easy_strerror(code);
    }
    if (headers) lc::curl_slist_free_all(headers);
    lc::curl_easy_cleanup(curl);
    return response;
}
