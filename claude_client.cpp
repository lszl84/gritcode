#include "claude_client.h"
#include <wx/wx.h>
#include <wx/log.h>
#include <nlohmann/json.hpp>

namespace fcn::claude {

using json = nlohmann::json;

static constexpr int kPollIntervalMs = 10;

ClaudeClient::ClaudeClient() : pollTimer_(this) {
  Bind(wxEVT_TIMER, &ClaudeClient::OnPollTimer, this, pollTimer_.GetId());
}

ClaudeClient::~ClaudeClient() {
  Shutdown();
}

bool ClaudeClient::Initialize() {
  return true;
}

void ClaudeClient::Shutdown() {
  pollTimer_.Stop();
  if (process_) {
    long pid = process_->GetPid();
    if (pid > 0) wxKill(pid, wxSIGKILL);
    process_ = nullptr;
  }
}

void ClaudeClient::SendMessage(
    const std::string& model,
    const std::string& prompt,
    std::function<void(const std::string&)> onChunk,
    std::function<void(bool, const std::string&, int, int)> onComplete) {

  if (process_) {
    wxLogWarning("ClaudeClient: already running, interrupting previous process");
    Interrupt();
  }

  chunkCallback_ = std::move(onChunk);
  completeCallback_ = std::move(onComplete);
  lastAssistantText_.clear();
  fullResponse_.clear();
  lineBuffer_.clear();
  resultReceived_ = false;

  // Build command
  wxString cmd = "claude --print --verbose --output-format stream-json --include-partial-messages";
  if (!model.empty() && model != "claude") {
    cmd += " --model " + wxString::FromUTF8(model);
  }
  if (!sessionId_.empty()) {
    cmd += " --resume " + wxString::FromUTF8(sessionId_);
  }

  if (logCallback_) {
    logCallback_("CMD", cmd.ToStdString());
    logCallback_("SEND", prompt);
  }

  process_ = new wxProcess(this);
  process_->Redirect();
  process_->Bind(wxEVT_END_PROCESS, &ClaudeClient::OnProcessEnd, this);

  long pid = wxExecute(cmd, wxEXEC_ASYNC | wxEXEC_MAKE_GROUP_LEADER, process_);
  if (pid == 0) {
    wxLogError("ClaudeClient: failed to spawn claude process");
    auto cb = std::move(completeCallback_);
    process_ = nullptr;
    if (cb) cb(false, "Failed to spawn claude binary", 0, 0);
    return;
  }

  // Write prompt as plain text to stdin, then close it
  wxOutputStream* stdinStream = process_->GetOutputStream();
  if (stdinStream) {
    stdinStream->Write(prompt.c_str(), prompt.length());
    process_->CloseOutput();
  }

  pollTimer_.Start(kPollIntervalMs);
}

void ClaudeClient::OnPollTimer(wxTimerEvent&) {
  if (!process_) {
    pollTimer_.Stop();
    return;
  }

  wxInputStream* out = process_->GetInputStream();
  if (!out) return;

  while (out->CanRead()) {
    char buf[4096];
    out->Read(buf, sizeof(buf) - 1);
    size_t n = out->LastRead();
    if (n == 0) break;
    lineBuffer_.append(buf, n);

    size_t pos;
    while ((pos = lineBuffer_.find('\n')) != std::string::npos) {
      std::string line = lineBuffer_.substr(0, pos);
      lineBuffer_.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) ProcessLine(line);
    }
  }
}

void ClaudeClient::OnProcessEnd(wxProcessEvent& event) {
  pollTimer_.Stop();
  DrainAndProcess();
  process_ = nullptr;

  if (!resultReceived_ && completeCallback_) {
    // Process exited without a result message — treat accumulated text as response
    FireComplete(true, fullResponse_, 0, 0);
  }

  event.Skip();
}

void ClaudeClient::DrainAndProcess() {
  if (!process_) return;

  wxInputStream* out = process_->GetInputStream();
  if (!out) return;

  // Read all remaining bytes
  char buf[4096];
  while (out->CanRead()) {
    out->Read(buf, sizeof(buf) - 1);
    size_t n = out->LastRead();
    if (n == 0) break;
    lineBuffer_.append(buf, n);
  }

  size_t pos;
  while ((pos = lineBuffer_.find('\n')) != std::string::npos) {
    std::string line = lineBuffer_.substr(0, pos);
    lineBuffer_.erase(0, pos + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) ProcessLine(line);
  }
  if (!lineBuffer_.empty()) {
    ProcessLine(lineBuffer_);
    lineBuffer_.clear();
  }
}

void ClaudeClient::ProcessLine(const std::string& line) {
  if (logCallback_) logCallback_("RECV", line);

  try {
    json j = json::parse(line);
    const std::string type = j.value("type", "");

    if (type == "system") {
      if (j.value("subtype", "") == "init") {
        std::string sid = j.value("session_id", "");
        if (!sid.empty()) {
          sessionId_ = sid;
          wxLogMessage("ClaudeClient: session_id=%s", sessionId_.c_str());
        }
      }

    } else if (type == "stream_event") {
      // Real-time streaming: extract text deltas from content_block_delta events
      if (j.contains("event")) {
        const auto& evt = j["event"];
        std::string evtType = evt.value("type", "");
        if (evtType == "content_block_delta" && evt.contains("delta")) {
          const auto& delta = evt["delta"];
          if (delta.value("type", "") == "text_delta") {
            std::string text = delta.value("text", "");
            if (!text.empty()) {
              fullResponse_ += text;
              if (chunkCallback_) chunkCallback_(text);
            }
          }
        }
      }

    } else if (type == "assistant") {
      // Final cumulative assistant message — update fullResponse_ but don't emit
      // (chunks were already emitted via stream_event deltas)
      std::string currentText;
      if (j.contains("message") && j["message"].contains("content")) {
        for (const auto& block : j["message"]["content"]) {
          if (block.value("type", "") == "text") {
            currentText += block.value("text", "");
          }
        }
      }
      if (!currentText.empty()) {
        fullResponse_ = currentText;
      }

    } else if (type == "result") {
      resultReceived_ = true;
      bool isError = j.value("is_error", false);
      std::string result = isError
        ? j.value("error", fullResponse_)
        : j.value("result", fullResponse_);

      int inputTok = 0, outputTok = 0;
      if (j.contains("usage")) {
        inputTok  = j["usage"].value("input_tokens", 0);
        outputTok = j["usage"].value("output_tokens", 0);
      }

      FireComplete(!isError, result, inputTok, outputTok);
    }

  } catch (const std::exception& e) {
    wxLogWarning("ClaudeClient: JSON parse error: %s — line: %s", e.what(), line.c_str());
  }
}

void ClaudeClient::FireComplete(bool success, const std::string& text, int inputTok, int outputTok) {
  if (!completeCallback_) return;
  auto cb = std::move(completeCallback_);
  completeCallback_ = nullptr;
  cb(success, text, inputTok, outputTok);
}

void ClaudeClient::Interrupt() {
  if (process_) {
    long pid = process_->GetPid();
    if (pid > 0) wxKill(pid, wxSIGINT);
  }
}

std::vector<ModelInfo> ClaudeClient::GetAvailableModels() {
  return {
    {"claude-opus-4-6",   "Claude Opus 4.6"},
    {"claude-sonnet-4-6", "Claude Sonnet 4.6"},
    {"claude-haiku-4-5",  "Claude Haiku 4.5"},
  };
}

} // namespace fcn::claude
