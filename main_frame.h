#pragma once

#include <wx/wx.h>
#include <wx/splitter.h>
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
  void OnZenError(wxCommandEvent& event);
  void OnZenModelsLoaded(wxCommandEvent& event);
  
  void UpdateConnectionStatus();
  void PopulateModelList();
  void AppendToChat(const wxString& sender, const wxString& message);
  wxString LoadApiKeyFromKeychain();
  bool SaveApiKeyToKeychain(const wxString& key);
  bool ClearApiKeyFromKeychain();
  
  wxPanel* m_sidebarPanel = nullptr;
  wxPanel* m_mainPanel = nullptr;
  wxSplitterWindow* m_splitter = nullptr;
  
  // Chat UI elements
  StreamingTextCtrl* m_chatDisplay = nullptr;
  wxTextCtrl* m_messageInput = nullptr;
  wxButton* m_sendButton = nullptr;
  wxChoice* m_modelChoice = nullptr;
  wxStaticText* m_statusLabel = nullptr;
  
  wxDECLARE_EVENT_TABLE();
};

enum class MenuID : int {
  Exit = wxID_EXIT,
  About = wxID_ABOUT,
  Connect = 1000,
  Disconnect,
  Settings,
  SendMessage = 2000,
  SetApiKey = 3000
};

} // namespace fcn::ui
