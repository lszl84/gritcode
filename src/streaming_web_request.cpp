#include "streaming_web_request.h"

#include <curl/curl.h>
#include <wx/event.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <cctype>
#include <cstring>
#include <thread>
#include <utility>

namespace {

// One curl_global_init / cleanup pair, run lazily on first use.
struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobal() { curl_global_cleanup(); }
};
void EnsureCurlGlobal() {
    static CurlGlobal g;
    (void)g;
}

void ApplySpec(CURL* curl, const WebRequestSpec& spec, struct curl_slist*& outHeaders) {
    curl_easy_setopt(curl, CURLOPT_URL, spec.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)spec.connectTimeoutSeconds);
    if (spec.idleTimeoutSeconds > 0) {
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)spec.idleTimeoutSeconds);
    }
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
#ifdef _WIN32
    // MSYS2 libcurl uses OpenSSL backend, not Schannel. Point at the
    // bundled cacert.pem installed next to the exe.
    static std::string caPath;
    if (caPath.empty()) {
        wxString exeDir = wxStandardPaths::Get().GetResourcesDir();
        if (wxFileName::FileExists(exeDir + "/cacert.pem"))
            caPath = (exeDir + "/cacert.pem").ToStdString();
        else if (wxFileName::FileExists(exeDir + "/../cacert.pem"))
            caPath = (exeDir + "/../cacert.pem").ToStdString();
        else
            caPath = (exeDir + "/cacert.pem").ToStdString();
    }
    curl_easy_setopt(curl, CURLOPT_CAINFO, caPath.c_str());
#endif

    if (spec.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, spec.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)spec.body.size());
    } else if (spec.method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, spec.method.c_str());
        if (!spec.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, spec.body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)spec.body.size());
        }
    }

    outHeaders = nullptr;
    if ((spec.method == "POST" || (spec.method != "GET" && !spec.body.empty()))
        && !spec.bodyContentType.empty()) {
        std::string h = "Content-Type: " + spec.bodyContentType;
        outHeaders = curl_slist_append(outHeaders, h.c_str());
    }
    for (const auto& [k, v] : spec.headers) {
        std::string h = k + ": " + v;
        outHeaders = curl_slist_append(outHeaders, h.c_str());
    }
    if (outHeaders) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, outHeaders);
}

std::string LowercaseContentType(CURL* curl) {
    char* ct = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
    if (!ct) return {};
    std::string s(ct);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// ---- sync ----

struct SyncCtx {
    std::string* body;
    WebCancelToken* token;
};

size_t SyncWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<SyncCtx*>(userdata);
    if (ctx->token && ctx->token->cancelled.load()) return 0;
    ctx->body->append(ptr, size * nmemb);
    return size * nmemb;
}

int SyncProgress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<SyncCtx*>(userdata);
    return (ctx->token && ctx->token->cancelled.load()) ? 1 : 0;
}

}  // namespace

WebResponse RequestSync(WebRequestSpec spec, WebCancelToken* token) {
    EnsureCurlGlobal();
    WebResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.error = "curl_easy_init failed";
        return resp;
    }

    struct curl_slist* hdrs = nullptr;
    ApplySpec(curl, spec, hdrs);

    SyncCtx ctx{&resp.body, token};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, SyncWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, SyncProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    resp.status = status;
    resp.contentType = LowercaseContentType(curl);

    if (rc == CURLE_OK) {
        resp.ok = (status >= 200 && status < 400);
        if (!resp.ok) resp.error = "HTTP " + std::to_string(status);
    } else {
        bool cancelled = token && token->cancelled.load();
        resp.error = cancelled ? std::string("cancelled")
                               : std::string(curl_easy_strerror(rc));
        if (!cancelled) resp.networkError = true;
    }

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return resp;
}

// ---- streaming ----

// Held by the worker and any in-flight CallAfter lambdas. Once the last ref
// dies the std::function objects (and anything they captured) are released.
// wx drops queued CallAfters when the target wxEvtHandler is destroyed, so
// user-side captures stay safe across handler teardown.
struct StreamCbHolder {
    StreamingWebRequest::DataFn data;
    StreamingWebRequest::DoneFn done;
};

struct StreamingRequestImpl {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> active{true};
    std::thread worker;
    std::shared_ptr<StreamCbHolder> cb;
};

namespace {

struct StreamCtx {
    std::atomic<bool>* cancelled;
    wxEvtHandler* target;
    std::shared_ptr<StreamCbHolder> cb;
};

size_t StreamWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    if (ctx->cancelled->load()) return 0;
    size_t n = size * nmemb;
    if (n == 0) return 0;
    if (ctx->cb && ctx->cb->data && ctx->target) {
        std::string chunk(ptr, n);
        auto cb = ctx->cb;
        ctx->target->CallAfter([cb, chunk = std::move(chunk)]() {
            if (cb && cb->data) cb->data(chunk);
        });
    }
    return n;
}

int StreamProgress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    return ctx->cancelled->load() ? 1 : 0;
}

}  // namespace

StreamingWebRequest::StreamingWebRequest() noexcept = default;

StreamingWebRequest::StreamingWebRequest(wxEvtHandler* target,
                                         WebRequestSpec spec,
                                         DataFn onData,
                                         DoneFn onDone)
    : impl_(std::make_unique<StreamingRequestImpl>()) {
    EnsureCurlGlobal();
    impl_->cb = std::make_shared<StreamCbHolder>();
    impl_->cb->data = std::move(onData);
    impl_->cb->done = std::move(onDone);

    StreamingRequestImpl* impl = impl_.get();
    impl_->worker = std::thread([impl, target, spec = std::move(spec)]() mutable {
        WebResponse resp;
        CURL* curl = curl_easy_init();
        if (!curl) {
            resp.error = "curl_easy_init failed";
        } else {
            struct curl_slist* hdrs = nullptr;
            ApplySpec(curl, spec, hdrs);

            StreamCtx ctx{&impl->cancelled, target, impl->cb};
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamWrite);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, StreamProgress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

            CURLcode rc = curl_easy_perform(curl);
            long status = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
            resp.status = status;
            resp.contentType = LowercaseContentType(curl);

            if (rc == CURLE_OK) {
                resp.ok = (status >= 200 && status < 400);
                if (!resp.ok) resp.error = "HTTP " + std::to_string(status);
            } else {
                bool cancelled = impl->cancelled.load();
                resp.error = cancelled ? std::string("cancelled")
                                       : std::string(curl_easy_strerror(rc));
                if (!cancelled) resp.networkError = true;
            }

            if (hdrs) curl_slist_free_all(hdrs);
            curl_easy_cleanup(curl);
        }

        impl->active.store(false);
        if (impl->cb && impl->cb->done && target) {
            auto cb = impl->cb;
            target->CallAfter([cb, resp = std::move(resp)]() mutable {
                if (cb && cb->done) cb->done(std::move(resp));
            });
        }
    });
}

StreamingWebRequest::~StreamingWebRequest() {
    if (impl_) {
        impl_->cancelled.store(true);
        if (impl_->worker.joinable()) impl_->worker.join();
    }
}

StreamingWebRequest::StreamingWebRequest(StreamingWebRequest&&) noexcept = default;

StreamingWebRequest& StreamingWebRequest::operator=(StreamingWebRequest&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            impl_->cancelled.store(true);
            if (impl_->worker.joinable()) impl_->worker.join();
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}

bool StreamingWebRequest::IsActive() const noexcept {
    return impl_ && impl_->active.load();
}

void StreamingWebRequest::Cancel() noexcept {
    if (impl_) impl_->cancelled.store(true);
}

namespace {

// Length of the line ending at s[i] ("\n", "\r\n" or "\r"), 0 if none.
size_t LineEndAt(std::string_view s, size_t i) {
    if (i >= s.size()) return 0;
    if (s[i] == '\n') return 1;
    if (s[i] == '\r') return i + 1 < s.size() && s[i + 1] == '\n' ? 2 : 1;
    return 0;
}

// The data of one event: its `data:` lines (one optional space after the
// colon dropped) joined by '\n'. Other fields and comments are ignored.
std::string SseEventData(std::string_view event) {
    std::string data;
    bool first = true;
    while (!event.empty()) {
        size_t end = event.find_first_of("\r\n");
        std::string_view line = event.substr(0, end);
        event.remove_prefix(end == std::string_view::npos
                                ? event.size() : end + LineEndAt(event, end));
        if (!line.starts_with("data:")) continue;
        line.remove_prefix(5);
        if (line.starts_with(' ')) line.remove_prefix(1);
        if (!first) data += '\n';
        data += line;
        first = false;
    }
    return data;
}

}  // namespace

std::vector<std::string> TakeSseEvents(std::string& buf, std::string_view chunk,
                                       bool streamEnded) {
    // A blank line is at most 4 bytes ("\r\n\r\n") and `buf` held none, so
    // only its last 3 bytes can start one: don't rescan the rest.
    size_t pos = buf.size() < 3 ? 0 : buf.size() - 3;
    buf.append(chunk);

    std::vector<std::string> events;
    size_t start = 0;  // where the current event begins
    while ((pos = buf.find_first_of("\r\n", pos)) != std::string::npos) {
        size_t next = pos + LineEndAt(buf, pos);
        // A line ending followed by another one is a blank line, which ends
        // the event. At the end of `buf` that is still unknown: wait.
        size_t blank = LineEndAt(buf, next);
        if (blank == 0) {
            pos = next;
            continue;
        }
        std::string data = SseEventData(std::string_view(buf).substr(start, pos - start));
        if (!data.empty()) events.push_back(std::move(data));
        start = pos = next + blank;
    }
    buf.erase(0, start);

    if (streamEnded && !buf.empty()) {
        std::string data = SseEventData(buf);
        if (!data.empty()) events.push_back(std::move(data));
        buf.clear();
    }
    return events;
}
