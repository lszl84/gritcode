#include "zen_client.h"
#include <wx/wx.h>
#include <wx/log.h>
#include <algorithm>

namespace fcn::zen {

wxDEFINE_EVENT(ZEN_MESSAGE_RECEIVED, wxCommandEvent);
wxDEFINE_EVENT(ZEN_STREAM_CHUNK, wxCommandEvent);
wxDEFINE_EVENT(ZEN_ERROR_OCCURRED, wxCommandEvent);
wxDEFINE_EVENT(ZEN_CONNECTED, wxCommandEvent);
wxDEFINE_EVENT(ZEN_DISCONNECTED, wxCommandEvent);
wxDEFINE_EVENT(ZEN_MODELS_LOADED, wxCommandEvent);

ZenClient::ZenClient() {
  httpClient_ = std::make_unique<network::HttpClient>();
}

ZenClient::~ZenClient() {
  Disconnect();
}

ZenClient& ZenClient::Instance() {
  static ZenClient instance;
  return instance;
}

bool ZenClient::Connect(const std::string& apiKey) {
  wxLogMessage("ZenClient::Connect: Starting...");
  
  if (connected_) {
    wxLogMessage("ZenClient::Connect: Already connected");
    wxCommandEvent event(ZEN_CONNECTED);
    event.SetString("Already connected");
    wxPostEvent(this, event);
    return true;
  }
  
  if (!httpClient_->Initialize()) {
    wxLogError("ZenClient::Connect: Failed to initialize HTTP client");
    wxCommandEvent event(ZEN_ERROR_OCCURRED);
    event.SetString("Failed to initialize HTTP client - wxWebRequest not available");
    wxPostEvent(this, event);
    return false;
  }
  
  wxLogMessage("ZenClient::Connect: HTTP client initialized");
  
  httpClient_->SetApiKey(apiKey);
  httpClient_->SetBaseUrl("https://opencode.ai/zen/v1");
  
  connected_ = true;
  
  wxLogMessage("ZenClient::Connect: Connected successfully (anonymous=%d)", IsAnonymous());
  
  wxCommandEvent event(ZEN_CONNECTED);
  event.SetString(IsAnonymous() ? "Connected anonymously (no API key)" : "Connected with API key");
  wxPostEvent(this, event);
  
  // Fetch models after connecting
  wxLogMessage("ZenClient::Connect: Fetching models...");
  FetchModels();
  
  return true;
}

void ZenClient::Disconnect() {
  if (httpClient_) {
    httpClient_->Shutdown();
  }
  connected_ = false;
  cachedModels_.clear();
  
  wxCommandEvent event(ZEN_DISCONNECTED);
  wxPostEvent(this, event);
}

bool ZenClient::IsConnected() const {
  return connected_;
}

void ZenClient::FetchModels() {
  if (!connected_) return;
  
  httpClient_->FetchModels([this](const std::vector<network::ModelInfo>& models) {
    this->OnModelsReceived(models);
  });
}

void ZenClient::OnModelsReceived(const std::vector<network::ModelInfo>& models) {
  cachedModels_ = models;
  
  wxLogMessage("ZenClient::OnModelsReceived: Received %zu models from API", models.size());
  
  // If we got no models from the API, use default free models
  if (cachedModels_.empty()) {
    wxLogWarning("ZenClient::OnModelsReceived: No models received, using fallback models");
    cachedModels_ = {
      {"big-pickle", "Big Pickle", true, 100},
      {"mimo-v2-flash-free", "MiMo V2 Flash Free", true, 100},
      {"nemotron-3-super-free", "Nemotron 3 Super Free", true, 100},
      {"minimax-m2.5-free", "MiniMax M2.5 Free", true, 100}
    };
  }
  
  // Set default model to first free one if anonymous
  if (httpClient_->IsAnonymous()) {
    auto freeModels = GetFreeModels();
    if (!freeModels.empty() && activeModel_.empty()) {
      activeModel_ = freeModels[0].id;
      wxLogMessage("ZenClient::OnModelsReceived: Set active model to '%s'", activeModel_.c_str());
    }
  }
  
  // Notify UI that models are loaded
  wxLogMessage("ZenClient::OnModelsReceived: Firing ZEN_MODELS_LOADED event");
  wxCommandEvent event(ZEN_MODELS_LOADED);
  wxPostEvent(this, event);
}

std::vector<network::ModelInfo> ZenClient::GetModels() const {
  return cachedModels_;
}

std::vector<network::ModelInfo> ZenClient::GetFreeModels() const {
  std::vector<network::ModelInfo> freeModels;
  
  wxLogMessage("ZenClient::GetFreeModels: Checking %zu cached models", cachedModels_.size());
  
  for (const auto& model : cachedModels_) {
    bool isFree = model.allowAnonymous || 
                  model.id.find("free") != std::string::npos ||
                  model.id.find("big-pickle") != std::string::npos;
    
    wxLogMessage("ZenClient::GetFreeModels: Model '%s' (allowAnonymous=%d, isFree=%d)", 
                 model.id.c_str(), model.allowAnonymous, isFree);
    
    if (isFree) {
      freeModels.push_back(model);
    }
  }
  
  wxLogMessage("ZenClient::GetFreeModels: Returning %zu free models", freeModels.size());
  
  return freeModels;
}

void ZenClient::SendMessage(const std::string& model, const std::string& message) {
  wxLogMessage("ZenClient::SendMessage: model='%s', message length=%zu, history=%zu msgs",
               model.c_str(), message.length(), conversationHistory_.size());

  if (!connected_) {
    wxLogError("ZenClient::SendMessage: Not connected!");
    wxCommandEvent event(ZEN_ERROR_OCCURRED);
    event.SetString("Not connected to Zen");
    wxPostEvent(this, event);
    return;
  }

  // Add the user message to conversation history
  conversationHistory_.push_back({"user", message});

  network::ChatRequest request;
  request.model = model.empty() ? activeModel_ : model;
  request.messages = conversationHistory_;
  request.stream = false;

  wxLogMessage("ZenClient::SendMessage: Using model='%s' (STREAMING MODE), sending %zu messages",
               request.model.c_str(), request.messages.size());

  // Use streaming for better UX
  httpClient_->SendStreamingChatRequest(request,
    [this](const std::string& chunk, bool isThinking) {
      // Called for each chunk of text as it arrives
      // Send just the delta chunk for incremental UI append
      wxCommandEvent event(ZEN_STREAM_CHUNK);
      event.SetString(wxString::FromUTF8(chunk.c_str(), chunk.length()));
      event.SetExtraLong(isThinking ? 1 : 0);
      wxPostEvent(this, event);
    },
    [this](const network::ChatResponse& response) {
      // Called when streaming is complete
      wxLogMessage("ZenClient::SendMessage: Streaming complete, content length=%zu",
                   response.content.length());
      this->OnChatResponse(response);
    }
  );
}

void ZenClient::ClearConversation() {
  conversationHistory_.clear();
  wxLogMessage("ZenClient::ClearConversation: History cleared");
}

void ZenClient::OnChatResponse(const network::ChatResponse& response) {
  wxLogMessage("ZenClient::OnChatResponse: error=%d, content length=%zu",
               response.error, response.content.length());

  if (response.error) {
    // Remove the user message that caused the error so it can be retried
    if (!conversationHistory_.empty() && conversationHistory_.back().role == "user") {
      conversationHistory_.pop_back();
    }
    wxCommandEvent event(ZEN_ERROR_OCCURRED);
    event.SetString(wxString::FromUTF8(response.errorMessage));
    wxPostEvent(this, event);
  } else {
    // Store assistant response in conversation history
    if (!response.content.empty()) {
      conversationHistory_.push_back({"assistant", response.content});
      wxLogMessage("ZenClient::OnChatResponse: History now has %zu messages",
                   conversationHistory_.size());
    }

    wxCommandEvent event(ZEN_MESSAGE_RECEIVED);
    wxString wxContent = wxString::FromUTF8(response.content.c_str(), response.content.length());
    event.SetString(wxContent);
    event.SetExtraLong(response.totalTokens);
    wxPostEvent(this, event);
  }
}

void ZenClient::SetActiveModel(const std::string& modelId) {
  activeModel_ = modelId;
}

std::string ZenClient::GetActiveModel() const {
  return activeModel_;
}

bool ZenClient::IsAnonymous() const {
  return httpClient_->IsAnonymous();
}

void ZenClient::SetJsonLogCallback(network::JsonLogCallback callback) {
  httpClient_->SetJsonLogCallback(callback);
}

} // namespace fcn::zen
