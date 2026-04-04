#include "main_frame.h"
#include "streaming_text_ctrl.h"
#include "mcp_server.h"
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
  EVT_CHOICE(static_cast<int>(MenuID::ProviderChoice), MainFrame::OnProviderSelected)
  EVT_CHOICE(static_cast<int>(MenuID::ModelChoice), MainFrame::OnModelSelected)
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

  // Focus the prompt so user can start typing immediately
  m_messageInput->SetFocus();
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
  // No visible menu bar — keyboard shortcuts via accelerator table
  wxAcceleratorEntry entries[] = {
    {wxACCEL_CTRL, 'K', static_cast<int>(MenuID::SetApiKey)},
    {wxACCEL_CTRL, 'R', static_cast<int>(MenuID::Connect)},
    {wxACCEL_CTRL, 'D', static_cast<int>(MenuID::Disconnect)},
    {wxACCEL_ALT, WXK_F4, static_cast<int>(MenuID::Exit)},
  };
  SetAcceleratorTable(wxAcceleratorTable(4, entries));
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
  
  // Bottom control bar
  auto* bottomSizer = new wxBoxSizer(wxHORIZONTAL);

  // Provider selection
  bottomSizer->Add(new wxStaticText(panel, wxID_ANY, "Provider:"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
  m_providerChoice = new wxChoice(panel, static_cast<int>(MenuID::ProviderChoice));
  m_providerChoice->Append("OpenCode Zen");
  m_providerChoice->Append("Claude (ACP)");
  m_providerChoice->SetSelection(0);
  bottomSizer->Add(m_providerChoice, 0, wxALL, 5);

  // Model selection
  bottomSizer->Add(new wxStaticText(panel, wxID_ANY, "Model:"), 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);
  m_modelChoice = new wxChoice(panel, static_cast<int>(MenuID::ModelChoice));
  bottomSizer->Add(m_modelChoice, 0, wxALL, 5);

  // Status label
  m_statusLabel = new wxStaticText(panel, wxID_ANY, "Disconnected");
  bottomSizer->Add(m_statusLabel, 0, wxALL | wxALIGN_CENTER_VERTICAL, 5);

  bottomSizer->AddStretchSpacer();

  // API Key button (only relevant for Zen provider)
  m_apiKeyButton = new wxButton(panel, static_cast<int>(MenuID::SetApiKey), "API Key...");
  bottomSizer->Add(m_apiKeyButton, 0, wxALL, 5);
  
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

  // Escape key: cancel in-progress request
  m_messageInput->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& evt) {
    if (evt.GetKeyCode() == WXK_ESCAPE && !m_sendButton->IsEnabled()) {
      // Request is in progress — abort it
      auto& zen = zen::ZenClient::Instance();
      zen.Abort();

      m_chunkFlushTimer.Stop();
      m_markdownRenderTimer.Stop();

      // Finalize thinking state
      if (m_isReceivingThinking) {
        m_chatDisplay->StopThinking(m_thinkingBlockIndex);
        m_isReceivingThinking = false;
      }

      // Final markdown render of whatever we got so far
      if (m_collectingAiResponse && !m_aiResponseBuffer.IsEmpty()) {
        if (!m_thinkingBuffer.IsEmpty() && m_aiResponseStartBlock <= m_thinkingBlockIndex) {
          m_aiResponseStartBlock = m_thinkingBlockIndex + 1;
        }
        ReplaceAiResponseWithMarkdown();
      }
      m_collectingAiResponse = false;
      m_aiResponseBuffer.Clear();

      AppendToChat("System", "Cancelled");
      m_sendButton->Enable();
      m_providerChoice->Enable();
      return;
    }
    evt.Skip();
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
  
  // Track where the AI response blocks will start
  m_aiResponseStartBlock = m_chatDisplay->GetBlockCount();
  
  // Reset thinking state
  m_thinkingBuffer.Clear();
  m_isReceivingThinking = false;

  // Start collecting AI response for markdown rendering
  // No plain text block created - markdown timer handles display
  m_aiResponseBuffer.Clear();
  m_lastMarkdownRenderLen = 0;
  m_collectingAiResponse = true;
  
  // Start periodic markdown re-render timer
  m_markdownRenderTimer.Start(MARKDOWN_RENDER_INTERVAL_MS);
  
  m_messageInput->Clear();
  m_sendButton->Disable();
  m_providerChoice->Disable();

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
  // Final message after streaming completes - flush any pending chunks
  if (!m_pendingChunks.IsEmpty()) {
    if (m_collectingAiResponse) {
      m_aiResponseBuffer += m_pendingChunks;
    }
    m_pendingChunks.clear();
  }
  m_chunkFlushTimer.Stop();
  m_markdownRenderTimer.Stop();
  
  // Finalize thinking state
  if (m_isReceivingThinking) {
    m_chatDisplay->StopThinking(m_thinkingBlockIndex);
    m_isReceivingThinking = false;
  }

  // Final markdown render of the complete response
  if (m_collectingAiResponse && !m_aiResponseBuffer.IsEmpty()) {
    m_collectingAiResponse = false;
    // Ensure start block is after thinking block if present
    if (!m_thinkingBuffer.IsEmpty() && m_aiResponseStartBlock <= m_thinkingBlockIndex) {
      m_aiResponseStartBlock = m_thinkingBlockIndex + 1;
    }
    ReplaceAiResponseWithMarkdown();
    m_aiResponseBuffer.Clear();
  } else {
    m_collectingAiResponse = false;
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
  m_providerChoice->Enable();
}

void MainFrame::OnZenStreamChunk(wxCommandEvent& event) {
  wxString chunk = event.GetString();
  bool isThinking = event.GetExtraLong() != 0;

  if (!chunk.IsEmpty() && m_collectingAiResponse) {
    if (isThinking) {
      // Thinking chunk — accumulate into thinking block
      if (!m_isReceivingThinking) {
        // First thinking chunk: create a collapsed THINKING block with loading animation
        m_isReceivingThinking = true;
        m_thinkingBuffer.Clear();
        m_thinkingBlockIndex = m_chatDisplay->GetBlockCount();
        m_chatDisplay->AppendStream(BlockType::THINKING, chunk);
        m_chatDisplay->StartThinking(m_thinkingBlockIndex);
      } else {
        // Continue appending to existing thinking block
        m_chatDisplay->ContinueStream(chunk);
      }
      m_thinkingBuffer += chunk;
      return;
    }

    // Content chunk — finalize thinking block if transitioning
    if (m_isReceivingThinking) {
      m_isReceivingThinking = false;
      m_chatDisplay->StopThinking(m_thinkingBlockIndex);
      // Update AI response start to after the thinking block
      m_aiResponseStartBlock = m_chatDisplay->GetBlockCount();
    }

    m_aiResponseBuffer += chunk;
    // Don't use ContinueStream - the markdown render timer handles display
    return;
  }

  if (!chunk.IsEmpty()) {
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
  // Incremental update: diffs new blocks against existing ones so only the
  // changed tail (usually the last paragraph) is re-measured on paint.
  auto utf8Buf = m_aiResponseBuffer.ToUTF8();
  std::string markdownStr(utf8Buf.data(), utf8Buf.length());
  m_chatDisplay->UpdateMarkdown(m_aiResponseStartBlock, markdownStr);
}

void MainFrame::OnZenError(wxCommandEvent& event) {
  wxString error = event.GetString();
  AppendToChat("Error", error);
  
  // Notify MCP server
  mcp::MCPServer::Instance().OnError(error);

  m_sendButton->Enable();
  m_providerChoice->Enable();
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
    wxString label = (zen.GetProviderType() == fcn::ProviderType::Claude)
      ? "Claude (Connected)"
      : (zen.IsAnonymous() ? "Zen (Anonymous)" : "Zen (API Key)");
    m_statusLabel->SetLabel(label);
  } else {
    m_statusLabel->SetLabel("Disconnected");
  }
}

void MainFrame::PopulateModelList() {
  m_modelChoice->Clear();

  auto& zen = zen::ZenClient::Instance();
  auto models = (zen.GetProviderType() == fcn::ProviderType::Zen && zen.IsAnonymous())
    ? zen.GetFreeModels() : zen.GetModels();

  wxLogMessage("MainFrame::PopulateModelList: %zu models", models.size());

  for (const auto& model : models) {
    m_modelChoice->Append(wxString::FromUTF8(model.name),
                          new wxStringClientData(wxString::FromUTF8(model.id)));
  }

  if (!models.empty()) {
    int defaultIdx = 0;
    if (zen.GetProviderType() == fcn::ProviderType::Zen) {
      for (size_t i = 0; i < models.size(); ++i) {
        if (models[i].id == "kimi-k2.5") { defaultIdx = static_cast<int>(i); break; }
      }
    } else {
      for (size_t i = 0; i < models.size(); ++i) {
        if (models[i].id == "claude-sonnet-4-6") { defaultIdx = static_cast<int>(i); break; }
      }
    }
    m_modelChoice->SetSelection(defaultIdx);
    zen.SetActiveModel(models[defaultIdx].id);
    wxLogMessage("MainFrame::PopulateModelList: active model '%s'", models[defaultIdx].id.c_str());
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

void MainFrame::OnProviderSelected(wxCommandEvent& /*event*/) {
  int sel = m_providerChoice->GetSelection();
  auto type = (sel == 0) ? fcn::ProviderType::Zen : fcn::ProviderType::Claude;

  auto& zen = zen::ZenClient::Instance();
  if (zen.GetProviderType() == type) return;

  zen.SetProvider(type);

  // Update UI for new provider
  m_apiKeyButton->Show(type == fcn::ProviderType::Zen);
  m_apiKeyButton->GetParent()->Layout();

  if (type == fcn::ProviderType::Zen) {
    wxString savedKey = LoadApiKeyFromKeychain();
    AppendToChat("System", savedKey.IsEmpty()
      ? "Switched to OpenCode Zen (anonymous)..."
      : "Switched to OpenCode Zen...");
    zen.Connect(savedKey.ToStdString());
  } else {
    AppendToChat("System", "Switched to Claude (ACP)...");
    zen.Connect();
  }
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
