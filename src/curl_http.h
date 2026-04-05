// FastCode Native — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once
#include <string>
#include <vector>
#include <functional>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <mutex>
#include <atomic>

namespace net {

using json = nlohmann::json;

struct ModelInfo {
    std::string id, name;
    bool allowAnonymous = false;
    int rateLimit = 0;
};

// Async SSE streaming HTTP client using libcurl.
// Runs requests in a background thread, calls back on that thread.
// Caller must synchronize (e.g., queue events for main loop).
class CurlHttpClient {
public:
    CurlHttpClient();
    ~CurlHttpClient();

    void SetBaseUrl(const std::string& url) { baseUrl_ = url; }
    void SetApiKey(const std::string& key) { apiKey_ = key; }
    bool IsAnonymous() const { return apiKey_.empty(); }

    // Fetch models (async, callback on bg thread)
    void FetchModels(std::function<void(std::vector<ModelInfo>)> cb);

    // Send streaming chat request (callbacks on bg thread)
    void SendStreaming(
        const std::string& requestJson,
        std::function<void(const std::string& chunk, bool isThinking)> onChunk,
        std::function<void(bool ok, const std::string& content,
                           const std::string& error,
                           const std::vector<json>& toolCalls,
                           int inTok, int outTok)> onComplete
    );

    void Abort();

private:
    std::string baseUrl_;
    std::string apiKey_;
    std::atomic<bool> aborted_{false};
    std::thread requestThread_;

    static size_t WriteCallback(char* data, size_t size, size_t nmemb, void* userp);
    void ProcessSSELine(const std::string& line, const std::string& sseBuffer,
                        std::string& accContent,
                        std::vector<json>& toolCalls,
                        std::string& finishReason,
                        const std::function<void(const std::string&, bool)>& onChunk);
};

} // namespace net
