#include "claude_provider.h"
#include <wx/wx.h>
#include <wx/log.h>
#include <nlohmann/json.hpp>

namespace fcn {

using json = nlohmann::json;

static constexpr int kPollIntervalMs = 10;

ClaudeProvider::ClaudeProvider() : pollTimer_(this) {
  Bind(wxEVT_TIMER, &ClaudeProvider::OnPollTimer, this, pollTimer_.GetId());
}

ClaudeProvider::~ClaudeProvider() {
  Shutdown();
}

bool ClaudeProvider::Initialize() {
  return true;
}

void ClaudeProvider::Shutdown() {
  pollTimer_.Stop();
  if (process_) {
    long pid = process_->GetPid();
    if (pid > 0) wxKill(pid, wxSIGKILL);
    process_ = nullptr;
  }
}

void ClaudeProvider::FetchModels(
    std::function<void(const std::vector<ProviderModelInfo>&)> callback) {
  std::vector<ProviderModelInfo> models = {
    {"claude-opus-4-6",   "Claude Opus 4.6"},
    {"claude-sonnet-4-6", "Claude Sonnet 4.6"},
    {"claude-haiku-4-5",  "Claude Haiku 4.5"},
  };
  callback(models);
}

void ClaudeProvider::SendMessage(
    const std::string& model,
    const std::string& message,
    const std::vector<ChatMessage>& history,
    std::function<void(const std::string& chunk, bool isThinking)> onChunk,
    std::function<void(bool success, const std::string& content,
                       const std::string& error,
                       const std::vector<ToolCall>& toolCalls,
                       int inputTokens, int outputTokens)> onComplete) {

  if (process_) {
    wxLogWarning("ClaudeProvider: already running, interrupting previous process");
    Interrupt();
  }

  chunkCallback_ = std::move(onChunk);
  completeCallback_ = [onComplete](bool s, const std::string& c,
                                    const std::string& e, const std::vector<ToolCall>& tc,
                                    int i, int o) {
    onComplete(s, c, e, tc, i, o);
  };
  fullResponse_.clear();
  lineBuffer_.clear();
  resultReceived_ = false;
  inThinkingBlock_ = false;

  // Build prompt: include conversation history so context survives
  // cross-provider switches. history includes the current user message
  // as the last element. The |message| param may be empty (tool loop),
  // so always use the last history entry as the actual user message.
  std::string currentMessage = message;
  if (currentMessage.empty() && !history.empty() && history.back().role == "user") {
    currentMessage = history.back().content;
  }

  std::string prompt;
  if (history.size() > 1) {
    prompt = "<conversation_history>\n";
    for (size_t i = 0; i < history.size() - 1; ++i) {
      const auto& msg = history[i];
      prompt += "<" + msg.role + ">\n" + msg.content + "\n</" + msg.role + ">\n";
    }
    prompt += "</conversation_history>\n\n";
  }
  prompt += currentMessage;

  // Build command — no --resume; full context is in the prompt
  wxString cmd = "claude --print --verbose --output-format stream-json --include-partial-messages";
  if (!model.empty() && model != "claude") {
    cmd += " --model " + wxString::FromUTF8(model);
  }

  if (logCallback_) {
    logCallback_("CMD", cmd.ToStdString());
    logCallback_("SEND", prompt);
  }

  process_ = new wxProcess(this);
  process_->Redirect();
  process_->Bind(wxEVT_END_PROCESS, &ClaudeProvider::OnProcessEnd, this);

  long pid = wxExecute(cmd, wxEXEC_ASYNC | wxEXEC_MAKE_GROUP_LEADER, process_);
  if (pid == 0) {
    wxLogError("ClaudeProvider: failed to spawn claude process");
    process_ = nullptr;
    if (completeCallback_) {
      auto cb = std::move(completeCallback_);
      completeCallback_ = nullptr;
      cb(false, "", "Failed to spawn claude binary — is it installed?", {}, 0, 0);
    }
    return;
  }

  // Write prompt to stdin, then close
  wxOutputStream* stdinStream = process_->GetOutputStream();
  if (stdinStream) {
    stdinStream->Write(prompt.c_str(), prompt.length());
    process_->CloseOutput();
  }

  pollTimer_.Start(kPollIntervalMs);
}

void ClaudeProvider::OnPollTimer(wxTimerEvent&) {
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

void ClaudeProvider::OnProcessEnd(wxProcessEvent& event) {
  pollTimer_.Stop();
  DrainAndProcess();
  process_ = nullptr;

  if (!resultReceived_ && completeCallback_) {
    FireComplete(true, fullResponse_, "", 0, 0);
  }

  event.Skip();
}

void ClaudeProvider::DrainAndProcess() {
  if (!process_) return;

  wxInputStream* out = process_->GetInputStream();
  if (!out) return;

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

void ClaudeProvider::ProcessLine(const std::string& line) {
  if (logCallback_) logCallback_("RECV", line);

  try {
    json j = json::parse(line);
    const std::string type = j.value("type", "");

    if (type == "system") {
      // init messages ignored — we don't use --resume

    } else if (type == "stream_event") {
      if (!j.contains("event")) return;
      const auto& evt = j["event"];
      std::string evtType = evt.value("type", "");

      if (evtType == "content_block_start" && evt.contains("content_block")) {
        std::string blockType = evt["content_block"].value("type", "");
        inThinkingBlock_ = (blockType == "thinking");

      } else if (evtType == "content_block_stop") {
        inThinkingBlock_ = false;

      } else if (evtType == "content_block_delta" && evt.contains("delta")) {
        const auto& delta = evt["delta"];
        std::string deltaType = delta.value("type", "");

        if (deltaType == "text_delta") {
          std::string text = delta.value("text", "");
          if (!text.empty()) {
            fullResponse_ += text;
            if (chunkCallback_) chunkCallback_(text, false);
          }
        } else if (deltaType == "thinking_delta") {
          std::string thinking = delta.value("thinking", "");
          if (!thinking.empty()) {
            if (chunkCallback_) chunkCallback_(thinking, true);
          }
        }
      }

    } else if (type == "assistant") {
      // Final cumulative assistant message
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
      std::string result;
      if (isError) {
        result = j.value("error", fullResponse_);
      } else {
        result = j.value("result", "");
        if (result.empty()) result = fullResponse_;
      }

      int inputTok = 0, outputTok = 0;
      if (j.contains("usage")) {
        inputTok  = j["usage"].value("input_tokens", 0);
        outputTok = j["usage"].value("output_tokens", 0);
      }

      FireComplete(!isError, result, isError ? result : "", inputTok, outputTok);
    }

  } catch (const std::exception& e) {
    wxLogWarning("ClaudeProvider: JSON parse error: %s — line: %.100s", e.what(), line.c_str());
  }
}

void ClaudeProvider::FireComplete(bool success, const std::string& text,
                                   const std::string& error, int inputTok, int outputTok) {
  if (!completeCallback_) return;
  auto cb = std::move(completeCallback_);
  completeCallback_ = nullptr;
  cb(success, text, error, {}, inputTok, outputTok);
}

void ClaudeProvider::Interrupt() {
  if (process_) {
    long pid = process_->GetPid();
    if (pid > 0) wxKill(pid, wxSIGINT);
  }
}

void ClaudeProvider::SetJsonLogCallback(JsonLogCallback callback) {
  logCallback_ = std::move(callback);
}

std::unique_ptr<ChatProvider> CreateClaudeProvider() {
  return std::make_unique<ClaudeProvider>();
}

} // namespace fcn
