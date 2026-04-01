#pragma once

#include "chat_provider.h"
#include "http_client.h"
#include <memory>

namespace fcn {

class ZenProvider : public ChatProvider {
public:
  ZenProvider();
  ~ZenProvider() override;

  bool Initialize() override;
  void Shutdown() override;

  ProviderType GetType() const override { return ProviderType::Zen; }
  std::string GetDisplayName() const override { return "OpenCode Zen"; }

  void FetchModels(
    std::function<void(const std::vector<ProviderModelInfo>&)> callback) override;

  void SetTools(const std::vector<ToolDefinition>& tools) override;

  void SendMessage(
    const std::string& model,
    const std::string& message,
    const std::vector<ChatMessage>& history,
    std::function<void(const std::string& chunk, bool isThinking)> onChunk,
    std::function<void(bool success, const std::string& content,
                       const std::string& error,
                       const std::vector<ToolCall>& toolCalls,
                       int inputTokens, int outputTokens)> onComplete
  ) override;

  bool ManagesOwnContext() const override { return false; }
  void ResetContext() override {}

  bool NeedsApiKey() const override { return true; }
  void SetApiKey(const std::string& key) override;
  bool IsAnonymous() const override;

  void SetJsonLogCallback(JsonLogCallback callback) override;

private:
  std::unique_ptr<network::HttpClient> httpClient_;
  std::vector<ToolDefinition> tools_;
};

} // namespace fcn
