#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace fcn {

enum class ProviderType {
  Zen,    // OpenCode Zen HTTP API
  Claude  // Claude binary via ACP (stream-json)
};

struct ChatMessage {
  std::string role;    // "user", "assistant", "system"
  std::string content;
};

struct ProviderModelInfo {
  std::string id;
  std::string name;
  bool allowAnonymous = false;
  int rateLimit = 0;
};

using JsonLogCallback = std::function<void(const std::string& direction, const std::string& json)>;

// Abstract interface for chat backends.
// Implementations: ZenProvider (HTTP API), ClaudeProvider (ACP binary).
class ChatProvider {
public:
  virtual ~ChatProvider() = default;

  virtual bool Initialize() = 0;
  virtual void Shutdown() = 0;

  virtual ProviderType GetType() const = 0;
  virtual std::string GetDisplayName() const = 0;

  virtual void FetchModels(
    std::function<void(const std::vector<ProviderModelInfo>&)> callback) = 0;

  // Send a streaming chat message.
  // |history| contains all prior messages INCLUDING the current user message.
  // Providers that manage their own context (Claude ACP) ignore history and
  // use |message| directly.
  virtual void SendMessage(
    const std::string& model,
    const std::string& message,
    const std::vector<ChatMessage>& history,
    std::function<void(const std::string& chunk, bool isThinking)> onChunk,
    std::function<void(bool success, const std::string& content,
                       const std::string& error,
                       int inputTokens, int outputTokens)> onComplete
  ) = 0;

  // True if the provider manages conversation context internally
  // (e.g. Claude ACP uses --resume). When true, the caller should
  // not accumulate conversation history.
  virtual bool ManagesOwnContext() const = 0;
  virtual void ResetContext() = 0;

  virtual bool NeedsApiKey() const = 0;
  virtual void SetApiKey(const std::string&) {}
  virtual bool IsAnonymous() const { return false; }

  virtual void SetJsonLogCallback(JsonLogCallback callback) = 0;
};

std::unique_ptr<ChatProvider> CreateZenProvider();
std::unique_ptr<ChatProvider> CreateClaudeProvider();

} // namespace fcn
