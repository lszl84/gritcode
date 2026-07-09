#pragma once
#include <wx/wx.h>
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/bmpbuttn.h>
#include <wx/thread.h>
#include <nlohmann/json.hpp>
#include "chat_canvas.h"
#include "md_parser.h"
#include "mcp_server.h"
#include "memory.h"
#include "session_store.h"
#include "streaming_web_request.h"
#include "tools.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
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

    // Queue-mode buttons. Visible only when the agent is idle AND the
    // pending queue is non-empty (idle-queue mode), where they replace the
    // input + Send row. Continue dispatches the next queued message; Clear
    // discards the queue and restores the normal input row.
    wxButton* continueQueueBtn_ = nullptr;
    wxButton* clearQueueBtn_ = nullptr;

    // Chip row sitting between the canvas and the input. Hidden when the
    // queue is empty; rebuilt from pendingQueue_ on every queue change.
    wxPanel* chipRow_ = nullptr;
    wxSizer* chipSizer_ = nullptr;

    // Pending user messages typed/queued while a turn was in flight. On
    // natural turn end the front auto-dispatches; on error/cancel we land
    // in idle-queue mode.
    std::vector<std::string> pendingQueue_;
    static constexpr size_t kMaxQueue_ = 5;

    // Toolbar row above the input. Plain wxBoxSizer with two labelled
    // dropdowns and a gear button on the right.
    wxChoice* sessionChoice_ = nullptr;
    wxChoice* modelChoice_ = nullptr;
    wxBitmapButton* playBtn_ = nullptr;
    wxBitmapButton* settingsBtn_ = nullptr;

    // On-disk session persistence. Sessions are keyed by working directory
    // (one session per folder, gritcode model). The dropdown is rebuilt from
    // store_.List(); index 0 is the "New Session…" sentinel and indices 1..N
    // map to cwds in sessionCwds_ (parallel to the dropdown rows).
    SessionStore store_;
    std::vector<std::string> sessionCwds_;
    std::string activeCwd_;

    // Cross-project memory: FTS5 index over every saved session. Opened at
    // construction; written after every PersistActive(); queried by the
    // grit_history_search tool. If the open fails (e.g. read-only home) the
    // tool returns "Memory index unavailable" — memory is an enhancement,
    // not required for normal chat.
    MemoryDB memory_;

    ModelChoice currentModel_ = ModelChoice::OpencodeFree;

    StreamingWebRequest request_;
    std::unique_ptr<MdStream> mdStream_;

    // Chat history as raw OpenAI-format messages. System prompt is the first
    // entry; user/assistant/tool messages are appended in order.
    std::vector<nlohmann::json> history_;
    std::string activeAssistantText_;
    std::string activeReasoning_;  // accumulated reasoning_content for the in-flight assistant turn
    // Per-completion-round flag. Reset in StartCompletion. Flipped true the
    // first time we emit a Thinking block for this round (either at the
    // first content/tool_call delta, or at HandleCompletion for a pure-
    // reasoning response). Keeps us from rendering the same reasoning twice
    // when a delta carries both reasoning and content.
    bool thinkingEmitted_ = false;
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

    // Number of tool dispatch rounds taken in the current user turn.
    // Telemetry only — the loop ends when the model stops emitting tool calls.
    int toolIter_ = 0;

    // Shared with the active tool worker thread, if any. Set when a batch
    // starts in HandleCompletion, cleared in OnToolBatchDone. Escape flips
    // `cancelled` and signals any in-flight bash subprocess via `activePgid`.
    std::shared_ptr<ToolCancelToken> currentToolToken_;

    // Tool dispatch worker. Owned (not detached) so the destructor can cancel
    // and join it — otherwise a long-running bash invocation could outlive the
    // frame and call wxQueueEvent on a dangling `this`.
    std::thread toolWorker_;

    // Separate thread for Play-button runs so a long-running dev server
    // doesn't block the tool dispatch worker (they share the same cancellation
    // pattern but must not contend for the same std::thread slot).
    std::shared_ptr<ToolCancelToken> currentPlayToken_;
    std::thread playWorker_;

    // Graceful close — see OnClose.
    bool quitRequested_ = false;

    // Set in ~ChatFrame before stopping MCP. Read by guiSync (on the MCP
    // thread) so it can break out of its future.get() wait when the GUI
    // thread is no longer pumping events. Without this, a CallAfter posted
    // by guiSync would never fire during destruction and mcp_.Stop() would
    // deadlock joining the MCP thread.
    std::atomic<bool> destroying_{false};

    // TCP JSON-RPC server for programmatic control (port 8765+). Reads/writes
    // happen on a background thread; mutations bounce back to the GUI thread
    // via CallAfter, reads block via std::promise resolved from the GUI side.
    MCPServer mcp_;

    // Build the JSON snapshot the MCP getConversation method returns. Must be
    // called on the GUI thread.
    nlohmann::json BuildConversationSnapshot() const;
    nlohmann::json BuildBlocksSnapshot() const;

    void OnSend(wxCommandEvent&);
    void OnContinueQueue(wxCommandEvent&);
    void OnClearQueue(wxCommandEvent&);
    void OnInputKey(wxKeyEvent&);
    void OnCharHook(wxKeyEvent&);
    void OnClose(wxCloseEvent&);

    // Pop the front of pendingQueue_ and dispatch it as a fresh turn.
    // No-op on empty. Used both from FinalizeTurn (auto-dispatch on
    // natural completion) and from the Continue button (idle-queue mode).
    void DispatchNextQueued();
    // Reflect (streaming, queue) state across input/Send/Continue/Clear/
    // chip-row visibility and the Send button label.
    void UpdateQueueUI();
    // Recreate chip widgets to match pendingQueue_. Called from
    // UpdateQueueUI whenever the queue is non-empty.
    void RebuildChips();

    // Cancel whatever is currently in flight: HTTP stream, tool batch, or
    // both. Safe to call when nothing is running. Wired to Escape.
    void RequestCancel();
    void OnSessionChoice(wxCommandEvent&);
    void OnModelChoice(wxCommandEvent&);
    void OnSettings(wxCommandEvent&);
    void OnPlay(wxCommandEvent&);

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

    // Streaming HTTP callbacks (delivered on the GUI thread via CallAfter).
    // OnStreamData appends to sseBuf_ and parses any complete SSE events.
    // OnStreamDone fires once with the final WebResponse and runs the same
    // post-stream finalization as the old OnWebRequestState handler did.
    void OnStreamData(std::string_view chunk);
    void OnStreamDone(WebResponse resp);

    // Worker-thread tool dispatch result (one entry per tool call). Populated
    // off the GUI thread; consumed in OnToolBatchDone.
    struct ToolBatchEntry {
        std::string id;
        std::string name;
        std::string argsJson;  // raw arguments string for display
        std::string result;
    };
    void OnToolBatchDone(wxThreadEvent& e);

    // After a non-2xx response: pull a human-readable error string out of
    // sseBuf_ (which doubles as raw-body capture under Storage_None).
    wxString ExtractErrorBody() const;

    void StartTurn(const wxString& userText);
    // Builds and sends the next chat completion request from the current
    // history_ vector. Used both to start a turn and to continue after tool
    // results have been appended.
    void StartCompletion();
    // Build + dispatch the actual chat completion request. Split out of
    // StartCompletion so the compaction path can chain into it directly
    // after the summary returns without re-checking the budget.
    void DoSendActualRequest();

    // ---- Context compaction ----
    // historyCompactBaseCount_: message count immediately after the last
    //   successful compaction. The hysteresis check (<5 growth since then)
    //   prevents firing a new compaction on every single turn — history
    //   only grows between requests, so without the gate we'd re-compact
    //   the same head every time.
    // compacting_: true while a summary LLM call is in flight. Set by
    //   RunSummaryThenSend, cleared by ApplyCompaction. StartCompletion
    //   skips the budget check while this is true.
    int historyCompactBaseCount_ = 0;
    bool compacting_ = false;
    // Buffered state for the in-flight summary request.
    std::string summarySseBuf_;
    std::string summaryText_;
    int compactionSplitIdx_ = -1;
    int compactionHeadCount_ = 0;

    // Returns true if a summary request was kicked off (caller should
    // return immediately); false if history fits and the caller should
    // proceed to send the normal request.
    bool MaybeCompactThenSend();
    // Fires the summary stream request. The head [0..splitIdx) of
    // history_ is summarized into a single user-role message.
    void RunSummaryThenSend(int splitIdx);
    // Summary stream callbacks (separate from the main OnStreamData /
    // OnStreamDone — the summary response is private to the compaction
    // path and never lands in canvas blocks).
    void OnSummaryStreamData(std::string_view chunk);
    void OnSummaryStreamDone(WebResponse resp);
    // Replace history_[0..splitIdx) with a single isSummary user message
    // carrying `summary`, persist, then resume the real request. Falls
    // back to a generic "context dropped" notice if `success` is false so
    // the next request still fits.
    void ApplyCompaction(bool success, const std::string& summary,
                         const std::string& error);

    // After State_Completed: if any tool_calls were accumulated, dispatch them
    // and continue; otherwise finalize the turn.
    void HandleCompletion(const wxString& errorIfFailed);
    // wasCancelledOrError=true (any failure path) lands us in idle-queue
    // mode if the queue is non-empty so the user decides whether to
    // Continue. Default false (natural completion) auto-dispatches.
    void FinalizeTurn(bool wasCancelledOrError = false);

    void RenderToolBlock(const std::string& name,
                         const std::string& argsJson,
                         const std::string& result);
    void RenderErrorBlock(const wxString& msg);
    // Append a thinking block to the canvas. Always starts collapsed.
    // Empty text is a no-op (a model can advertise reasoning_content but
    // send nothing).
    void RenderThinkingBlock(const wxString& text);
    // Emit the pending thinking block (if any) for the current completion
    // round and set thinkingEmitted_ true. Idempotent.
    void EmitPendingThinking();
};
