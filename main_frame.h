#pragma once

#include <wx/wx.h>
#include <wx/textctrl.h>
#include "zen_client.h"
#include "mcp_server.h"

class StreamingTextCtrl;

namespace fcn::ui {

class MainFrame : public wxFrame {
public:
  MainFrame();
  
private:
  void CreateMenuBar();
  void CreateUI();
  void SetupEventHandlers();
  void StartMCPServer();
  
  void OnExit(wxCommandEvent& event);
  void OnAbout(wxCommandEvent& event);
  void OnConnect(wxCommandEvent& event);
  void OnDisconnect(wxCommandEvent& event);
  void OnSendMessage(wxCommandEvent& event);
  void OnModelSelected(wxCommandEvent& event);
  void OnSetApiKey(wxCommandEvent& event);
  
  // Zen event handlers
  void OnZenConnected(wxCommandEvent& event);
  void OnZenDisconnected(wxCommandEvent& event);
  void OnZenMessageReceived(wxCommandEvent& event);
  void OnZenStreamChunk(wxCommandEvent& event);
  void OnZenError(wxCommandEvent& event);
  void OnZenModelsLoaded(wxCommandEvent& event);
  
  void UpdateConnectionStatus();
  void PopulateModelList();
  void AppendToChat(const wxString& sender, const wxString& message);
  void LogJsonTraffic(const wxString& direction, const wxString& json);
  wxString LoadApiKeyFromKeychain();
  bool SaveApiKeyToKeychain(const wxString& key);
  bool ClearApiKeyFromKeychain();
  
  void OnProviderSelected(wxCommandEvent& event);

  // Chat UI elements
  StreamingTextCtrl* m_chatDisplay = nullptr;
  wxTextCtrl* m_messageInput = nullptr;
  wxButton* m_sendButton = nullptr;
  wxChoice* m_providerChoice = nullptr;
  wxChoice* m_modelChoice = nullptr;
  wxStaticText* m_statusLabel = nullptr;
  wxButton* m_apiKeyButton = nullptr;
  
  // Debug UI elements
  wxTextCtrl* m_jsonLog = nullptr;
  
  // Streaming chunk batching
  wxString m_pendingChunks;
  wxTimer m_chunkFlushTimer;
  static constexpr int CHUNK_FLUSH_TIMER_ID = 4001;
  static constexpr int CHUNK_FLUSH_INTERVAL_MS = 50; // ~20 FPS
  void OnChunkFlushTimer(wxTimerEvent& event);
  
  // Thinking block support
  wxString m_thinkingBuffer;           // Buffer for thinking/reasoning text
  bool m_isReceivingThinking = false;  // Currently receiving thinking chunks
  size_t m_thinkingBlockIndex = 0;     // Block index of the thinking block

  // Markdown rendering support
  wxString m_aiResponseBuffer;      // Full buffer of AI response for markdown parsing
  bool m_collectingAiResponse = false;  // Whether we're currently collecting an AI response
  size_t m_aiResponseStartBlock = 0;   // Block index where the AI response starts
  size_t m_lastMarkdownRenderLen = 0;  // Buffer length at last markdown render
  
  // Periodic markdown re-render during streaming
  wxTimer m_markdownRenderTimer;
  static constexpr int MARKDOWN_RENDER_TIMER_ID = 4002;
  static constexpr int MARKDOWN_RENDER_INTERVAL_MS = 500; // Re-render every 500ms
  void OnMarkdownRenderTimer(wxTimerEvent& event);
  
  // Replaces blocks from m_aiResponseStartBlock onwards with markdown-rendered blocks
  void ReplaceAiResponseWithMarkdown();
  
  wxDECLARE_EVENT_TABLE();
};

enum class MenuID : int {
  Exit = wxID_EXIT,
  About = wxID_ABOUT,
  Connect = 1000,
  Disconnect,
  Settings,
  SendMessage = 2000,
  SetApiKey = 3000,
  ProviderChoice = 4000,
  ModelChoice = 4001
};

} // namespace fcn::ui
