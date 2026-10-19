#pragma once

#include <wx/event.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Result of a single HTTP request. `body` is populated only by the synchronous
// API; the streaming wrapper hands raw bytes to its data callback as they
// arrive and leaves `body` empty.
struct WebResponse {
    bool ok = false;
    long status = 0;
    std::string contentType;  // lowercased
    std::string body;
    std::string error;
    // True when the request failed at the transport level (timeout, reset,
    // closed mid-stream, unreachable, …) rather than with an HTTP error.
    // Distinct from `error == "cancelled"`.
    bool networkError = false;
};

struct WebRequestSpec {
    std::string url;
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string bodyContentType = "application/json";

    // libcurl LOW_SPEED watchdog: fail with CURLE_OPERATION_TIMEDOUT when the
    // transfer rate stays under 1 byte/sec for this many seconds. Catches
    // half-closed SSE connections that would otherwise hang forever.
    int idleTimeoutSeconds = 0;
    int connectTimeoutSeconds = 30;
};

// Set from any thread to abort an in-flight synchronous request.
struct WebCancelToken {
    std::atomic<bool> cancelled{false};
};

// Blocking call. Runs on the calling thread, returns the full response body.
WebResponse RequestSync(WebRequestSpec spec, WebCancelToken* token = nullptr);

// Forward-declared so the unique_ptr in StreamingWebRequest doesn't need its
// definition; defined in the .cpp.
struct StreamingRequestImpl;

// Async streaming wrapper. Construction starts a worker thread that runs
// curl_easy_perform; data and completion callbacks are dispatched back through
// the supplied wxEvtHandler's CallAfter so handlers run on the wx event loop.
//
// RAII: destruction (or move-assign onto a live request) signals cancellation
// and joins the worker before returning.
class StreamingWebRequest {
public:
    using DataFn = std::function<void(std::string_view)>;
    using DoneFn = std::function<void(WebResponse)>;

    StreamingWebRequest() noexcept;
    StreamingWebRequest(wxEvtHandler* target,
                        WebRequestSpec spec,
                        DataFn onData,
                        DoneFn onDone);
    ~StreamingWebRequest();

    StreamingWebRequest(const StreamingWebRequest&) = delete;
    StreamingWebRequest& operator=(const StreamingWebRequest&) = delete;
    StreamingWebRequest(StreamingWebRequest&& other) noexcept;
    StreamingWebRequest& operator=(StreamingWebRequest&& other) noexcept;

    // True from construction until the worker has finished.
    bool IsActive() const noexcept;

    // Idempotent. Safe from any thread.
    void Cancel() noexcept;

private:
    std::unique_ptr<StreamingRequestImpl> impl_;
};

// Server-sent events, the chat APIs' streaming format. Appends `chunk` to
// `buf`, takes each complete event off the front and returns its data (the
// event's `data:` lines joined by '\n'); events without data, such as
// comments and keepalives, are skipped. Lines end in "\n", "\r\n" or "\r",
// and a blank line ends an event. An unfinished event stays in `buf` (so a
// non-SSE error body stays whole there) until `streamEnded`, when what is
// left counts as a last event even without the blank line.
std::vector<std::string> TakeSseEvents(std::string& buf, std::string_view chunk,
                                       bool streamEnded = false);
