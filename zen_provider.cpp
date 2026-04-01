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

void ZenProvider::SendMessage(
    const std::string& model,
    const std::string& /*message*/,
    const std::vector<ChatMessage>& history,
    std::function<void(const std::string& chunk, bool isThinking)> onChunk,
    std::function<void(bool success, const std::string& content,
                       const std::string& error,
                       int inputTokens, int outputTokens)> onComplete) {

  network::ChatRequest request;
  request.model = model;
  request.stream = true;

  // history already includes the current user message
  for (const auto& msg : history) {
    request.messages.push_back({msg.role, msg.content});
  }

  httpClient_->SendStreamingChatRequest(request, onChunk,
    [onComplete](const network::ChatResponse& response) {
      onComplete(!response.error, response.content,
                 response.errorMessage,
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
