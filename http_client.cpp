#include "http_client.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iostream>
#include <wx/log.h>
#include <wx/sstream.h>

namespace fcn::network {

using json = nlohmann::json;

HttpClient::HttpClient() : initialized_(false) {
  // Bind to web request events
  Bind(wxEVT_WEBREQUEST_STATE, &HttpClient::OnRequestStateChanged, this);
}

HttpClient::~HttpClient() {
  Shutdown();
}

bool HttpClient::Initialize() {
  if (initialized_) return true;
  
  // wxWebRequest is initialized automatically by wxWidgets
  initialized_ = true;
  return true;
}

void HttpClient::Shutdown() {
  wxLogMessage("HttpClient::Shutdown: Shutting down...");
  
  // Clear callbacks first to prevent any late events from invoking them
  modelsCallback_ = nullptr;
  chatCallback_ = nullptr;
  
  // Just cancel if active - don't reset the request object
  if (currentRequest_.IsOk() && currentRequest_.GetState() == wxWebRequest::State_Active) {
    wxLogMessage("HttpClient::Shutdown: Cancelling active request");
    currentRequest_.Cancel();
  }
  
  initialized_ = false;
  wxLogMessage("HttpClient::Shutdown: Done");
}

void HttpClient::SetBaseUrl(const std::string& url) {
  baseUrl_ = url;
}

void HttpClient::SetApiKey(const std::string& apiKey) {
  apiKey_ = apiKey;
}

void HttpClient::SetTimeout(int seconds) {
  timeout_ = seconds;
}

void HttpClient::SetJsonLogCallback(JsonLogCallback callback) {
  jsonLogCallback_ = callback;
}

std::string HttpClient::BuildRequestJson(const ChatRequest& request) {
  json j;
  j["model"] = request.model;
  j["stream"] = request.stream;
  
  if (request.temperature.has_value()) {
    j["temperature"] = request.temperature.value();
  }
  
  if (request.maxTokens.has_value()) {
    j["max_tokens"] = request.maxTokens.value();
  }
  
  json messages = json::array();
  for (const auto& msg : request.messages) {
    messages.push_back({
      {"role", msg.role},
      {"content", msg.content}
    });
  }
  j["messages"] = messages;
  
  return j.dump();
}

ChatResponse HttpClient::ParseResponse(const std::string& responseJson) {
  ChatResponse response;
  
  wxLogMessage("ParseResponse: Got JSON length=%zu", responseJson.length());
  
  if (responseJson.empty()) {
    response.error = true;
    response.errorMessage = "Empty response from server";
    wxLogError("ParseResponse: Empty JSON input!");
    return response;
  }
  
  try {
    json j = json::parse(responseJson);
    
    wxLogMessage("ParseResponse: Parsed JSON successfully");
    
    if (j.contains("error")) {
      response.error = true;
      if (j["error"].contains("message")) {
        response.errorMessage = j["error"]["message"].get<std::string>();
      } else {
        response.errorMessage = "Unknown error";
      }
      wxLogError("ParseResponse: API returned error: %s", response.errorMessage.c_str());
      return response;
    }
    
    response.id = j.value("id", "");
    
    if (j.contains("choices") && !j["choices"].empty()) {
      const auto& choice = j["choices"][0];
      if (choice.contains("message")) {
        response.content = choice["message"].value("content", "");
      }
      response.finishReason = choice.value("finish_reason", "");
    }
    
    if (j.contains("usage")) {
      const auto& usage = j["usage"];
      response.promptTokens = usage.value("prompt_tokens", 0);
      response.completionTokens = usage.value("completion_tokens", 0);
      response.totalTokens = usage.value("total_tokens", 0);
    }
    
    wxLogMessage("ParseResponse: Success - content length=%zu, tokens=%d", 
                 response.content.length(), response.totalTokens);
    
  } catch (const json::exception& e) {
    response.error = true;
    response.errorMessage = "JSON parsing error: " + std::string(e.what());
    wxLogError("ParseResponse: JSON exception: %s", e.what());
    wxLogError("ParseResponse: JSON was: %s", responseJson.substr(0, 200).c_str());
  }
  
  return response;
}

std::vector<ModelInfo> HttpClient::ParseModels(const std::string& responseJson) {
  std::vector<ModelInfo> models;
  
  try {
    json j = json::parse(responseJson);
    
    if (j.contains("data") && j["data"].is_array()) {
      for (const auto& model : j["data"]) {
        ModelInfo info;
        info.id = model.value("id", "");
        info.name = model.value("name", info.id);
        
        // Check for free/anonymous models
        if (model.contains("allow_anonymous")) {
          info.allowAnonymous = model["allow_anonymous"].get<bool>();
        }
        if (model.contains("rate_limit")) {
          info.rateLimit = model["rate_limit"].get<int>();
        }
        
        models.push_back(info);
      }
    }
  } catch (const json::exception& e) {
    // Failed to parse
  }
  
  return models;
}

void HttpClient::OnRequestStateChanged(wxWebRequestEvent& event) {
  wxLogMessage("HttpClient: Request state changed to %d", static_cast<int>(event.GetState()));
  
  // Verify this event is from the current request (not an old cancelled one)
  if (event.GetRequest().GetId() != currentRequest_.GetId()) {
    wxLogMessage("HttpClient: Ignoring event - request ID mismatch (event=%d, current=%d)",
                 event.GetRequest().GetId(), currentRequest_.GetId());
    return;
  }
  
  if (event.GetState() == wxWebRequest::State_Completed) {
    auto response = event.GetResponse();
    wxLogMessage("HttpClient: Request completed with status %d", response.GetStatus());
    
    if (response.GetStatus() == 200) {
      wxString responseStr = response.AsString();
      // Use ToUTF8() to preserve all bytes including nulls, then convert to std::string
      wxCharBuffer utf8Buffer = responseStr.ToUTF8();
      std::string responseBody(utf8Buffer.data(), utf8Buffer.length());
      
      wxLogMessage("HttpClient: Got response, length=%zu, wxString length=%zu", 
                   responseBody.length(), responseStr.length());
      
      // Log the JSON response
      if (jsonLogCallback_ && !responseBody.empty()) {
        jsonLogCallback_("RECV", responseBody);
      }
      
      // If AsString is empty, try reading from stream
      if (responseBody.empty()) {
        wxLogWarning("HttpClient: AsString() returned empty, trying stream...");
        auto stream = response.GetStream();
        if (stream) {
          wxStringOutputStream outStream;
          stream->Read(outStream);
          wxString streamStr = outStream.GetString();
          wxCharBuffer utf8Buffer = streamStr.ToUTF8();
          responseBody = std::string(utf8Buffer.data(), utf8Buffer.length());
          wxLogMessage("HttpClient: Read %zu bytes from stream", responseBody.length());
        }
      }
      
      // Debug: check which callback is set
      wxLogMessage("HttpClient: modelsCallback_=%p, chatCallback_=%p", 
                   modelsCallback_ ? &modelsCallback_ : nullptr,
                   chatCallback_ ? &chatCallback_ : nullptr);
      
      // Check which callback to invoke based on what's set
      if (chatCallback_) {
        wxLogMessage("HttpClient: Processing chat response");
        auto chatResponse = ParseResponse(responseBody);
        auto cb = chatCallback_;
        modelsCallback_ = nullptr;
        chatCallback_ = nullptr;
        cb(chatResponse);
      } else if (modelsCallback_) {
        wxLogMessage("HttpClient: Processing models response");
        auto models = ParseModels(responseBody);
        wxLogMessage("HttpClient: Parsed %zu models", models.size());
        auto cb = modelsCallback_;
        modelsCallback_ = nullptr;
        chatCallback_ = nullptr;
        cb(models);
      } else {
        wxLogWarning("HttpClient: No callback set for response!");
      }
    } else {
      // Error response
      std::string errorMsg = "HTTP Error " + std::to_string(response.GetStatus());
      wxLogError("HttpClient: %s", errorMsg.c_str());
      
      if (chatCallback_) {
        ChatResponse error;
        error.error = true;
        error.errorMessage = errorMsg;
        auto cb = chatCallback_;
        modelsCallback_ = nullptr;
        chatCallback_ = nullptr;
        cb(error);
      } else if (modelsCallback_) {
        auto cb = modelsCallback_;
        modelsCallback_ = nullptr;
        chatCallback_ = nullptr;
        cb({}); // Empty models list
      }
    }
  } else if (event.GetState() == wxWebRequest::State_Failed) {
    // Request failed - get more details
    wxString errorStr = event.GetErrorDescription();
    std::string errorMsg = "Request failed";
    if (!errorStr.IsEmpty()) {
      errorMsg += ": " + errorStr.ToStdString();
    } else {
      errorMsg += " (network error or timeout)";
    }
    wxLogError("HttpClient: %s", errorMsg.c_str());
    
    if (chatCallback_) {
      ChatResponse error;
      error.error = true;
      error.errorMessage = errorMsg;
      auto cb = chatCallback_;
      modelsCallback_ = nullptr;
      chatCallback_ = nullptr;
      cb(error);
    } else if (modelsCallback_) {
      auto cb = modelsCallback_;
      modelsCallback_ = nullptr;
      chatCallback_ = nullptr;
      cb({});
    }
  } else if (event.GetState() == wxWebRequest::State_Cancelled) {
    wxLogMessage("HttpClient: Request was cancelled");
    // Don't call callbacks for cancelled requests - they were intentionally stopped
  } else if (event.GetState() == wxWebRequest::State_Active) {
    wxLogMessage("HttpClient: Request is active...");
  } else if (event.GetState() == wxWebRequest::State_Idle) {
    wxLogMessage("HttpClient: Request is idle");
  } else if (event.GetState() == wxWebRequest::State_Unauthorized) {
    wxLogError("HttpClient: Request unauthorized");
    
    if (chatCallback_) {
      ChatResponse error;
      error.error = true;
      error.errorMessage = "Unauthorized - check API key";
      auto cb = chatCallback_;
      modelsCallback_ = nullptr;
      chatCallback_ = nullptr;
      cb(error);
    } else if (modelsCallback_) {
      auto cb = modelsCallback_;
      modelsCallback_ = nullptr;
      chatCallback_ = nullptr;
      cb({});
    }
  }
}

void HttpClient::FetchModels(ModelsCallback callback) {
  wxLogMessage("HttpClient::FetchModels: Starting...");
  
  if (!initialized_) {
    wxLogError("HttpClient::FetchModels: Not initialized");
    callback({});
    return;
  }
  
  // Check if there's an existing active request - if so, cancel it
  if (currentRequest_.IsOk() && currentRequest_.GetState() == wxWebRequest::State_Active) {
    wxLogWarning("HttpClient::FetchModels: Cancelling existing active request");
    currentRequest_.Cancel();
  }
  
  std::string url = baseUrl_ + "/models";
  wxLogMessage("HttpClient::FetchModels: URL=%s", url.c_str());
  
  currentRequest_ = wxWebSession::GetDefault().CreateRequest(this, wxString::FromUTF8(url));
  
  if (!currentRequest_.IsOk()) {
    wxLogError("HttpClient::FetchModels: Failed to create request");
    callback({});
    return;
  }
  
  currentRequest_.SetHeader("Content-Type", "application/json");
  
  if (!apiKey_.empty()) {
    currentRequest_.SetHeader("Authorization", "Bearer " + wxString::FromUTF8(apiKey_));
  }
  
  modelsCallback_ = callback;
  chatCallback_ = nullptr;
  
  wxLogMessage("HttpClient::FetchModels: Starting request...");
  currentRequest_.Start();
}

void HttpClient::SendChatRequest(const ChatRequest& request, ChatCallback callback) {
  wxLogMessage("HttpClient::SendChatRequest: Starting...");
  
  if (!initialized_) {
    wxLogError("HttpClient::SendChatRequest: Not initialized");
    ChatResponse error;
    error.error = true;
    error.errorMessage = "HTTP client not initialized";
    callback(error);
    return;
  }
  
  // Check if there's an existing active request - if so, reject this one
  if (currentRequest_.IsOk() && currentRequest_.GetState() == wxWebRequest::State_Active) {
    wxLogWarning("HttpClient::SendChatRequest: Another request is already active, rejecting");
    ChatResponse error;
    error.error = true;
    error.errorMessage = "Another request is in progress, please wait";
    callback(error);
    return;
  }
  
  std::string url = baseUrl_ + "/chat/completions";
  std::string requestBody = BuildRequestJson(request);
  
  wxLogMessage("HttpClient::SendChatRequest: URL=%s, body length=%zu", url.c_str(), requestBody.length());
  
  // Log the JSON being sent
  if (jsonLogCallback_) {
    jsonLogCallback_("SEND", requestBody);
  }
  
  currentRequest_ = wxWebSession::GetDefault().CreateRequest(this, wxString::FromUTF8(url));
  
  if (!currentRequest_.IsOk()) {
    wxLogError("HttpClient::SendChatRequest: Failed to create request");
    ChatResponse error;
    error.error = true;
    error.errorMessage = "Failed to create web request";
    callback(error);
    return;
  }
  
  currentRequest_.SetHeader("Content-Type", "application/json");
  
  if (!apiKey_.empty()) {
    currentRequest_.SetHeader("Authorization", "Bearer " + wxString::FromUTF8(apiKey_));
  }
  
  // Set the data to POST - this automatically makes it a POST request
  wxLogMessage("HttpClient::SendChatRequest: Setting POST data...");
  currentRequest_.SetData(wxString::FromUTF8(requestBody), "application/json");
  
  chatCallback_ = callback;
  modelsCallback_ = nullptr;
  
  wxLogMessage("HttpClient::SendChatRequest: Starting request...");
  currentRequest_.Start();
}

void HttpClient::SendStreamingChatRequest(
  const ChatRequest& request,
  std::function<void(const std::string& chunk)> onChunk,
  std::function<void(const ChatResponse& response)> onComplete
) {
  // For now, just call the regular request and pass the whole content as one chunk
  SendChatRequest(request, [onChunk, onComplete](const ChatResponse& response) {
    if (!response.content.empty()) {
      onChunk(response.content);
    }
    onComplete(response);
  });
}

} // namespace fcn::network
