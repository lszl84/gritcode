#include "main_frame.h"
#include "streaming_text_ctrl.h"
#include "mcp_server.h"
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/statline.h>
#include <wx/log.h>
#include <wx/datetime.h>

namespace fcn::ui {

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
  EVT_MENU(static_cast<int>(MenuID::Exit), MainFrame::OnExit)
  EVT_MENU(static_cast<int>(MenuID::About), MainFrame::OnAbout)
  EVT_MENU(static_cast<int>(MenuID::Connect), MainFrame::OnConnect)
  EVT_MENU(static_cast<int>(MenuID::Disconnect), MainFrame::OnDisconnect)
  EVT_BUTTON(static_cast<int>(MenuID::SendMessage), MainFrame::OnSendMessage)
  EVT_BUTTON(static_cast<int>(MenuID::SetApiKey), MainFrame::OnSetApiKey)
  EVT_MENU(static_cast<int>(MenuID::SetApiKey), MainFrame::OnSetApiKey)
  EVT_CHOICE(wxID_ANY, MainFrame::OnModelSelected)
wxEND_EVENT_TABLE()

MainFrame::MainFrame() 
  : wxFrame(nullptr, wxID_ANY, "FastCode Native", wxDefaultPosition, wxSize(1200, 800))
  , m_chunkFlushTimer(this, CHUNK_FLUSH_TIMER_ID)
  , m_markdownRenderTimer(this, MARKDOWN_RENDER_TIMER_ID) {
  
  // Set X11 WM_CLASS instance name (class is set by SetAppName in Application)
  SetName("main");
  
  CreateMenuBar();
  CreateUI();
  SetupEventHandlers();
  
  Centre();

  // Start MCP server for programmatic control
  StartMCPServer();
  
  // Connect via ACP to the system claude binary (no API key needed)
  auto& zen = zen::ZenClient::Instance();
  AppendToChat("System", "Connecting via claude binary (ACP)...");
  zen.Connect();
}

void MainFrame::StartMCPServer() {
  wxLogMessage("MainFrame::StartMCPServer: Starting MCP server...");
  auto& mcp = mcp::MCPServer::Instance();
  if (mcp.Start()) {
    wxLogMessage("MainFrame::StartMCPServer: MCP stdio server started");
  } else {
    wxLogMessage("MainFrame::StartMCPServer: MCP server not started (stdin is a terminal)");
  }
}

void MainFrame::CreateMenuBar() {
  auto* menuBar = new wxMenuBar();
  
  // File menu
  auto* fileMenu = new wxMenu();
  fileMenu->Append(static_cast<int>(MenuID::SetApiKey), "Claude &Settings...\tCtrl+K", "Show claude binary connection info");
  fileMenu->AppendSeparator();
  fileMenu->Append(static_cast<int>(MenuID::Connect), "&Reconnect\tCtrl+R", "Reconnect via claude binary");
  fileMenu->Append(static_cast<int>(MenuID::Disconnect), "&Disconnect\tCtrl+D", "Disconnect");
  fileMenu->AppendSeparator();
  fileMenu->Append(static_cast<int>(MenuID::Exit), "E&xit\tAlt+F4", "Quit the application");
  menuBar->Append(fileMenu, "&File");
  
  // Help menu
  auto* helpMenu = new wxMenu();
  helpMenu->Append(static_cast<int>(MenuID::About), "&About\tF1", "Show about dialog");
  menuBar->Append(helpMenu, "&Help");
  
  SetMenuBar(menuBar);
}

void MainFrame::CreateUI() {
  // Create main panel (like cutescroll)
  auto* panel = new wxPanel(this);
  auto* mainSizer = new wxBoxSizer(wxVERTICAL);
  
  // Chat display (main area)
  m_chatDisplay = new StreamingTextCtrl(panel, wxID_ANY);
  m_chatDisplay->SetAutoScroll(true);
  mainSizer->Add(m_chatDisplay, 1, wxEXPAND | wxALL, 5);
  
  // Message input area
  auto* inputSizer = new wxBoxSizer(wxHORIZONTAL);
  m_messageInput = new wxTextCtrl(panel, wxID_ANY, wxEmptyString,
                                   wxDefaultPosition, wxSize(-1, 60),
                                   wxTE_MULTILINE | wxTE_PROCESS_ENTER);
  m_sendButton = new wxButton(panel, static_cast<int>(MenuID::SendMessage), "Send");
  m_sendButton->SetDefault();
  
  inputSizer->Add(m_messageInput, 1, wxALL | wxEXPAND, 5);
  inputSizer->Add(m_sendButton, 0, wxALL, 5);
  
  mainSizer->Add(inputSizer, 0, wxEXPAND);
  
  // Bottom control bar (model selector)
  auto* bottomSizer = new wxBoxSizer(wxHORIZONTAL);
  
  // Model selection
  bottomSizer->Add(new wxStaticText(panel, wxID_ANY, "Model:"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
  m_modelChoice = new wxChoice(panel, wxID_ANY);
  bottomSizer->Add(m_modelChoice, 0, wxALL, 5);
  
  // Status label
  m_statusLabel = new wxStaticText(panel, wxID_ANY, "Disconnected");
  bottomSizer->Add(m_statusLabel, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
  
  bottomSizer->AddStretchSpacer();
  
  mainSizer->Add(bottomSizer, 0, wxEXPAND);
  
  panel->SetSizer(mainSizer);
  
  // Force initial layout so sizer positions children correctly before first paint
  panel->Layout();
}

void MainFrame::SetupEventHandlers() {
  // Bind enter key in message input
  m_messageInput->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& evt) {
    OnSendMessage(evt);
  });
  
  // Handle MCP send_message requests — simulate typing in the input box
  Bind(mcp::MCP_SEND_MESSAGE_REQUEST, [this](wxCommandEvent& evt) {
    m_messageInput->SetValue(evt.GetString());
    wxCommandEvent dummy;
    OnSendMessage(dummy);
  });

  // Bind ZenClient events directly to our handlers
  auto& zen = zen::ZenClient::Instance();
  zen.Bind(fcn::zen::ZEN_CONNECTED, &MainFrame::OnZenConnected, this);
  zen.Bind(fcn::zen::ZEN_DISCONNECTED, &MainFrame::OnZenDisconnected, this);
  zen.Bind(fcn::zen::ZEN_MESSAGE_RECEIVED, &MainFrame::OnZenMessageReceived, this);
  zen.Bind(fcn::zen::ZEN_STREAM_CHUNK, &MainFrame::OnZenStreamChunk, this);
  zen.Bind(fcn::zen::ZEN_ERROR_OCCURRED, &MainFrame::OnZenError, this);
  zen.Bind(fcn::zen::ZEN_MODELS_LOADED, &MainFrame::OnZenModelsLoaded, this);
  
  // Bind chunk flush timer
  Bind(wxEVT_TIMER, &MainFrame::OnChunkFlushTimer, this, CHUNK_FLUSH_TIMER_ID);
  
  // Bind markdown render timer
  Bind(wxEVT_TIMER, &MainFrame::OnMarkdownRenderTimer, this, MARKDOWN_RENDER_TIMER_ID);
  
  // Set up JSON logging callback
  zen.SetJsonLogCallback([this](const std::string& direction, const std::string& json) {
    this->LogJsonTraffic(wxString::FromUTF8(direction), wxString::FromUTF8(json));
  });
  
  wxLogMessage("MainFrame::SetupEventHandlers: Bound ZenClient events to MainFrame");
}

void MainFrame::OnExit(wxCommandEvent& /*event*/) {
  Close(true);
}

void MainFrame::OnAbout(wxCommandEvent& /*event*/) {
  wxMessageBox("FastCode Native - Claude ACP Harness\n\n"
               "Communicates with the system 'claude' binary\n"
               "via ACP (Agent Communication Protocol).\n\n"
               "Models: Claude Opus 4.6, Sonnet 4.6, Haiku 4.5\n\n"
               "License: GPL v3",
               "About FastCode Native",
               wxOK | wxICON_INFORMATION);
}

void MainFrame::OnConnect(wxCommandEvent& /*event*/) {
  wxLogMessage("OnConnect: Reconnecting...");

  auto& zen = zen::ZenClient::Instance();
  zen.Disconnect();

  AppendToChat("System", "Reconnecting via claude binary...");
  zen.Connect();
}

void MainFrame::OnDisconnect(wxCommandEvent& /*event*/) {
  auto& zen = zen::ZenClient::Instance();
  zen.Disconnect();
  UpdateConnectionStatus();
}

void MainFrame::OnSendMessage(wxCommandEvent& /*event*/) {
  wxString message = m_messageInput->GetValue();
  if (message.IsEmpty()) return;
  
  // Add user message to chat
  AppendToChat("You", message);
  
  // Track where the AI response blocks will start
  m_aiResponseStartBlock = m_chatDisplay->GetBlockCount();
  
  // Start collecting AI response for markdown rendering
  // No plain text block created - markdown timer handles display
  m_aiResponseBuffer.Clear();
  m_lastMarkdownRenderLen = 0;
  m_collectingAiResponse = true;
  
  // Start periodic markdown re-render timer
  m_markdownRenderTimer.Start(MARKDOWN_RENDER_INTERVAL_MS);
  
  m_messageInput->Clear();
  m_sendButton->Disable();
  
  // Send to Zen
  auto& zen = zen::ZenClient::Instance();
  if (zen.IsConnected()) {
    std::string model = zen.GetActiveModel();
    if (model.empty() && m_modelChoice->GetSelection() != wxNOT_FOUND) {
      auto* data = dynamic_cast<wxStringClientData*>(
          m_modelChoice->GetClientObject(m_modelChoice->GetSelection()));
      if (data) {
        model = data->GetData().ToStdString();
      }
    }
    zen.SendMessage(model, message.ToStdString());
  }
}

void MainFrame::OnModelSelected(wxCommandEvent& /*event*/) {
  int selection = m_modelChoice->GetSelection();
  if (selection != wxNOT_FOUND) {
    auto* data = dynamic_cast<wxStringClientData*>(m_modelChoice->GetClientObject(selection));
    if (data) {
      std::string modelId = data->GetData().ToStdString();
      wxLogMessage("MainFrame::OnModelSelected: Setting model to '%s'", modelId.c_str());
      zen::ZenClient::Instance().SetActiveModel(modelId);
    }
  }
}

void MainFrame::OnZenConnected(wxCommandEvent& event) {
  wxLogMessage("MainFrame::OnZenConnected called");
  UpdateConnectionStatus();
  
  // Notify MCP server
  mcp::MCPServer::Instance().OnConnected(event.GetString());
  
  wxString msg = event.GetString();
  AppendToChat("System", msg);
}

void MainFrame::OnZenDisconnected(wxCommandEvent& /*event*/) {
  wxLogMessage("MainFrame::OnZenDisconnected called");
  UpdateConnectionStatus();
  m_modelChoice->Clear();
  
  // Notify MCP server
  mcp::MCPServer::Instance().OnDisconnected();
  
  AppendToChat("System", "Disconnected");
}

void MainFrame::OnZenMessageReceived(wxCommandEvent& event) {
  // Final message after streaming completes - flush any pending chunks
  if (!m_pendingChunks.IsEmpty()) {
    if (m_collectingAiResponse) {
      m_aiResponseBuffer += m_pendingChunks;
    }
    m_pendingChunks.clear();
  }
  m_chunkFlushTimer.Stop();
  m_markdownRenderTimer.Stop();
  
  // Final markdown render of the complete response
  if (m_collectingAiResponse && !m_aiResponseBuffer.IsEmpty()) {
    m_collectingAiResponse = false;
    ReplaceAiResponseWithMarkdown();
    m_aiResponseBuffer.Clear();
  }
  
  // Force full redraw to ensure final text layout is rendered
  m_chatDisplay->Refresh();
  
  long tokens = event.GetExtraLong();
  
  if (tokens > 0) {
    AppendToChat("System", wxString::Format("Tokens used: %ld", tokens));
  }
  
  // Notify MCP server
  wxString message = event.GetString();
  mcp::MCPServer::Instance().OnMessageReceived(message, static_cast<int>(tokens));
  
  m_sendButton->Enable();
}

void MainFrame::OnZenStreamChunk(wxCommandEvent& event) {
  wxString chunk = event.GetString();
  if (!chunk.IsEmpty()) {
    // Always collect into markdown buffer during AI response
    if (m_collectingAiResponse) {
      m_aiResponseBuffer += chunk;
      // Don't use ContinueStream - the markdown render timer handles display
      return;
    }
    
    // Non-AI chunks (shouldn't happen, but fallback to plain text)
    m_pendingChunks += chunk;
    if (!m_chunkFlushTimer.IsRunning()) {
      m_chunkFlushTimer.Start(CHUNK_FLUSH_INTERVAL_MS);
    }
  }
}

void MainFrame::OnChunkFlushTimer(wxTimerEvent& /*event*/) {
  if (!m_pendingChunks.IsEmpty()) {
    m_chatDisplay->ContinueStream(m_pendingChunks);
    m_pendingChunks.clear();
  } else {
    m_chunkFlushTimer.Stop();
  }
}

void MainFrame::OnMarkdownRenderTimer(wxTimerEvent& /*event*/) {
  if (!m_collectingAiResponse || m_aiResponseBuffer.IsEmpty()) return;
  
  // Only re-render if buffer has grown significantly since last render
  size_t currentLen = m_aiResponseBuffer.length();
  if (currentLen <= m_lastMarkdownRenderLen + 20) return;  // Need at least 20 new chars
  
  ReplaceAiResponseWithMarkdown();
  m_lastMarkdownRenderLen = currentLen;
}

void MainFrame::ReplaceAiResponseWithMarkdown() {
  // Remove all blocks from the AI response start onwards
  m_chatDisplay->RemoveBlocksFrom(m_aiResponseStartBlock);
  
  // Re-render the full buffer as markdown
  auto utf8Buf = m_aiResponseBuffer.ToUTF8();
  std::string markdownStr(utf8Buf.data(), utf8Buf.length());
  m_chatDisplay->RenderMarkdown(markdownStr);
}

void MainFrame::OnZenError(wxCommandEvent& event) {
  wxString error = event.GetString();
  AppendToChat("Error", error);
  
  // Notify MCP server
  mcp::MCPServer::Instance().OnError(error);
  
  m_sendButton->Enable();
}

void MainFrame::OnZenModelsLoaded(wxCommandEvent& /*event*/) {
  wxLogMessage("MainFrame::OnZenModelsLoaded: Models have been loaded, populating list...");
  PopulateModelList();
  
  // Notify MCP server
  mcp::MCPServer::Instance().OnModelsLoaded();
  
  AppendToChat("System", "Models loaded successfully");
}

void MainFrame::UpdateConnectionStatus() {
  auto& zen = zen::ZenClient::Instance();
  m_statusLabel->SetLabel(zen.IsConnected() ? "Connected (claude)" : "Disconnected");
}

void MainFrame::PopulateModelList() {
  wxLogMessage("MainFrame::PopulateModelList: Clearing choice control");
  m_modelChoice->Clear();

  auto& zen = zen::ZenClient::Instance();
  auto models = zen.GetModels();

  wxLogMessage("MainFrame::PopulateModelList: Got %zu models", models.size());

  for (const auto& model : models) {
    m_modelChoice->Append(wxString::FromUTF8(model.name),
                          new wxStringClientData(wxString::FromUTF8(model.id)));
  }

  if (!models.empty()) {
    // Default to claude-sonnet-4-6
    int defaultIdx = 0;
    for (size_t i = 0; i < models.size(); ++i) {
      if (models[i].id == "claude-sonnet-4-6") {
        defaultIdx = static_cast<int>(i);
        break;
      }
    }
    m_modelChoice->SetSelection(defaultIdx);
    zen.SetActiveModel(models[defaultIdx].id);
    wxLogMessage("MainFrame::PopulateModelList: Set active model to '%s'", models[defaultIdx].id.c_str());
  } else {
    wxLogWarning("MainFrame::PopulateModelList: No models available!");
  }
}

void MainFrame::AppendToChat(const wxString& sender, const wxString& message) {
  if (sender == "You") {
    m_chatDisplay->AppendStream(BlockType::USER_PROMPT, message);
  } else if (sender == "AI") {
    m_chatDisplay->AppendStream(BlockType::NORMAL, message);
  } else if (sender == "Error") {
    m_chatDisplay->AppendStream(BlockType::THINKING, "Error: " + message);
  } else {
    // System messages
    m_chatDisplay->AppendStream(BlockType::THINKING, sender + ": " + message);
  }
}

void MainFrame::LogJsonTraffic(const wxString& direction, const wxString& json) {
  if (!m_jsonLog) return;
  
  wxString timestamp = wxDateTime::Now().Format("%H:%M:%S");
  wxString prefix = direction;  // Now "SEND" or "RECV"
  
  m_jsonLog->AppendText(wxString::Format("\n=== %s [%s] ===\n", prefix, timestamp));
  m_jsonLog->AppendText(json);
  m_jsonLog->AppendText("\n");
  
  // Auto-scroll to bottom
  m_jsonLog->ShowPosition(m_jsonLog->GetLastPosition());
}


void MainFrame::OnSetApiKey(wxCommandEvent& /*event*/) {
    wxMessageBox(
        "This build communicates with the system 'claude' binary\n"
        "via ACP (Agent Communication Protocol).\n\n"
        "Authentication is handled by the claude binary itself\n"
        "(run 'claude login' in a terminal to authenticate).\n\n"
        "No API key is needed here.",
        "Claude ACP Settings",
        wxOK | wxICON_INFORMATION);
}

} // namespace fcn::ui
