#include "main_frame.h"
#include "streaming_text_ctrl.h"
#include "mcp_server.h"
#include <wx/splitter.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/statline.h>
#include <wx/log.h>
#include <wx/secretstore.h>
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
  
  // Zen events
  EVT_COMMAND(wxID_ANY, fcn::zen::ZEN_CONNECTED, MainFrame::OnZenConnected)
  EVT_COMMAND(wxID_ANY, fcn::zen::ZEN_DISCONNECTED, MainFrame::OnZenDisconnected)
  EVT_COMMAND(wxID_ANY, fcn::zen::ZEN_MESSAGE_RECEIVED, MainFrame::OnZenMessageReceived)
  EVT_COMMAND(wxID_ANY, fcn::zen::ZEN_ERROR_OCCURRED, MainFrame::OnZenError)
  EVT_COMMAND(wxID_ANY, fcn::zen::ZEN_MODELS_LOADED, MainFrame::OnZenModelsLoaded)
wxEND_EVENT_TABLE()

MainFrame::MainFrame() 
  : wxFrame(nullptr, wxID_ANY, "FastCode Native", wxDefaultPosition, wxSize(1200, 800)) {
  
  // Set X11 WM_CLASS instance name (class is set by SetAppName in Application)
  SetName("main");
  
  CreateMenuBar();
  CreateUI();
  SetupEventHandlers();
  
  Centre();

  // Start MCP server for programmatic control
  StartMCPServer();
  
  // Auto-connect: use saved API key from keychain if available
  wxString savedKey = LoadApiKeyFromKeychain();
  auto& zen = zen::ZenClient::Instance();
  if (!savedKey.IsEmpty()) {
    AppendToChat("System", "Connecting with saved API key...");
    zen.Connect(savedKey.ToStdString());
  } else {
    AppendToChat("System", "Connecting anonymously...");
    zen.Connect("");
  }
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
  fileMenu->Append(static_cast<int>(MenuID::SetApiKey), "Set &API Key...\tCtrl+K", "Set or clear your OpenCode Zen API key");
  fileMenu->AppendSeparator();
  fileMenu->Append(static_cast<int>(MenuID::Connect), "&Reconnect\tCtrl+R", "Reconnect to OpenCode Zen");
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
  
  // Create outer splitter for sidebar vs (chat+debug)
  m_mainSplitter = new wxSplitterWindow(this, wxID_ANY);
  m_mainSplitter->SetMinimumPaneSize(150);
  
  // Sidebar panel
  m_sidebarPanel = new wxPanel(m_mainSplitter);
  auto* sidebarSizer = new wxBoxSizer(wxVERTICAL);
  
  // Connection status
  m_statusLabel = new wxStaticText(m_sidebarPanel, wxID_ANY, "Disconnected");
  sidebarSizer->Add(m_statusLabel, 0, wxALL | wxEXPAND, 10);
  
  sidebarSizer->Add(new wxStaticLine(m_sidebarPanel), 0, wxALL | wxEXPAND, 5);
  
  // Model selection
  sidebarSizer->Add(new wxStaticText(m_sidebarPanel, wxID_ANY, "Model:"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
  m_modelChoice = new wxChoice(m_sidebarPanel, wxID_ANY);
  sidebarSizer->Add(m_modelChoice, 0, wxALL | wxEXPAND, 10);
  
  sidebarSizer->Add(new wxStaticLine(m_sidebarPanel), 0, wxALL | wxEXPAND, 5);

  auto* apiKeyButton = new wxButton(m_sidebarPanel, static_cast<int>(MenuID::SetApiKey),
                                     "API Key...");
  sidebarSizer->Add(apiKeyButton, 0, wxALL | wxEXPAND, 10);

  sidebarSizer->AddStretchSpacer();

  m_sidebarPanel->SetSizer(sidebarSizer);
  
  // Create inner splitter for chat vs debug (inside the right pane of main splitter)
  m_chatSplitter = new wxSplitterWindow(m_mainSplitter);
  m_chatSplitter->SetMinimumPaneSize(300);
  
  // Chat panel (left side of chat splitter)
  m_mainPanel = new wxPanel(m_chatSplitter);
  auto* mainPanelSizer = new wxBoxSizer(wxVERTICAL);
  
  // Chat display
  m_chatDisplay = new StreamingTextCtrl(m_mainPanel, wxID_ANY);
  m_chatDisplay->SetAutoScroll(true);
  mainPanelSizer->Add(m_chatDisplay, 1, wxEXPAND);
  
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
  
  // Debug panel (right side of chat splitter)
  m_debugPanel = new wxPanel(m_chatSplitter);
  auto* debugSizer = new wxBoxSizer(wxVERTICAL);
  
  debugSizer->Add(new wxStaticText(m_debugPanel, wxID_ANY, "JSON Traffic:"), 0, wxALL, 5);
  
  m_jsonLog = new wxTextCtrl(m_debugPanel, wxID_ANY, wxEmptyString,
                              wxDefaultPosition, wxDefaultSize,
                              wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
  m_jsonLog->SetFont(wxFont(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
  debugSizer->Add(m_jsonLog, 1, wxEXPAND | wxALL, 5);
  
  m_debugPanel->SetSizer(debugSizer);
  
  // Split chat and debug (chat on left, debug on right)
  m_chatSplitter->SplitVertically(m_mainPanel, m_debugPanel, 600);
  
  // Split main (sidebar on left, chat+debug on right)
  m_mainSplitter->SplitVertically(m_sidebarPanel, m_chatSplitter, 200);
  
  mainSizer->Add(m_mainSplitter, 1, wxEXPAND);
  SetSizer(mainSizer);
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
  zen.Bind(fcn::zen::ZEN_ERROR_OCCURRED, &MainFrame::OnZenError, this);
  zen.Bind(fcn::zen::ZEN_MODELS_LOADED, &MainFrame::OnZenModelsLoaded, this);
  
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
  wxMessageBox("FastCode Native - OpenCode Zen Agent Harness\n\n"
               "A fast, native replacement for web-based TUIs\n\n"
               "Free models available:\n"
               "- Big Pickle\n"
               "- MiMo V2 Flash Free\n"
               "- Nemotron 3 Super Free\n"
               "- MiniMax M2.5 Free\n\n"
               "License: GPL v3",
               "About FastCode Native",
               wxOK | wxICON_INFORMATION);
}

void MainFrame::OnConnect(wxCommandEvent& /*event*/) {
  wxLogMessage("OnConnect: Reconnecting...");

  auto& zen = zen::ZenClient::Instance();
  zen.Disconnect();

  wxString savedKey = LoadApiKeyFromKeychain();
  AppendToChat("System", savedKey.IsEmpty()
               ? "Reconnecting anonymously..."
               : "Reconnecting with saved API key...");

  if (!zen.Connect(savedKey.ToStdString())) {
    AppendToChat("Error", "Failed to connect to OpenCode Zen");
  }
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

void MainFrame::OnZenModelsLoaded(wxCommandEvent& /*event*/) {
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
  // Show all models when authenticated, only free models when anonymous
  auto models = zen.IsAnonymous() ? zen.GetFreeModels() : zen.GetModels();

  wxLogMessage("MainFrame::PopulateModelList: Got %zu models (anonymous=%d)",
               models.size(), zen.IsAnonymous());
  
  for (const auto& model : models) {
    wxLogMessage("MainFrame::PopulateModelList: Adding model '%s' (id='%s')",
                 model.name.c_str(), model.id.c_str());
    m_modelChoice->Append(wxString::FromUTF8(model.name),
                          new wxStringClientData(wxString::FromUTF8(model.id)));
  }
  
  if (!models.empty()) {
    // Prefer kimi-k2.5 to save tokens, otherwise fall back to first model
    int defaultIdx = 0;
    for (size_t i = 0; i < models.size(); ++i) {
      if (models[i].id == "kimi-k2.5") {
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

// --- API Key management via wxSecretStore ---

static const wxString KEYCHAIN_SERVICE = "FastCodeNative/OpenCodeZen";

wxString MainFrame::LoadApiKeyFromKeychain() {
    auto store = wxSecretStore::GetDefault();
    if (!store.IsOk()) {
        wxLogWarning("MainFrame: Could not access system keychain");
        return {};
    }

    wxString username;
    wxSecretValue secret;
    if (store.Load(KEYCHAIN_SERVICE, username, secret)) {
        wxLogMessage("MainFrame: Loaded API key from keychain");
        return secret.GetAsString();
    }

    return {};
}

bool MainFrame::SaveApiKeyToKeychain(const wxString& key) {
    wxLogMessage("SaveApiKeyToKeychain: Starting...");
    
    try {
        wxLogMessage("SaveApiKeyToKeychain: Getting store...");
        auto store = wxSecretStore::GetDefault();
        
        wxLogMessage("SaveApiKeyToKeychain: Checking if store is OK...");
        if (!store.IsOk()) {
            wxLogError("MainFrame: Could not access system keychain");
            return false;
        }
        
        wxLogMessage("SaveApiKeyToKeychain: Creating secret value...");
        wxSecretValue secret(key);
        
        wxLogMessage("SaveApiKeyToKeychain: Checking if secret is OK...");
        if (!secret.IsOk()) {
            wxLogError("MainFrame: Failed to create secret value");
            return false;
        }
        
        wxLogMessage("SaveApiKeyToKeychain: Calling store.Save()...");
        if (!store.Save(KEYCHAIN_SERVICE, "apikey", secret)) {
            wxLogError("MainFrame: Failed to save API key to keychain");
            return false;
        }

        wxLogMessage("MainFrame: API key saved to keychain");
        return true;
    } catch (const std::exception& e) {
        wxLogError("SaveApiKeyToKeychain: Exception: %s", e.what());
        return false;
    } catch (...) {
        wxLogError("SaveApiKeyToKeychain: Unknown exception");
        return false;
    }
}

bool MainFrame::ClearApiKeyFromKeychain() {
    auto store = wxSecretStore::GetDefault();
    if (!store.IsOk()) return false;
    return store.Delete(KEYCHAIN_SERVICE);
}

void MainFrame::OnSetApiKey(wxCommandEvent& /*event*/) {
    wxString currentKey = LoadApiKeyFromKeychain();
    bool hasKey = !currentKey.IsEmpty();

    wxString prompt = hasKey
        ? "An API key is currently saved in your keychain.\n\n"
          "Enter a new key to replace it, or clear the field and\n"
          "click OK to remove it and use anonymous access."
        : "Enter your OpenCode Zen API key to unlock all models.\n\n"
          "Get your key at https://opencode.ai\n"
          "Leave empty for anonymous access (free models only).";

    wxTextEntryDialog dlg(this, prompt, "API Key", hasKey ? "********" : "");

    if (dlg.ShowModal() != wxID_OK) return;

    wxString input = dlg.GetValue().Trim();

    // User didn't change the placeholder
    if (input == "********") return;

    auto& zen = zen::ZenClient::Instance();
    zen.Disconnect();

    if (input.IsEmpty()) {
        ClearApiKeyFromKeychain();
        AppendToChat("System", "API key removed. Reconnecting anonymously...");
        zen.Connect("");
    } else {
        if (!SaveApiKeyToKeychain(input)) {
            wxMessageBox("Could not save the API key to your system keychain.",
                         "Keychain Error", wxOK | wxICON_ERROR);
            return;
        }
        AppendToChat("System", "API key saved to keychain. Reconnecting...");
        zen.Connect(input.ToStdString());
    }
}

} // namespace fcn::ui
