#include "zen_provider.h"

namespace fcn {

ZenProvider::ZenProvider() {
  httpClient_ = std::make_unique<network::HttpClient>();
}

ZenProvider::~ZenProvider() {
  Shutdown();
}

bool ZenProvider::Initialize() {
  if (!httpClient_->Initialize()) return false;
  httpClient_->SetBaseUrl("https://opencode.ai/zen/v1");
  return true;
}

void ZenProvider::Shutdown() {
  if (httpClient_) httpClient_->Shutdown();
}

void ZenProvider::Abort() {
  if (httpClient_) httpClient_->Abort();
}

void ZenProvider::FetchModels(
    std::function<void(const std::vector<ProviderModelInfo>&)> callback) {
  httpClient_->FetchModels([callback](const std::vector<network::ModelInfo>& models) {
    std::vector<ProviderModelInfo> result;
    result.reserve(models.size());
    for (const auto& m : models) {
      result.push_back({m.id, m.name, m.allowAnonymous, m.rateLimit});
    }
    callback(result);
  });
}

void ZenProvider::SetTools(const std::vector<ToolDefinition>& tools) {
  tools_ = tools;
}

void ZenProvider::SendMessage(
    const std::string& model,
    const std::string& /*message*/,
    const std::vector<ChatMessage>& history,
    std::function<void(const std::string& chunk, bool isThinking)> onChunk,
    std::function<void(bool success, const std::string& content,
                       const std::string& error,
                       const std::vector<ToolCall>& toolCalls,
                       int inputTokens, int outputTokens)> onComplete) {

  network::ChatRequest request;
  request.model = model;
  request.stream = true;

  // Convert history (includes current user message)
  for (const auto& msg : history) {
    network::Message netMsg;
    netMsg.role = msg.role;
    netMsg.content = msg.content;
    netMsg.toolCallId = msg.toolCallId;
    for (const auto& tc : msg.toolCalls) {
      netMsg.toolCalls.push_back({tc.id, tc.name, tc.arguments});
    }
    request.messages.push_back(netMsg);
  }

  // Convert tool definitions
  for (const auto& td : tools_) {
    request.tools.push_back({td.name, td.description, td.parametersJson});
  }

  httpClient_->SendStreamingChatRequest(request, onChunk,
    [onComplete](const network::ChatResponse& response) {
      // Convert tool calls from network to fcn types
      std::vector<ToolCall> toolCalls;
      for (const auto& tc : response.toolCalls) {
        toolCalls.push_back({tc.id, tc.name, tc.arguments});
      }
      onComplete(!response.error, response.content,
                 response.errorMessage, toolCalls,
                 response.promptTokens, response.completionTokens);
    }
  );
}

void ZenProvider::SetApiKey(const std::string& key) {
  httpClient_->SetApiKey(key);
}

bool ZenProvider::IsAnonymous() const {
  return httpClient_->IsAnonymous();
}

void ZenProvider::SetJsonLogCallback(JsonLogCallback callback) {
  httpClient_->SetJsonLogCallback(callback);
}

std::unique_ptr<ChatProvider> CreateZenProvider() {
  return std::make_unique<ZenProvider>();
}

} // namespace fcn
