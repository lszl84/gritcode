#include "ui/main_frame.h"
#include "mcp/mcp_server.h"
#include <wx/splitter.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/statline.h>
#include <wx/log.h>

namespace zencode::ui {

wxBEGIN_EVENT_TABLE(MainFrame, wxFrame)
  EVT_MENU(static_cast<int>(MenuID::Exit), MainFrame::OnExit)
  EVT_MENU(static_cast<int>(MenuID::About), MainFrame::OnAbout)
  EVT_MENU(static_cast<int>(MenuID::Connect), MainFrame::OnConnect)
  EVT_MENU(static_cast<int>(MenuID::Disconnect), MainFrame::OnDisconnect)
  EVT_BUTTON(static_cast<int>(MenuID::SendMessage), MainFrame::OnSendMessage)
  EVT_CHOICE(wxID_ANY, MainFrame::OnModelSelected)
  
  // Zen events
  EVT_COMMAND(wxID_ANY, zencode::zen::ZEN_CONNECTED, MainFrame::OnZenConnected)
  EVT_COMMAND(wxID_ANY, zencode::zen::ZEN_DISCONNECTED, MainFrame::OnZenDisconnected)
  EVT_COMMAND(wxID_ANY, zencode::zen::ZEN_MESSAGE_RECEIVED, MainFrame::OnZenMessageReceived)
  EVT_COMMAND(wxID_ANY, zencode::zen::ZEN_ERROR_OCCURRED, MainFrame::OnZenError)
  EVT_COMMAND(wxID_ANY, zencode::zen::ZEN_MODELS_LOADED, MainFrame::OnZenModelsLoaded)
wxEND_EVENT_TABLE()

MainFrame::MainFrame() 
  : wxFrame(nullptr, wxID_ANY, "ZenCode", wxDefaultPosition, wxSize(1200, 800)) {
  
  CreateMenuBar();
  CreateUI();
  SetupEventHandlers();
  
  Centre();
  
  // Start MCP server for programmatic control
  StartMCPServer();
  
  // Auto-connect anonymously on startup
  wxCommandEvent dummy;
  OnConnect(dummy);
}

void MainFrame::StartMCPServer() {
  wxLogMessage("MainFrame::StartMCPServer: Starting MCP server...");
  auto& mcp = mcp::MCPServer::Instance();
  mcp.Start(8765);
  wxLogMessage("MainFrame::StartMCPServer: MCP server started on port 8765");
}

void MainFrame::CreateMenuBar() {
  auto* menuBar = new wxMenuBar();
  
  // File menu
  auto* fileMenu = new wxMenu();
  fileMenu->Append(static_cast<int>(MenuID::Connect), "&Connect (Anonymous)\tCtrl+C", "Connect to OpenCode Zen anonymously");
  fileMenu->Append(static_cast<int>(MenuID::Disconnect), "&Disconnect\tCtrl+D", "Disconnect from Zen");
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
  auto* mainSizer = new wxBoxSizer(wxVERTICAL);
  
  // Create splitter for sidebar and main content
  m_splitter = new wxSplitterWindow(this, wxID_ANY);
  m_splitter->SetMinimumPaneSize(200);
  
  // Sidebar panel
  m_sidebarPanel = new wxPanel(m_splitter);
  auto* sidebarSizer = new wxBoxSizer(wxVERTICAL);
  
  // Connection status
  m_statusLabel = new wxStaticText(m_sidebarPanel, wxID_ANY, "Disconnected");
  sidebarSizer->Add(m_statusLabel, 0, wxALL | wxEXPAND, 10);
  
  sidebarSizer->Add(new wxStaticLine(m_sidebarPanel), 0, wxALL | wxEXPAND, 5);
  
  // Model selection
  sidebarSizer->Add(new wxStaticText(m_sidebarPanel, wxID_ANY, "Model:"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
  m_modelChoice = new wxChoice(m_sidebarPanel, wxID_ANY);
  sidebarSizer->Add(m_modelChoice, 0, wxALL | wxEXPAND, 10);
  
  sidebarSizer->AddStretchSpacer();
  
  m_sidebarPanel->SetSizer(sidebarSizer);
  
  // Main content panel - Chat interface
  m_mainPanel = new wxPanel(m_splitter);
  auto* mainPanelSizer = new wxBoxSizer(wxVERTICAL);
  
  // Chat display (read-only)
  m_chatDisplay = new wxTextCtrl(m_mainPanel, wxID_ANY, wxEmptyString, 
                                  wxDefaultPosition, wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH);
  m_chatDisplay->SetBackgroundColour(wxColour(30, 30, 30));
  m_chatDisplay->SetForegroundColour(wxColour(220, 220, 220));
  mainPanelSizer->Add(m_chatDisplay, 1, wxALL | wxEXPAND, 10);
  
  // Message input area
  auto* inputSizer = new wxBoxSizer(wxHORIZONTAL);
  m_messageInput = new wxTextCtrl(m_mainPanel, wxID_ANY, wxEmptyString,
                                   wxDefaultPosition, wxSize(-1, 60),
                                   wxTE_MULTILINE | wxTE_PROCESS_ENTER);
  m_sendButton = new wxButton(m_mainPanel, static_cast<int>(MenuID::SendMessage), "Send");
  m_sendButton->SetDefault();
  
  inputSizer->Add(m_messageInput, 1, wxALL | wxEXPAND, 5);
  inputSizer->Add(m_sendButton, 0, wxALL, 5);
  
  mainPanelSizer->Add(inputSizer, 0, wxALL | wxEXPAND, 5);
  m_mainPanel->SetSizer(mainPanelSizer);
  
  // Split the window
  m_splitter->SplitVertically(m_sidebarPanel, m_mainPanel, 250);
  
  mainSizer->Add(m_splitter, 1, wxEXPAND);
  SetSizer(mainSizer);
}

void MainFrame::SetupEventHandlers() {
  // Bind enter key in message input
  m_messageInput->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent& evt) {
    OnSendMessage(evt);
  });
  
  // Bind ZenClient events directly to our handlers
  auto& zen = zen::ZenClient::Instance();
  zen.Bind(zencode::zen::ZEN_CONNECTED, &MainFrame::OnZenConnected, this);
  zen.Bind(zencode::zen::ZEN_DISCONNECTED, &MainFrame::OnZenDisconnected, this);
  zen.Bind(zencode::zen::ZEN_MESSAGE_RECEIVED, &MainFrame::OnZenMessageReceived, this);
  zen.Bind(zencode::zen::ZEN_ERROR_OCCURRED, &MainFrame::OnZenError, this);
  zen.Bind(zencode::zen::ZEN_MODELS_LOADED, &MainFrame::OnZenModelsLoaded, this);
  wxLogMessage("MainFrame::SetupEventHandlers: Bound ZenClient events to MainFrame");
}

void MainFrame::OnExit(wxCommandEvent& event) {
  Close(true);
}

void MainFrame::OnAbout(wxCommandEvent& event) {
  wxMessageBox("ZenCode - OpenCode Zen Agent Harness\n\n"
               "A fast, native replacement for web-based TUIs\n\n"
               "Free models available:\n"
               "- Big Pickle\n"
               "- MiMo V2 Flash Free\n"
               "- Nemotron 3 Super Free\n"
               "- MiniMax M2.5 Free\n\n"
               "License: GPL v3",
               "About ZenCode",
               wxOK | wxICON_INFORMATION);
}

void MainFrame::OnConnect(wxCommandEvent& event) {
  wxLogMessage("OnConnect: Starting connection...");
  AppendToChat("System", "Connecting to OpenCode Zen...");
  
  // Connect anonymously (empty API key)
  auto& zen = zen::ZenClient::Instance();
  if (!zen.Connect("")) {
    wxLogError("OnConnect: Connection failed!");
    AppendToChat("Error", "Failed to connect to OpenCode Zen - check console for details");
    wxMessageBox("Failed to connect to OpenCode Zen\n\nMake sure you have internet connectivity.", 
                 "Connection Error", wxOK | wxICON_ERROR);
  } else {
    wxLogMessage("OnConnect: Connection initialized successfully");
    AppendToChat("System", "Connection initialized - waiting for models...");
  }
}

void MainFrame::OnDisconnect(wxCommandEvent& event) {
  auto& zen = zen::ZenClient::Instance();
  zen.Disconnect();
  UpdateConnectionStatus();
}

void MainFrame::OnSendMessage(wxCommandEvent& event) {
  wxString message = m_messageInput->GetValue();
  if (message.IsEmpty()) return;
  
  // Add user message to chat
  AppendToChat("You", message);
  m_messageInput->Clear();
  m_sendButton->Disable();
  
  // Send to Zen
  auto& zen = zen::ZenClient::Instance();
  if (zen.IsConnected()) {
    std::string model = zen.GetActiveModel();
    if (model.empty() && m_modelChoice->GetSelection() != wxNOT_FOUND) {
      model = m_modelChoice->GetString(m_modelChoice->GetSelection()).ToStdString();
    }
    zen.SendMessage(model, message.ToStdString());
  }
}

void MainFrame::OnModelSelected(wxCommandEvent& event) {
  int selection = m_modelChoice->GetSelection();
  if (selection != wxNOT_FOUND) {
    wxString modelName = m_modelChoice->GetString(selection);
    // Extract model ID from the choice (stored in client data or parsed)
    // For now, we'll use the model name directly
    zen::ZenClient::Instance().SetActiveModel(modelName.ToStdString());
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

void MainFrame::OnZenDisconnected(wxCommandEvent& event) {
  wxLogMessage("MainFrame::OnZenDisconnected called");
  UpdateConnectionStatus();
  m_modelChoice->Clear();
  
  // Notify MCP server
  mcp::MCPServer::Instance().OnDisconnected();
  
  AppendToChat("System", "Disconnected from Zen");
}

void MainFrame::OnZenMessageReceived(wxCommandEvent& event) {
  wxString message = event.GetString();
  long tokens = event.GetExtraLong();
  
  AppendToChat("AI", message);
  
  if (tokens > 0) {
    AppendToChat("System", wxString::Format("Tokens used: %ld", tokens));
  }
  
  // Notify MCP server
  mcp::MCPServer::Instance().OnMessageReceived(message, static_cast<int>(tokens));
  
  m_sendButton->Enable();
}

void MainFrame::OnZenError(wxCommandEvent& event) {
  wxString error = event.GetString();
  AppendToChat("Error", error);
  
  // Notify MCP server
  mcp::MCPServer::Instance().OnError(error);
  
  m_sendButton->Enable();
}

void MainFrame::OnZenModelsLoaded(wxCommandEvent& event) {
  wxLogMessage("MainFrame::OnZenModelsLoaded: Models have been loaded, populating list...");
  PopulateModelList();
  
  // Notify MCP server
  mcp::MCPServer::Instance().OnModelsLoaded();
  
  AppendToChat("System", "Models loaded successfully");
}

void MainFrame::UpdateConnectionStatus() {
  auto& zen = zen::ZenClient::Instance();
  if (zen.IsConnected()) {
    if (zen.IsAnonymous()) {
      m_statusLabel->SetLabel("Connected (Anonymous)");
    } else {
      m_statusLabel->SetLabel("Connected (API Key)");
    }
  } else {
    m_statusLabel->SetLabel("Disconnected");
  }
}

void MainFrame::PopulateModelList() {
  wxLogMessage("MainFrame::PopulateModelList: Clearing choice control");
  m_modelChoice->Clear();
  
  auto& zen = zen::ZenClient::Instance();
  auto models = zen.GetFreeModels();
  
  wxLogMessage("MainFrame::PopulateModelList: Got %zu free models", models.size());
  
  for (const auto& model : models) {
    wxLogMessage("MainFrame::PopulateModelList: Adding model '%s'", model.name.c_str());
    m_modelChoice->Append(wxString::FromUTF8(model.name));
  }
  
  if (!models.empty()) {
    m_modelChoice->SetSelection(0);
    zen.SetActiveModel(models[0].id);
    wxLogMessage("MainFrame::PopulateModelList: Set active model to '%s'", models[0].id.c_str());
  } else {
    wxLogWarning("MainFrame::PopulateModelList: No free models available!");
  }
}

void MainFrame::AppendToChat(const wxString& sender, const wxString& message) {
  wxString timestamp = wxDateTime::Now().Format("%H:%M:%S");
  wxString formatted;
  
  if (sender == "You") {
    formatted = wxString::Format("[%s] %s:\n%s\n\n", timestamp, sender, message);
    m_chatDisplay->SetDefaultStyle(wxTextAttr(wxColour(100, 200, 255)));
  } else if (sender == "AI") {
    formatted = wxString::Format("[%s] %s:\n%s\n\n", timestamp, sender, message);
    m_chatDisplay->SetDefaultStyle(wxTextAttr(wxColour(150, 255, 150)));
  } else if (sender == "Error") {
    formatted = wxString::Format("[%s] ERROR: %s\n\n", timestamp, message);
    m_chatDisplay->SetDefaultStyle(wxTextAttr(wxColour(255, 100, 100)));
  } else {
    formatted = wxString::Format("[%s] %s: %s\n\n", timestamp, sender, message);
    m_chatDisplay->SetDefaultStyle(wxTextAttr(wxColour(200, 200, 200)));
  }
  
  m_chatDisplay->AppendText(formatted);
  m_chatDisplay->ShowPosition(m_chatDisplay->GetLastPosition());
}

} // namespace zencode::ui
