#include "curl_http.h"
#include <sstream>
#include <cstring>

namespace net {

CurlHttpClient::CurlHttpClient() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

CurlHttpClient::~CurlHttpClient() {
    Abort();
    if (requestThread_.joinable()) requestThread_.join();
    curl_global_cleanup();
}

void CurlHttpClient::Abort() {
    aborted_ = true;
}

struct StreamCtx {
    std::string sseBuffer;
    std::string accContent;
    std::vector<json> toolCalls;  // raw tool_calls json objects
    std::string finishReason;
    std::function<void(const std::string&, bool)> onChunk;
    CurlHttpClient* client;
    std::atomic<bool>* aborted;

    // Tool call accumulation (streamed across chunks)
    struct PendingTC { std::string id, name, args; };
    std::vector<PendingTC> pendingToolCalls;
};

size_t CurlHttpClient::WriteCallback(char* data, size_t size, size_t nmemb, void* userp) {
    auto* ctx = static_cast<StreamCtx*>(userp);
    if (ctx->aborted->load()) return 0;  // Abort transfer

    size_t bytes = size * nmemb;
    ctx->sseBuffer.append(data, bytes);

    // Process complete SSE events (separated by \n\n)
    size_t pos;
    while ((pos = ctx->sseBuffer.find("\n\n")) != std::string::npos) {
        std::string event = ctx->sseBuffer.substr(0, pos);
        ctx->sseBuffer.erase(0, pos + 2);

        std::istringstream stream(event);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.substr(0, 6) == "data: ") {
                std::string jsonData = line.substr(6);
                if (jsonData == "[DONE]") continue;

                try {
                    auto j = json::parse(jsonData);
                    if (!j.contains("choices") || j["choices"].empty()) continue;
                    const auto& choice = j["choices"][0];

                    // Track finish_reason
                    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
                        ctx->finishReason = choice["finish_reason"].get<std::string>();
                    }

                    if (!choice.contains("delta")) continue;
                    const auto& delta = choice["delta"];

                    // Content
                    std::string content;
                    bool isThinking = false;

                    for (const auto& field : {"reasoning_content", "reasoning", "reasoning_text"}) {
                        if (delta.contains(field) && delta[field].is_string()) {
                            content = delta[field].get<std::string>();
                            isThinking = true;
                            break;
                        }
                    }
                    if (content.empty() && delta.contains("content") && delta["content"].is_string()) {
                        content = delta["content"].get<std::string>();
                    }
                    if (!content.empty()) {
                        if (!isThinking) ctx->accContent += content;
                        if (ctx->onChunk) ctx->onChunk(content, isThinking);
                    }

                    // Tool calls
                    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                        for (const auto& tc : delta["tool_calls"]) {
                            int idx = tc.value("index", 0);
                            if (idx >= (int)ctx->pendingToolCalls.size())
                                ctx->pendingToolCalls.resize(idx + 1);
                            if (tc.contains("id") && tc["id"].is_string())
                                ctx->pendingToolCalls[idx].id = tc["id"].get<std::string>();
                            if (tc.contains("function") && tc["function"].is_object()) {
                                const auto& fn = tc["function"];
                                if (fn.contains("name") && fn["name"].is_string())
                                    ctx->pendingToolCalls[idx].name = fn["name"].get<std::string>();
                                if (fn.contains("arguments") && fn["arguments"].is_string())
                                    ctx->pendingToolCalls[idx].args += fn["arguments"].get<std::string>();
                            }
                        }
                    }
                } catch (...) {}
            }
        }
    }
    return bytes;
}

void CurlHttpClient::FetchModels(std::function<void(std::vector<ModelInfo>)> cb) {
    Abort();
    if (requestThread_.joinable()) requestThread_.detach();

    aborted_ = false;
    requestThread_ = std::thread([this, cb]() {
        CURL* curl = curl_easy_init();
        if (!curl) { cb({}); return; }

        std::string url = baseUrl_ + "/models";
        std::string response;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](char* d, size_t s, size_t n, void* p) -> size_t {
                ((std::string*)p)->append(d, s * n);
                return s * n;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (!apiKey_.empty()) {
            headers = curl_slist_append(headers,
                ("Authorization: Bearer " + apiKey_).c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        std::vector<ModelInfo> models;
        if (res == CURLE_OK) {
            try {
                auto j = json::parse(response);
                if (j.contains("data") && j["data"].is_array()) {
                    for (const auto& m : j["data"]) {
                        models.push_back({
                            m.value("id", ""),
                            m.value("name", m.value("id", "")),
                            m.value("allow_anonymous", false),
                            m.value("rate_limit", 0)
                        });
                    }
                }
            } catch (...) {}
        }
        cb(std::move(models));
    });
}

void CurlHttpClient::SendStreaming(
    const std::string& requestJson,
    std::function<void(const std::string&, bool)> onChunk,
    std::function<void(bool, const std::string&, const std::string&,
                       const std::vector<json>&, int, int)> onComplete) {

    // Abort any in-flight request and detach its thread (don't block main thread)
    Abort();
    if (requestThread_.joinable()) requestThread_.detach();

    aborted_ = false;
    requestThread_ = std::thread([this, requestJson, onChunk, onComplete]() {
        CURL* curl = curl_easy_init();
        if (!curl) {
            onComplete(false, "", "Failed to init curl", {}, 0, 0);
            return;
        }

        std::string url = baseUrl_ + "/chat/completions";

        StreamCtx ctx;
        ctx.onChunk = onChunk;
        ctx.client = this;
        ctx.aborted = &aborted_;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestJson.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

        // Timeouts: don't hang forever
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);      // 15s to connect
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);      // At least 1 byte/s
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);      // ...for 60s, else abort

        // Progress callback for abort during connect/DNS
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
            +[](void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return ((std::atomic<bool>*)p)->load() ? 1 : 0;  // 1 = abort
            });
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &aborted_);

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: text/event-stream");
        if (!apiKey_.empty()) {
            headers = curl_slist_append(headers,
                ("Authorization: Bearer " + apiKey_).c_str());
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        CURLcode res = curl_easy_perform(curl);

        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (aborted_.load()) {
            onComplete(false, ctx.accContent, "Cancelled", {}, 0, 0);
            return;
        }

        // Convert pending tool calls to json
        std::vector<json> toolCallsJson;
        for (auto& tc : ctx.pendingToolCalls) {
            if (!tc.name.empty()) {
                toolCallsJson.push_back({
                    {"id", tc.id},
                    {"name", tc.name},
                    {"arguments", tc.args}
                });
            }
        }

        if (res != CURLE_OK || httpCode >= 400) {
            std::string err = "HTTP " + std::to_string(httpCode);
            if (res != CURLE_OK) err = curl_easy_strerror(res);
            onComplete(false, ctx.accContent, err, {}, 0, 0);
        } else {
            onComplete(true, ctx.accContent, "", toolCallsJson, 0, 0);
        }
    });
}

} // namespace net
