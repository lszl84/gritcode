#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <wx/string.h>
#include <wx/webrequest.h>
#include <wx/event.h>
#include <wx/timer.h>

namespace fcn::network {

struct Message {
  std::string role;
  std::string content;
};

struct ChatRequest {
  std::string model;
  std::vector<Message> messages;
  bool stream = false;
  std::optional<float> temperature;
  std::optional<int> maxTokens;
};

struct ChatResponse {
  std::string id;
  std::string content;
  std::string finishReason;
  int promptTokens = 0;
  int completionTokens = 0;
  int totalTokens = 0;
  bool error = false;
  std::string errorMessage;
};

struct ModelInfo {
  std::string id;
  std::string name;
  bool allowAnonymous = false;
  int rateLimit = 0;
};

// Callbacks for async operations
using ModelsCallback = std::function<void(const std::vector<ModelInfo>&)>;
using ChatCallback = std::function<void(const ChatResponse&)>;
using JsonLogCallback = std::function<void(const std::string& direction, const std::string& json)>;

class HttpClient : public wxEvtHandler {
public:
  HttpClient();
  ~HttpClient();

  bool Initialize();
  void Shutdown();

  void SetBaseUrl(const std::string& url);
  void SetApiKey(const std::string& apiKey);
  void SetTimeout(int seconds);
  void SetJsonLogCallback(JsonLogCallback callback);

  // Async methods - callbacks will be invoked when complete
  void FetchModels(ModelsCallback callback);
  void SendChatRequest(const ChatRequest& request, ChatCallback callback);
  
  void SendStreamingChatRequest(
    const ChatRequest& request,
    std::function<void(const std::string& chunk, bool isThinking)> onChunk,
    std::function<void(const ChatResponse& response)> onComplete
  );

  bool IsAnonymous() const { return apiKey_.empty(); }

private:
  void OnRequestStateChanged(wxWebRequestEvent& event);
  void OnRequestData(wxWebRequestEvent& event);

  std::string BuildRequestJson(const ChatRequest& request);
  ChatResponse ParseResponse(const std::string& json);
  std::vector<ModelInfo> ParseModels(const std::string& json);

  void ProcessSSEChunk(const std::string& chunk);
  void RetryCurrentRequest();

  wxWebRequest currentRequest_;

  std::string baseUrl_ = "https://opencode.ai/zen/v1";
  std::string apiKey_;
  int timeout_ = 60;
  bool initialized_ = false;

  // Callbacks for current operation
  ModelsCallback modelsCallback_;
  ChatCallback chatCallback_;
  JsonLogCallback jsonLogCallback_;

  // Streaming support
  std::function<void(const std::string& chunk, bool isThinking)> streamingChunkCallback_;
  std::function<void(const ChatResponse& response)> streamingCompleteCallback_;
  std::string sseBuffer_;
  std::string accumulatedContent_;
  bool isStreaming_ = false;

  // Retry support for 429 rate limiting
  static constexpr int MAX_RETRIES = 3;
  int retryCount_ = 0;
  wxTimer retryTimer_;
  std::string pendingRequestBody_;  // Saved for retries
};

} // namespace fcn::network
