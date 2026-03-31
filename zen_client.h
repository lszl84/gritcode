#pragma once

#include "claude_client.h"
#include <wx/event.h>
#include <memory>
#include <vector>

namespace fcn::zen {

// Thin wrapper around ClaudeClient that preserves the event-based API used by MainFrame.
// Emits the same ZEN_* events as before; internally communicates via ACP with the
// system 'claude' binary instead of the OpenCode Zen HTTP API.
class ZenClient : public wxEvtHandler {
public:
  static ZenClient& Instance();

  bool Connect(const std::string& /*apiKey*/ = "");
  void Disconnect();
  bool IsConnected() const;

  void FetchModels();
  std::vector<claude::ModelInfo> GetModels() const;

  void SendMessage(const std::string& model, const std::string& message);

  void SetActiveModel(const std::string& modelId);
  std::string GetActiveModel() const;

  // Always false — auth is handled by the system claude binary
  bool IsAnonymous() const { return false; }

  void SetJsonLogCallback(claude::JsonLogCallback callback);

private:
  ZenClient();
  ~ZenClient();

  ZenClient(const ZenClient&) = delete;
  ZenClient& operator=(const ZenClient&) = delete;

  std::unique_ptr<claude::ClaudeClient> client_;
  std::string activeModel_;
  bool connected_ = false;
  std::vector<claude::ModelInfo> cachedModels_;
};

wxDECLARE_EVENT(ZEN_MESSAGE_RECEIVED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_STREAM_CHUNK, wxCommandEvent);
wxDECLARE_EVENT(ZEN_ERROR_OCCURRED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_CONNECTED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_DISCONNECTED, wxCommandEvent);
wxDECLARE_EVENT(ZEN_MODELS_LOADED, wxCommandEvent);

} // namespace fcn::zen
