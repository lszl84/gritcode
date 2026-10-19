#pragma once
#include <wx/wx.h>
#include <wx/choice.h>
#include <wx/textctrl.h>
#include <wx/treectrl.h>
#include <wx/bmpbuttn.h>
#include <wx/splitter.h>
#include <wx/dnd.h>
#include <wx/statbmp.h>
#include <wx/wrapsizer.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/fswatcher.h>
#include <chrono>
#include <nlohmann/json.hpp>
#include "chat_canvas.h"
#include "claude_agent.h"
#include "md_parser.h"
#include "mcp_server.h"
#include "memory.h"
#include "session_store.h"
#include "streaming_web_request.h"
#include "tools.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

class DebugWindow;

class ChatFrame : public wxFrame {
public:
    ChatFrame();
    ~ChatFrame() override;

    // Handle files dropped onto the window (adds supported images).
    void AddDroppedFiles(const wxArrayString& files);

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
    // queue is empty; rebuilt from the active thread's pendingQueue on every
    // queue change.
    wxPanel* chipRow_ = nullptr;
    wxSizer* chipSizer_ = nullptr;

    // Pending attached images (drag-and-dropped / pasted), shown as
    // deletable thumbnails above the input until the next send.
    struct PendingImage {
        std::string hash;
        std::string mime;
        std::string name;
        wxString path;  // sidecar blob path used to build the thumbnail
    };
    std::vector<PendingImage> pendingImages_;
    wxPanel* imageRow_ = nullptr;
    wxSizer* imageSizer_ = nullptr;

    // A queued user message plus any images attached to it.
    struct QueuedMessage {
        std::string text;
        std::vector<PendingImage> images;
    };

    static constexpr size_t kMaxQueue_ = 5;

    // Toolbar row above the input. Plain wxBoxSizer with two labelled
    // dropdowns and a gear button on the right.
    wxChoice* sessionChoice_ = nullptr;
    wxChoice* modelChoice_ = nullptr;
    // "Thread:" / "Model:" captions. These, the settings/export/debug
    // buttons and the model dropdown hide as the chat pane narrows (see
    // UpdateToolbarFit).
    wxStaticText* sessionLabel_ = nullptr;
    wxStaticText* modelLabel_ = nullptr;
    wxButton* debugBtn_ = nullptr;   // Debug builds only
    wxBoxSizer* toolbarRow_ = nullptr;
    wxBitmapButton* playBtn_ = nullptr;
    wxBitmapButton* settingsBtn_ = nullptr;
    wxBitmapButton* exportBtn_ = nullptr;
    wxBitmapButton* hamburgerBtn_ = nullptr;
    wxBitmapButton* editorBtn_ = nullptr;

    // Streaming tool_call accumulator. Each delta carries an `index` field that
    // identifies which call the (id/name/arguments) fragments belong to.
    struct StreamToolCall {
        std::string id;
        std::string name;
        std::string args;  // JSON string, accumulated across deltas
    };

    // Worker-thread tool dispatch result (one entry per tool call). Populated
    // off the GUI thread (or by a subagent); consumed in OnToolBatchDone.
    struct ToolBatchEntry {
        std::string id;
        std::string name;
        std::string argsJson;  // raw arguments string for display
        std::string result;
    };

    // One conversation. Several threads can run at once (one running turn per
    // folder); only the active one draws to the canvas, the others update
    // their data only.
    struct Thread {
        std::string id;        // session file id (SessionStore::Entry::id)
        std::string cwd;       // tools and Claude Code run here
        std::string title;     // from the first user message
        std::string parent;    // parent thread id, empty for top-level threads
        std::string lastUsed;  // ISO 8601, orders the dropdown
        // A thread with a parent is a subagent: a `task` call run headless, or
        // a Claude Code subagent shown from its parent's stream. It is
        // view-only. parentToolId is the parent's tool call it answers (known
        // while this run has it open); failure is why its turn failed, which
        // the parent gets as that call's result.
        std::string parentToolId;
        std::string failure;
        // Bumped whenever the thread's request, Claude process or tool worker
        // is replaced. Async callbacks capture (id, gen) and are dropped when
        // the gen no longer matches (see Find).
        uint32_t gen = 0;

        // Chat history as raw OpenAI-format messages. System prompt is the
        // first entry; user/assistant/tool messages are appended in order.
        std::vector<nlohmann::json> history;
        std::string lastAgentsContent;  // AGENTS.md text currently in history[0]

        // True from StartTurn until FinalizeTurn, including while `waiting`.
        bool streaming = false;
        // The turn is queued behind another thread running in the same folder
        // and starts when that one finishes (see StartNextWaiting).
        bool waiting = false;
        // Pending user messages typed/queued while a turn was in flight. On
        // natural turn end the front auto-dispatches; on error/cancel we land
        // in idle-queue mode.
        std::vector<QueuedMessage> pendingQueue;
        // The current turn's user text as typed (without hidden instructions).
        std::string turnPrompt;
        // A turn that failed while the thread wasn't shown: its notice, and its
        // prompt when the failure rolled that out of history. Switching to the
        // thread shows the notice and puts the prompt back into the input, so
        // a background failure never vanishes silently.
        std::optional<Block> failureNotice;
        std::string failedPrompt;

        // Model index the current turn started with. A switch to Claude in the
        // middle of an OpenAI-style tool loop can't take over that loop, so the
        // loop keeps this model until the turn ends.
        int turnModelIndex = 0;
        // Model id of the in-flight OpenAI-style completion, recorded on the
        // assistant messages it produces.
        std::string requestModel;

        StreamingWebRequest request;
        // Feeds streamed assistant text to the canvas while the thread is shown.
        std::unique_ptr<MdStream> mdStream;
        std::string activeAssistantText;  // in-flight text, not yet in history
        std::string activeReasoning;  // accumulated reasoning_content for the in-flight assistant turn
        // Per-completion-round flag. Reset in StartCompletion. Flipped true the
        // first time we emit a Thinking block for this round (either at the
        // first content/tool_call delta, or at HandleCompletion for a pure-
        // reasoning response). Keeps us from rendering the same reasoning twice
        // when a delta carries both reasoning and content.
        bool thinkingEmitted = false;
        // Canvas index of this round's Thinking block while reasoning is still
        // streaming (-1 when there is none, or the thread isn't shown). It is
        // added collapsed at the first reasoning delta; while expanded, its
        // text is refreshed a few times a second so the user can watch the
        // reasoning. EmitPendingThinking / FinalizeTurn finalize it.
        int liveThinkingIdx = -1;
        std::chrono::steady_clock::time_point liveThinkingLastUpdate{};
        // How long the last live re-layout took; paces the next one.
        std::chrono::steady_clock::duration liveThinkingLayoutCost{};
        // finish_reason of the in-flight completion ("stop", "length",
        // "tool_calls", ...) and the max_tokens it was sent with, so a reply that
        // hit the output limit can say so instead of ending silently.
        std::string finishReason;
        int requestMaxTokens = 0;

        // SSE parsing state. Doubles as raw-body capture: on a non-2xx response
        // the server sends a JSON error body instead of SSE, which lands here
        // verbatim and we display it in the error block.
        std::string sseBuf;
        std::vector<StreamToolCall> activeToolCalls;

        // Number of tool dispatch rounds taken in the current user turn.
        // Telemetry only — the loop ends when the model stops emitting tool calls.
        int toolIter = 0;

        // Shared with the tool worker thread, if any. Set when a batch starts
        // in HandleCompletion, cleared in OnToolBatchDone. Escape flips
        // `cancelled` and signals any in-flight bash subprocess via `activePgid`.
        std::shared_ptr<ToolCancelToken> currentToolToken;
        // Tool dispatch worker. Owned (not detached) so the destructor can
        // cancel and join it — otherwise a long-running bash invocation could
        // outlive the frame.
        std::thread toolWorker;
        bool toolWorkerRunning = false;
        // The current tool batch in call order. The worker fills in the
        // ordinary calls and each `task` call's child thread its own; the
        // batch ends (OnToolBatchDone) once the worker and the children are done.
        std::vector<ToolBatchEntry> toolBatch;

        // ---- Context compaction ----
        // historyCompactBaseCount: message count immediately after the last
        //   successful compaction. The hysteresis check (<5 growth since then)
        //   prevents firing a new compaction on every single turn — history
        //   only grows between requests, so without the gate we'd re-compact
        //   the same head every time.
        // compacting: true while a summary LLM call is in flight. Set by
        //   RunSummaryThenSend, cleared by ApplyCompaction. StartCompletion
        //   skips the budget check while this is true.
        int historyCompactBaseCount = 0;
        bool compacting = false;
        // Buffered state for the in-flight summary request.
        std::string summarySseBuf;
        std::string summaryText;
        int compactionSplitIdx = -1;
        int compactionHeadCount = 0;
        // Set once per turn after an overflow-forced compaction, so a second
        // provider overflow surfaces an error instead of looping.
        bool overflowRetried = false;

        // ---- Claude turns (the user's own Claude Code CLI) ----
        // A Claude turn runs `claude -p` with stream-json in and out. Claude
        // Code runs its own agent loop and tools; gritcode renders the events
        // and records them in history in the same OpenAI-style shape as other
        // models, tagged with "model" and "claudeSessionId" so the next turn
        // can --resume the same Claude Code session.
        ClaudeAgentProcess claudeProc;
        std::string claudeBuf;              // partial stdout line
        std::string claudeModel;            // model id the turn was sent to
        std::string claudeSessionId;        // from system/init of this run
        // Temp files for this run (--append-system-prompt-file, --mcp-config),
        // removed when it ends.
        std::vector<std::string> claudeTempFiles;
        bool claudeResumed = false;         // this run passed --resume
        bool claudeRetriedFresh = false;    // already retried without --resume
        bool claudeResultSeen = false;
        bool claudeIsError = false;
        std::string claudeErrorText;
        size_t claudeTurnHistoryStart = 0;  // history size after the user message
        // Top-level assistant message being assembled from per-block
        // "assistant" events (same API message id). Flushed into history
        // before its tool results, or when the next message starts.
        std::string claudeMsgId;
        nlohmann::json claudePendingMsg;
        std::string claudeStreamMsgId;               // message being streamed now
        std::set<std::string> claudeStreamedMsgIds;  // messages whose text streamed
        // tool_use id -> (name, compact JSON input), for rendering the result.
        std::map<std::string, std::pair<std::string, std::string>> claudeToolUses;
    };

    // Every thread opened in this run (std::deque: addresses stay stable as
    // threads are added). threads_[active_] is the one on the canvas.
    std::deque<Thread> threads_;
    size_t active_ = 0;
    Thread& Active() { return threads_[active_]; }
    const Thread& Active() const { return threads_[active_]; }
    bool Shown(const Thread& t) const { return &t == &threads_[active_]; }
    // Null when no open thread has this id (or, with `gen`, when the thread
    // has moved on since an async callback captured it).
    Thread* Find(const std::string& id);
    Thread* Find(const std::string& id, uint32_t gen);
    // Dropdown label: title, or a placeholder for untitled threads.
    static wxString ThreadLabel(const std::string& id, const std::string& cwd,
                                const std::string& title);
    // The label of thread `id`, open or not.
    wxString ThreadLabelOf(const std::string& id);

    // On-disk persistence: one session file per thread. The dropdown lists
    // the active folder's threads (rows map to ids in threadRowIds_), then
    // "New thread" and "Open folder…".
    SessionStore store_;
    std::vector<std::string> threadRowIds_;
    // Folder shown in the editor and file tree: the active thread's cwd.
    std::string activeCwd_;

    // Cross-project memory: FTS5 index over every saved thread. Opened at
    // construction; written after every Persist(); queried by the
    // grit_history_search tool. If the open fails (e.g. read-only home) the
    // tool returns "Memory index unavailable" — memory is an enhancement,
    // not required for normal chat.
    MemoryDB memory_;

    // Selected model index (not the dropdown row; see RouteForIndex). 0 is
    // Kilo Free, indices >= 1 map into remoteModels_ (the DeepSeek model
    // list), and the Claude entries have their own fixed indices.
    // remoteModels_ is the live GET /models result when available, otherwise
    // empty — and an empty list means the dropdown uses the hardcoded
    // DeepSeek fallback instead. The live list and fallback are never shown
    // together.
    int currentModelIndex_ = 0;
    std::vector<std::string> remoteModels_;

    // Separate thread for Play-button runs so a long-running dev server
    // doesn't block the tool dispatch worker (they share the same cancellation
    // pattern but must not contend for the same std::thread slot).
    std::shared_ptr<ToolCancelToken> currentPlayToken_;
    std::thread playWorker_;

    // Persist worker: runs the heavy SessionStore::WriteSessionFile +
    // MemoryDB::RebuildSession off the UI thread. Owned (not detached) so the
    // destructor joins it before the frame goes away.
    std::thread persistWorker_;

    // Remote DeepSeek model discovery (GET https://api.deepseek.com/models).
    // Owned (not detached) so the destructor can join it; results are applied
    // on the GUI thread via CallAfter.
    std::thread remoteModelsWorker_;
    WebCancelToken remoteModelsCancel_;

    // Graceful close — see OnClose.
    void RestoreSession();       // renders the active thread into the canvas + clears loading

    void RebuildImageRow();
    void RemovePendingImageByHash(const std::string& hash);
    bool TryPasteImage();
    static bool HistoryIsLarge(const Thread& t);  // >200 msgs or >250KB content
    void RestoreCanvasMaybeDeferred();     // loading placeholder + deferred restore when large
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

    // Pop the front of t.pendingQueue and dispatch it as a fresh turn.
    // No-op on empty. Used both from FinalizeTurn (auto-dispatch on
    // natural completion) and from the Continue button (idle-queue mode).
    void DispatchNextQueued(Thread& t);
    // Reflect the active thread's (streaming, queue) state across input/Send/
    // Continue/Clear/chip-row visibility and the Send button label.
    void UpdateQueueUI();
    // Recreate chip widgets to match the active thread's queue. Called from
    // UpdateQueueUI whenever the queue is non-empty.
    void RebuildChips();

    // Cancel whatever the thread has in flight: HTTP stream, tool batch, or
    // both, or a turn still waiting for its folder. Safe to call when nothing
    // is running. Wired to Escape.
    void RequestCancel(Thread& t);
    void OnSessionChoice(wxCommandEvent&);
    void OnModelChoice(wxCommandEvent&);
    void OnModelContextMenu(wxContextMenuEvent&);
    // Rebuild the model dropdown: Kilo Free plus the DeepSeek list
    // (live remoteModels_ when present, hardcoded fallback otherwise).
    void RebuildModelChoice();
    // Fetch the DeepSeek model catalog (GET /models) on a worker thread and
    // merge new model ids into the dropdown. No-op without an API key.
    void FetchRemoteModelsAsync();
    void OnRemoteModelsFetched(std::vector<std::string> models);
    void OnSettings(wxCommandEvent&);
    void OnCanvasLink(wxCommandEvent& e);
    void OnPlay(wxCommandEvent&);
    void OnExport(wxCommandEvent&);
    void OnHamburger(wxCommandEvent&);
    void OnEditorToggle(wxCommandEvent&);
    void OnImport(wxCommandEvent&);
    void ShowImportDialog();
    // Grow/shrink the frame so the chat pane keeps its width as side panels
    // Resize the frame so the chat pane keeps `centerW` pixels regardless of
    // which side panels are open, and keep the minimum width in sync. When
    // `tiled` (Hyprland tiling), the window keeps its size and
    // LayoutTiledPanes shares it between the panes instead.
    void SyncPanelSizing(int centerW, bool tiled = false);
    void LayoutTiledPanes();
    // Hide toolbar items the chat pane has no room for, least important first.
    void UpdateToolbarFit();
    void OnInnerSashChanging(wxSplitterEvent& e);
    // Project file tree in the right editor panel.
    void PopulateEditorTree();
    void PopulateTreeDir(wxTreeItemId parent, const wxString& path);
    // Reload the tree from disk, restoring expanded directories and the
    // selection where those paths still exist.
    void ReloadTreeKeepExpanded();
    void CollectExpandedPaths(wxTreeItemId parent, std::vector<wxString>& out);
    wxString GetSelectedTreePath() const;
    void ExpandPathTo(const wxString& path, std::set<wxString>& populated);
    // Watch activeCwd for filesystem changes and refresh the tree (debounced).
    void SetupFsWatcher();
    void RescanFsWatcher();
    void CollectExpandedDirs(wxTreeItemId parent, int depth,
                             std::vector<wxString>& out);
    void OnFsWatcherEvent(wxFileSystemWatcherEvent& e);
    void OnTreeRefreshTimer(wxTimerEvent& e);
    void OnTreeRefresh(wxCommandEvent& e);
    void OnTreeToggleHidden(wxCommandEvent& e);
    void OnEditorTreeExpanding(wxTreeEvent& e);
    void OnEditorTreeCollapsed(wxTreeEvent& e);
    void OnEditorTreeSelect(wxTreeEvent& e);
    void OnEditorTreeContextMenu(wxContextMenuEvent& e);
    void ShowTreeContextMenu(wxTreeItemId item);
    void TreeCtxTarget(wxString& dir, wxTreeItemId& parentItem);
    void OnTreeNewFile(wxCommandEvent& e);
    void OnTreeNewFolder(wxCommandEvent& e);
    void OnTreeRename(wxCommandEvent& e);
    void OnTreeShowInFiles(wxCommandEvent& e);
    void ShowFileInManager(const wxString& path);
    void LoadFileIntoEditor(const wxString& path);
    // Record the open file's mtime+size so a later stat can tell whether it
    // changed on disk (agent/external edit) independently of our own saves.
    void RecordEditorFileStamp();
    // If the open file changed on disk: prompt when there are unsaved edits,
    // otherwise silently reload.
    void CheckEditorFileChangedOnDisk();
    bool SaveEditorFile();
    bool SaveEditorFileAs();
    bool WriteEditorFile(const wxString& path);
    void ReloadEditorFile();
    void CloseEditorFile();
    void ClearEditorState();
    bool MaybeSaveEditor();
    void UpdateWindowTitle();
    void OnEditorTextChanged(wxCommandEvent& e);
    void OnEditorChar(wxKeyEvent& e);
    void OnEditorContextMenu(wxContextMenuEvent& e);
    void OnHighlightTimer(wxTimerEvent& e);
    void OnEditorSave(wxCommandEvent& e);
    void OnEditorSaveAs(wxCommandEvent& e);
    void OnEditorReload(wxCommandEvent& e);
    void OnEditorCloseFile(wxCommandEvent& e);
    void OnEditorShowInFiles(wxCommandEvent& e);
    wxTreeItemId FindTreeItemByPath(wxTreeItemId parent, const wxString& path);

    // Repopulate the thread dropdown: the active folder's threads, most
    // recent first (running ones marked), then "New thread" and
    // "Open folder…", and select the active thread.
    void RefreshSessionChoice();

    // CreateNewSession pops a directory dialog and switches to that folder.
    // SwitchToCwd opens the most recently used thread of `cwd` (loading it,
    // or seeding a fresh one). NewThread adds an empty thread in `cwd`.
    // SelectThread shows thread `id`, loading it from disk when needed; false
    // when it doesn't exist or the user cancelled (unsaved editor changes).
    void CreateNewSession();
    bool SwitchToCwd(const std::string& cwd);
    bool NewThread(const std::string& cwd);
    bool SelectThread(const std::string& id);
    // Adds an open thread for (id, cwd): loaded from disk when it has a
    // session file, otherwise seeded with a fresh system prompt.
    Thread& OpenThread(const std::string& id, const std::string& cwd);
    // Make open thread `id` the active one and rebuild the canvas from it.
    // False when the user cancelled (unsaved editor changes).
    bool ShowThread(const std::string& id);

    // Persist the thread's history to its session file.
    void Persist(Thread& t);

    // Re-render every renderable message in the active thread's history —
    // user prompts as UserPrompt blocks, assistant content via MdStream, tool
    // call/result pairs as ToolCall blocks; system messages are skipped —
    // followed by the in-flight turn (partial text, waiting notice).
    void RestoreCanvasFromHistory();

    // Push the default system prompt into t.history. Called on fresh threads.
    void SeedSystemPrompt(Thread& t);
    // Re-inject AGENTS.md instructions into t.history[0] for an existing
    // (restored or switched-to) thread whose system prompt predates them.
    void RefreshSystemPromptAgents(Thread& t);

    // Re-render SVG icons with the current system foreground color. Called
    // once at construction and again on EVT_SYS_COLOUR_CHANGED so the icons
    // stay readable when the user toggles light/dark themes.
    void ReloadToolbarIcons();
    // Recolour everything from the current system / Omarchy theme.
    void ApplyTheme();
    // Watch Omarchy's current-theme dir so a theme switch recolours live.
    void StartThemeWatcher();

    // ---- Session import/export ----
    std::vector<nlohmann::json> importedMessages_;   // raw messages from import
    wxPanel* importPanel_ = nullptr;                  // left pane of splitter
    ChatCanvas* importCanvas_ = nullptr;              // rendered import messages
    wxPanel* importEmptyView_ = nullptr;                // empty state with Load button
    wxSplitterWindow* splitter_ = nullptr;            // main splitter
    wxPanel* mainPanel_ = nullptr;                     // right pane (main UI)
    wxStaticText* refLabel_ = nullptr;                 // "Referenced Session: ..." at bottom
    wxButton* changeBtn_ = nullptr;                      // "Load another…" button
    wxString importedFileName_;                          // display name of imported file

    // ---- Editor panel (right) ----
    wxSplitterWindow* innerSplitter_ = nullptr;          // main | editor splitter
    wxPanel* editorPanel_ = nullptr;                     // right pane of inner splitter
    wxTreeCtrl* fileTree_ = nullptr;                     // project file tree
    wxTextCtrl* codeEdit_ = nullptr;                     // editable file content
    wxString editorFilePath_;                            // file open in codeEdit_
    bool editorDirty_ = false;                           // unsaved changes in editor
    bool suppressReloadPrompt_ = false;                  // show "changed on disk" once
    wxDateTime lastFileMtime_;                           // on-disk stamp (change detect)
    wxULongLong lastFileSize_;                           // on-disk size (change detect)
    bool treeSelectionRestoring_ = false;                // suppress SEL_CHANGED during reload
    wxTimer* highlightTimer_ = nullptr;                  // debounced syntax highlight
    wxFileSystemWatcher* fileWatcher_ = nullptr;         // watches activeCwd for changes
    // Omarchy theme watcher. Its events go to a separate handler so they
    // never reach the project watcher's OnFsWatcherEvent; the timer
    // coalesces the burst of writes a theme switch makes. The handler is
    // declared first so it outlives the timer.
    wxEvtHandler themeEvents_;
    wxTimer themeTimer_;
    wxFileSystemWatcher* themeWatcher_ = nullptr;
    wxTimer* treeRefreshTimer_ = nullptr;                // debounces watcher bursts
    wxTreeItemId treeCtxItem_;                           // right-clicked tree item
    wxString treeCtxPath_;                               // right-clicked item path
    bool treeCtxIsDir_ = true;                           // right-clicked item is a dir
    bool showHidden_ = false;                            // show dotfiles in the file tree
    int editorPaneW_ = 0;                                // editor width (user-adjustable)
    int imgFolder_ = -1;                                 // tree icon indices
    int imgFile_ = -1;

    // Streaming HTTP callbacks (delivered on the GUI thread via CallAfter).
    // OnStreamData appends to t.sseBuf and parses any complete SSE events.
    // OnStreamDone fires once with the final WebResponse and runs the same
    // post-stream finalization as the old OnWebRequestState handler did.
    void OnStreamData(Thread& t, std::string_view chunk);
    void OnStreamDone(Thread& t, WebResponse resp);

    // Ends t's tool batch once its worker and subagents are done: records
    // the results and continues the turn.
    void OnToolBatchDone(Thread& t);

    // ---- Subagents (child threads, one level deep) ----
    // Open child of `t` answering tool call `toolId`, if any.
    Thread* ChildFor(const Thread& t, const std::string& toolId);
    bool HasRunningChild(const Thread& t) const;
    // Adds a child thread of `parent` for tool call `toolId`, titled by
    // `description`, and links to it from the parent's canvas.
    Thread& OpenChild(Thread& parent, const std::string& toolId,
                      const std::string& description);
    // Runs a `task` call: a child thread with a fresh context does `prompt`.
    void StartSubagent(Thread& parent, const std::string& toolId,
                       const std::string& description, const std::string& prompt);
    // A finished `task` child: its report (or failure) becomes the result of
    // its parent's call.
    void ReportToParent(Thread& child, bool failed);
    // A Claude Code subagent (Agent tool call) gets a child thread that shows
    // its events; FinishClaudeSubagent ends it, with `failure` when it failed.
    Thread& StartClaudeSubagent(Thread& parent, const std::string& toolId,
                                const nlohmann::json& input);
    void FinishClaudeSubagent(Thread& child, const std::string& failure);
    // Model index thread t's requests use: the dropdown's, except that a
    // subagent keeps the model it was started with.
    int ModelIndexFor(const Thread& t) const;

    // After a non-2xx response: pull a human-readable error string out of
    // t.sseBuf (which doubles as raw-body capture under Storage_None).
    static wxString ExtractErrorBody(const Thread& t);

    // Starts a model turn for a user message (the only entry point, so every
    // turn is routed the same way). `hiddenInstructions` is appended to the
    // message the model sees but not shown on the canvas. While another
    // thread runs a turn in the same folder, the turn waits for it.
    void StartTurn(Thread& t, const wxString& userText,
                   std::vector<PendingImage> images = {},
                   const std::string& hiddenInstructions = std::string());
    // The thread (other than `self`) running a turn in `cwd`, if any.
    Thread* RunningIn(const std::string& cwd, const Thread& self);
    // Start the first turn waiting for `cwd`, unless one is running there.
    void StartNextWaiting(const std::string& cwd);
    void EnqueueMessage(Thread& t, const wxString& text);
    void LogDebug(const std::string& line);
    void OpenDebugWindow();
    nlohmann::json BuildModelView(const Thread& t) const;
    void PruneToolOutputs(nlohmann::json& tail) const;
    void EmitImageBlock(Thread& t, const std::string& hash, const std::string& mime,
                        const std::string& name);
    Block MakeImageBlock(const std::string& hash, const std::string& mime, const std::string& name);
    // Markdown stream for the thread's in-flight reply: its blocks go to the
    // canvas while the thread is shown and are dropped otherwise.
    std::unique_ptr<MdStream> MakeMdStream(Thread& t);
    // Builds and sends the next chat completion request from t.history.
    // Used both to start a turn and to continue after tool results have
    // been appended.
    void StartCompletion(Thread& t);
    // Build + dispatch the actual chat completion request. Split out of
    // StartCompletion so the compaction path can chain into it directly
    // after the summary returns without re-checking the budget.
    void DoSendActualRequest(Thread& t);

    // Spawn the CLI for the user message at the end of t.history. `fresh`
    // skips --resume (used after Claude Code reports the session is gone).
    void StartClaudeTurn(Thread& t, bool fresh = false);
    void OnClaudeData(Thread& t, std::string_view chunk);
    void OnClaudeDone(Thread& t, ClaudeProcessResult res);
    void HandleClaudeEvent(Thread& t, const nlohmann::json& ev);
    void FlushClaudePendingMessage(Thread& t);
    // Show a Claude Code failure (with a sign-in hint for auth errors), drop
    // the user message if the turn recorded nothing, and end the turn.
    void FailClaudeTurn(Thread& t, const std::string& detail);
    // Italic notice when a turn goes to a different model than the last one.
    void RenderModelSwitchNotice(Thread& t, const std::string& modelId);

    // Debug log (debug builds only).
    DebugWindow* debugWindow_ = nullptr;
    std::string debugBuffer_;

    // Returns true if a summary request was kicked off (caller should
    // return immediately); false if history fits and the caller should
    // proceed to send the normal request.
    bool MaybeCompactThenSend(Thread& t);
    // Index into t.history marking the start of the verbatim tail (the most
    // recent ~kTailBudgetTokens of non-summary, non-system, non-compacted
    // messages). Messages before this index are the head to summarize.
    static int SelectTailSplit(const Thread& t);
    // Fires the summary stream request. The head [0..splitIdx) of
    // t.history is summarized into a single user-role message.
    void RunSummaryThenSend(Thread& t, int splitIdx);
    // Summary stream callbacks (separate from the main OnStreamData /
    // OnStreamDone — the summary response is private to the compaction
    // path and never lands in canvas blocks).
    void OnSummaryStreamData(Thread& t, std::string_view chunk);
    void OnSummaryStreamDone(Thread& t, WebResponse resp);
    // Replace t.history[0..splitIdx) with a single isSummary user message
    // carrying `summary`, persist, then resume the real request. Falls
    // back to a generic "context dropped" notice if `success` is false so
    // the next request still fits.
    void ApplyCompaction(Thread& t, bool success, const std::string& summary,
                         const std::string& error);
    // Layer 3: aggressive compaction after a provider context-overflow 400.
    // Keeps only the most recent user turn verbatim. Returns false if there
    // is nothing to compact.
    bool ForceCompactForOverflow(Thread& t);

    // After State_Completed: if any tool_calls were accumulated, dispatch them
    // and continue; otherwise finalize the turn. freeModelStall renders the
    // friendly "free model stopped responding" notice instead of a generic
    // error (same history rollback + finalize path).
    void HandleCompletion(Thread& t, const wxString& errorIfFailed,
                          bool freeModelStall = false);
    // wasCancelledOrError=true (any failure path) lands us in idle-queue
    // mode if the queue is non-empty so the user decides whether to
    // Continue. Default false (natural completion) auto-dispatches.
    void FinalizeTurn(Thread& t, bool wasCancelledOrError = false);

    // Canvas output for thread `t`. These draw only while `t` is shown, so a
    // background thread's turn updates its data and nothing else.
    // `childId`: the subagent thread that ran the call, linked below it.
    void RenderToolBlock(Thread& t, const std::string& name,
                         const std::string& argsJson,
                         const std::string& result,
                         const std::string& childId = std::string());
    void RenderErrorBlock(Thread& t, const wxString& msg);
    // "Waiting for <thread> to finish" while t.waiting.
    void RenderWaitingNotice(Thread& t);
    // A failed turn's notice: drawn now when `t` is shown, otherwise kept on
    // the thread (with the prompt when it was rolled back) until viewed.
    void ShowTurnFailure(Thread& t, Block notice, bool promptRolledBack);
    // Inline notice for when the no-key free model stalls: explains it's the
    // free tier, recommends DeepSeek, and links to Settings.
    static Block FreeModelStallNotice();
    // Append a thinking block to the canvas. Always starts collapsed.
    // Empty text is a no-op (a model can advertise reasoning_content but
    // send nothing).
    void RenderThinkingBlock(Thread& t, const wxString& text);
    // Emit the pending thinking block (if any) for the current completion
    // round and set t.thinkingEmitted true. Idempotent.
    void EmitPendingThinking(Thread& t);
    // Called as reasoning deltas arrive: adds the collapsed live Thinking
    // block on the first one, then (throttled, and only while the user has it
    // expanded) refreshes its text. No-op once the round's thinking is emitted.
    void UpdateLiveThinking(Thread& t);
};
