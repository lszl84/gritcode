#pragma once
#include <wx/wx.h>
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/bmpbuttn.h>
#include <wx/webrequest.h>
#include <nlohmann/json.hpp>
#include "chat_canvas.h"
#include "md_parser.h"
#include "mcp_server.h"
#include "session_store.h"
#include <memory>
#include <string>
#include <vector>

// Single model dropdown picks one of these. Order matches the choice items
// (OpenCode Free, DeepSeek V4 Flash, DeepSeek V4 Pro).
enum class ModelChoice {
    OpencodeFree = 0,
    DeepseekFlash = 1,
    DeepseekPro = 2,
};

class ChatFrame : public wxFrame {
public:
    ChatFrame();
    ~ChatFrame() override;

private:
    ChatCanvas* canvas_ = nullptr;
    wxTextCtrl* input_ = nullptr;
    wxButton* sendBtn_ = nullptr;

    // Toolbar row above the input. Plain wxBoxSizer with two labelled
    // dropdowns and a gear button on the right.
    wxChoice* sessionChoice_ = nullptr;
    wxChoice* modelChoice_ = nullptr;
    wxBitmapButton* settingsBtn_ = nullptr;

    // On-disk session persistence. Sessions are keyed by working directory
    // (one session per folder, gritcode model). The dropdown is rebuilt from
    // store_.List(); index 0 is the "New Session…" sentinel and indices 1..N
    // map to cwds in sessionCwds_ (parallel to the dropdown rows).
    SessionStore store_;
    std::vector<std::string> sessionCwds_;
    std::string activeCwd_;

    ModelChoice currentModel_ = ModelChoice::OpencodeFree;

    wxWebRequest request_;
    std::unique_ptr<MdStream> mdStream_;

    // Chat history as raw OpenAI-format messages. System prompt is the first
    // entry; user/assistant/tool messages are appended in order.
    std::vector<nlohmann::json> history_;
    std::string activeAssistantText_;
    std::string activeReasoning_;  // accumulated reasoning_content for the in-flight assistant turn
    bool streaming_ = false;

    // SSE parsing state. Doubles as raw-body capture: on a non-2xx response the
    // server sends a JSON error body instead of SSE, which lands here verbatim
    // and we display it in the error block.
    std::string sseBuf_;

    // Streaming tool_call accumulator. Each delta carries an `index` field that
    // identifies which call the (id/name/arguments) fragments belong to.
    struct StreamToolCall {
        std::string id;
        std::string name;
        std::string args;  // JSON string, accumulated across deltas
    };
    std::vector<StreamToolCall> activeToolCalls_;

    // Number of tool dispatch rounds taken in the current user turn. Capped
    // so the model can't loop forever on a single message.
    int toolIter_ = 0;

    // Graceful close — see OnClose.
    bool quitRequested_ = false;

    // TCP JSON-RPC server for programmatic control (port 8765+). Reads/writes
    // happen on a background thread; mutations bounce back to the GUI thread
    // via CallAfter, reads block via std::promise resolved from the GUI side.
    MCPServer mcp_;

    // Build the JSON snapshot the MCP getConversation method returns. Must be
    // called on the GUI thread.
    nlohmann::json BuildConversationSnapshot() const;
    nlohmann::json BuildBlocksSnapshot() const;

    void OnSend(wxCommandEvent&);
    void OnInputKey(wxKeyEvent&);
    void OnClose(wxCloseEvent&);
    void OnSessionChoice(wxCommandEvent&);
    void OnModelChoice(wxCommandEvent&);
    void OnSettings(wxCommandEvent&);

    // Repopulate the session choice from store_.List() with the leading
    // "New Session…" entry, then restore the active selection.
    void RefreshSessionChoice();

    // Save current state, then create or switch sessions. CreateNewSession
    // pops a directory dialog, then either loads existing history for that
    // cwd or seeds a fresh one. Switch loads from disk and rebuilds canvas.
    void CreateNewSession();
    void SwitchToCwd(const std::string& cwd);

    // Persist current history to disk under activeCwd_.
    void PersistActive();

    // Wipe blocks_ and re-render every renderable message in history_ —
    // user prompts as UserPrompt blocks, assistant content via MdStream, tool
    // call/result pairs as ToolCall blocks. System messages are skipped.
    void RestoreCanvasFromHistory();

    // Push the default system prompt into history_. Called on fresh sessions.
    void SeedSystemPrompt();

    // Re-render SVG icons with the current system foreground color. Called
    // once at construction and again on EVT_SYS_COLOUR_CHANGED so the icons
    // stay readable when the user toggles light/dark themes.
    void ReloadToolbarIcons();

    void OnWebRequestData(wxWebRequestEvent&);
    void OnWebRequestState(wxWebRequestEvent&);

    // After a non-2xx response: pull a human-readable error string out of
    // sseBuf_ (which doubles as raw-body capture under Storage_None).
    wxString ExtractErrorBody() const;

    void StartTurn(const wxString& userText);
    // Builds and sends the next chat completion request from the current
    // history_ vector. Used both to start a turn and to continue after tool
    // results have been appended.
    void StartCompletion();

    // After State_Completed: if any tool_calls were accumulated, dispatch them
    // and continue; otherwise finalize the turn.
    void HandleCompletion(const wxString& errorIfFailed);
    void FinalizeTurn();

    void RenderToolBlock(const std::string& name,
                         const std::string& argsJson,
                         const std::string& result);
    void RenderErrorBlock(const wxString& msg);
};
