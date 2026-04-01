#pragma once

#include "chat_provider.h"
#include <wx/event.h>
#include <wx/process.h>
#include <wx/timer.h>

namespace fcn {

// Communicates with the system 'claude' binary via ACP.
// Each SendMessage() spawns a process with --print --output-format stream-json.
// Conversation history is formatted into the prompt so context survives
// cross-provider switches (Zen ↔ Claude).
// Supports thinking blocks (thinking_delta events).
class ClaudeProvider : public ChatProvider, public wxEvtHandler {
public:
  ClaudeProvider();
  ~ClaudeProvider() override;

  bool Initialize() override;
  void Shutdown() override;
  void Abort() override;

  ProviderType GetType() const override { return ProviderType::Claude; }
  std::string GetDisplayName() const override { return "Claude (ACP)"; }

  void FetchModels(
    std::function<void(const std::vector<ProviderModelInfo>&)> callback) override;

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

  bool NeedsApiKey() const override { return false; }
  bool IsAnonymous() const override { return false; }

  void SetJsonLogCallback(JsonLogCallback callback) override;

  void Interrupt();

private:
  void OnPollTimer(wxTimerEvent&);
  void OnProcessEnd(wxProcessEvent&);
  void DrainAndProcess();
  void ProcessLine(const std::string& line);
  void FireComplete(bool success, const std::string& text,
                    const std::string& error, int inputTok, int outputTok);

  wxProcess* process_ = nullptr;
  wxTimer pollTimer_;

  std::string lineBuffer_;
  std::string fullResponse_;
  bool resultReceived_ = false;
  bool inThinkingBlock_ = false;

  std::function<void(const std::string&, bool)> chunkCallback_;
  std::function<void(bool, const std::string&, const std::string&, const std::vector<ToolCall>&, int, int)> completeCallback_;
  JsonLogCallback logCallback_;
};

} // namespace fcn
