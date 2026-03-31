#pragma once

#include <string>
#include <vector>
#include <functional>
#include <wx/event.h>
#include <wx/process.h>
#include <wx/timer.h>

namespace fcn::claude {

struct ModelInfo {
  std::string id;
  std::string name;
};

using JsonLogCallback = std::function<void(const std::string& direction, const std::string& json)>;

// Communicates with the system 'claude' binary via ACP (stdout stream-json).
// Each SendMessage() spawns a new process with --print --output-format stream-json.
// Session continuity is maintained via --resume <session_id>.
class ClaudeClient : public wxEvtHandler {
public:
  ClaudeClient();
  ~ClaudeClient();

  bool Initialize();
  void Shutdown();

  void SendMessage(
    const std::string& model,
    const std::string& prompt,
    std::function<void(const std::string& chunk)> onChunk,
    std::function<void(bool success, const std::string& fullText, int inputTokens, int outputTokens)> onComplete
  );

  void Interrupt();

  static std::vector<ModelInfo> GetAvailableModels();

  void SetJsonLogCallback(JsonLogCallback cb) { logCallback_ = std::move(cb); }

  const std::string& GetSessionId() const { return sessionId_; }
  void ResetSession() { sessionId_.clear(); }

private:
  void OnPollTimer(wxTimerEvent&);
  void OnProcessEnd(wxProcessEvent&);
  void DrainAndProcess();
  void ProcessLine(const std::string& line);
  void FireComplete(bool success, const std::string& text, int inputTok, int outputTok);

  wxProcess* process_ = nullptr;
  wxTimer pollTimer_;

  std::string lineBuffer_;
  std::string lastAssistantText_;
  std::string sessionId_;
  std::string fullResponse_;
  bool resultReceived_ = false;

  std::function<void(const std::string&)> chunkCallback_;
  std::function<void(bool, const std::string&, int, int)> completeCallback_;
  JsonLogCallback logCallback_;
};

} // namespace fcn::claude
