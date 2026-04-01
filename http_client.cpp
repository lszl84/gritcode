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
  Bind(wxEVT_WEBREQUEST_DATA, &HttpClient::OnRequestData, this);
  retryTimer_.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { RetryCurrentRequest(); });
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
    json m;
    m["role"] = msg.role;

    if (msg.role == "tool") {
      // Tool result message
      m["tool_call_id"] = msg.toolCallId;
      m["content"] = msg.content;
    } else if (msg.role == "assistant" && !msg.toolCalls.empty()) {
      // Assistant message with tool calls
      m["content"] = msg.content.empty() ? json(nullptr) : json(msg.content);
      json tcArr = json::array();
      for (const auto& tc : msg.toolCalls) {
        tcArr.push_back({
          {"id", tc.id},
          {"type", "function"},
          {"function", {{"name", tc.name}, {"arguments", tc.arguments}}}
        });
      }
      m["tool_calls"] = tcArr;
    } else {
      m["content"] = msg.content;
    }

    messages.push_back(m);
  }
  j["messages"] = messages;

  // Tool definitions
  if (!request.tools.empty()) {
    json toolsArr = json::array();
    for (const auto& t : request.tools) {
      toolsArr.push_back({
        {"type", "function"},
        {"function", {
          {"name", t.name},
          {"description", t.description},
          {"parameters", json::parse(t.parametersJson)}
        }}
      });
    }
    j["tools"] = toolsArr;
    j["tool_choice"] = "auto";
  }

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
    
    // Handle streaming completion
    if (isStreaming_) {
      wxLogMessage("HttpClient: Streaming request completed");
      
      // Process any remaining data in buffer
      if (!sseBuffer_.empty()) {
        ProcessSSEChunk("\n\n"); // Force process remaining buffer
      }
      
      ChatResponse finalResponse;
      finalResponse.content = accumulatedContent_;
      finalResponse.finishReason = streamFinishReason_.empty() ? "stop" : streamFinishReason_;
      finalResponse.toolCalls = std::move(pendingToolCalls_);
      
      if (response.GetStatus() != 200) {
        // Retry on 429 rate limit
        if (response.GetStatus() == 429 && retryCount_ < MAX_RETRIES) {
          retryCount_++;
          int delayMs = 1000 * retryCount_;  // 1s, 2s, 3s backoff
          wxLogMessage("HttpClient: 429 rate limited, retry %d/%d in %dms",
                       retryCount_, MAX_RETRIES, delayMs);
          sseBuffer_.clear();
          accumulatedContent_.clear();
          retryTimer_.StartOnce(delayMs);
          return;
        }
        finalResponse.error = true;
        finalResponse.errorMessage = "HTTP Error " + std::to_string(response.GetStatus());
      }

      if (streamingCompleteCallback_) {
        auto cb = streamingCompleteCallback_;
        streamingCompleteCallback_ = nullptr;
        streamingChunkCallback_ = nullptr;
        isStreaming_ = false;
        cb(finalResponse);
      }
      return;
    }
    
    // Non-streaming handling (existing code)
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
      int status = response.GetStatus();
      std::string errorMsg = "HTTP Error " + std::to_string(status);
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
    wxString errorDesc = event.GetErrorDescription();
    wxLogError("HttpClient: Request failed: %s", errorDesc.c_str());
    
    // Handle streaming failure
    if (isStreaming_) {
      ChatResponse error;
      error.error = true;
      error.errorMessage = "Request failed: " + std::string(errorDesc.ToUTF8());
      
      if (streamingCompleteCallback_) {
        auto cb = streamingCompleteCallback_;
        streamingCompleteCallback_ = nullptr;
        streamingChunkCallback_ = nullptr;
        isStreaming_ = false;
        cb(error);
      }
      return;
    }
    
    if (chatCallback_) {
      ChatResponse error;
      error.error = true;
      error.errorMessage = "Request failed: " + std::string(errorDesc.ToUTF8());
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
    // Handle streaming cancellation
    if (isStreaming_) {
      ChatResponse cancelled;
      cancelled.error = true;
      cancelled.errorMessage = "Request cancelled";
      cancelled.content = accumulatedContent_; // Return what we got so far
      
      if (streamingCompleteCallback_) {
        auto cb = streamingCompleteCallback_;
        streamingCompleteCallback_ = nullptr;
        streamingChunkCallback_ = nullptr;
        isStreaming_ = false;
        cb(cancelled);
      }
    }
    // Don't call non-streaming callbacks for cancelled requests - they were intentionally stopped
  } else if (event.GetState() == wxWebRequest::State_Active) {
    wxLogMessage("HttpClient: Request is active...");
  } else if (event.GetState() == wxWebRequest::State_Idle) {
    wxLogMessage("HttpClient: Request is idle");
  } else if (event.GetState() == wxWebRequest::State_Unauthorized) {
    wxLogError("HttpClient: Request unauthorized");
    
    // Handle streaming unauthorized
    if (isStreaming_) {
      ChatResponse error;
      error.error = true;
      error.errorMessage = "Unauthorized - check API key";
      
      if (streamingCompleteCallback_) {
        auto cb = streamingCompleteCallback_;
        streamingCompleteCallback_ = nullptr;
        streamingChunkCallback_ = nullptr;
        isStreaming_ = false;
        cb(error);
      }
      return;
    }
    
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

void HttpClient::RetryCurrentRequest() {
  wxLogMessage("HttpClient::RetryCurrentRequest: Attempt %d/%d", retryCount_, MAX_RETRIES);

  std::string url = baseUrl_ + "/chat/completions";
  currentRequest_ = wxWebSession::GetDefault().CreateRequest(this, wxString::FromUTF8(url));

  if (!currentRequest_.IsOk()) {
    wxLogError("HttpClient::RetryCurrentRequest: Failed to create request");
    if (streamingCompleteCallback_) {
      ChatResponse error;
      error.error = true;
      error.errorMessage = "Retry failed: could not create request";
      auto cb = streamingCompleteCallback_;
      streamingCompleteCallback_ = nullptr;
      streamingChunkCallback_ = nullptr;
      isStreaming_ = false;
      cb(error);
    }
    return;
  }

  currentRequest_.SetStorage(wxWebRequest::Storage_None);
  currentRequest_.SetHeader("Content-Type", "application/json");
  currentRequest_.SetHeader("Accept", "text/event-stream");
  if (!apiKey_.empty()) {
    currentRequest_.SetHeader("Authorization", "Bearer " + wxString::FromUTF8(apiKey_));
  }
  currentRequest_.SetData(wxString::FromUTF8(pendingRequestBody_), "application/json");

  sseBuffer_.clear();
  accumulatedContent_.clear();
  pendingToolCalls_.clear();
  streamFinishReason_.clear();

  wxLogMessage("HttpClient::RetryCurrentRequest: Starting request...");
  currentRequest_.Start();
}

void HttpClient::SendStreamingChatRequest(
  const ChatRequest& request,
  std::function<void(const std::string& chunk, bool isThinking)> onChunk,
  std::function<void(const ChatResponse& response)> onComplete
) {
  wxLogMessage("HttpClient::SendStreamingChatRequest: Starting...");
  
  if (!initialized_) {
    wxLogError("HttpClient::SendStreamingChatRequest: Not initialized");
    ChatResponse error;
    error.error = true;
    error.errorMessage = "HTTP client not initialized";
    onComplete(error);
    return;
  }
  
  // Check if there's an existing active request - if so, cancel it
  if (currentRequest_.IsOk() && currentRequest_.GetState() == wxWebRequest::State_Active) {
    wxLogWarning("HttpClient::SendStreamingChatRequest: Cancelling existing active request");
    currentRequest_.Cancel();
  }
  
  std::string url = baseUrl_ + "/chat/completions";
  
  // Build request with streaming enabled
  ChatRequest streamRequest = request;
  streamRequest.stream = true;
  std::string requestBody = BuildRequestJson(streamRequest);
  
  wxLogMessage("HttpClient::SendStreamingChatRequest: URL=%s, body length=%zu", url.c_str(), requestBody.length());
  
  // Log the JSON being sent
  if (jsonLogCallback_) {
    jsonLogCallback_("SEND", requestBody);
  }
  
  currentRequest_ = wxWebSession::GetDefault().CreateRequest(this, wxString::FromUTF8(url));
  
  if (!currentRequest_.IsOk()) {
    wxLogError("HttpClient::SendStreamingChatRequest: Failed to create request");
    ChatResponse error;
    error.error = true;
    error.errorMessage = "Failed to create web request";
    onComplete(error);
    return;
  }
  
  // KEY: Set storage to None to receive data via wxEVT_WEBREQUEST_DATA events
  currentRequest_.SetStorage(wxWebRequest::Storage_None);
  
  currentRequest_.SetHeader("Content-Type", "application/json");
  currentRequest_.SetHeader("Accept", "text/event-stream");
  
  if (!apiKey_.empty()) {
    currentRequest_.SetHeader("Authorization", "Bearer " + wxString::FromUTF8(apiKey_));
  }
  
  // Set the data to POST
  wxLogMessage("HttpClient::SendStreamingChatRequest: Setting POST data...");
  currentRequest_.SetData(wxString::FromUTF8(requestBody), "application/json");
  
  // Store streaming callbacks and request body for retries
  streamingChunkCallback_ = onChunk;
  streamingCompleteCallback_ = onComplete;
  pendingRequestBody_ = requestBody;
  sseBuffer_.clear();
  accumulatedContent_.clear();
  pendingToolCalls_.clear();
  streamFinishReason_.clear();
  isStreaming_ = true;
  retryCount_ = 0;
  
  // Clear other callbacks
  modelsCallback_ = nullptr;
  chatCallback_ = nullptr;
  
  wxLogMessage("HttpClient::SendStreamingChatRequest: Starting request...");
  currentRequest_.Start();
}

void HttpClient::OnRequestData(wxWebRequestEvent& event) {
  if (!isStreaming_) {
    wxLogMessage("HttpClient::OnRequestData: Ignoring - not in streaming mode");
    return;
  }
  
  // Get the data chunk
  const void* buffer = event.GetDataBuffer();
  size_t size = event.GetDataSize();
  
  wxLogMessage("HttpClient::OnRequestData: Event received, buffer=%p, size=%zu", buffer, size);
  
  if (buffer && size > 0) {
    // Convert to string
    std::string chunk(static_cast<const char*>(buffer), size);
    wxLogMessage("HttpClient::OnRequestData: Processing chunk: '%s'", 
                 wxString::FromUTF8(chunk.substr(0, 100)).c_str());
    
    // Process as SSE
    ProcessSSEChunk(chunk);
  } else {
    wxLogWarning("HttpClient::OnRequestData: Empty buffer or zero size");
  }
}

void HttpClient::ProcessSSEChunk(const std::string& chunk) {
  wxLogMessage("HttpClient::ProcessSSEChunk: Appending %zu bytes to buffer (current size: %zu)", 
               chunk.length(), sseBuffer_.length());
  
  // Append to buffer
  sseBuffer_ += chunk;
  
  wxLogMessage("HttpClient::ProcessSSEChunk: Buffer now %zu bytes, looking for events...", sseBuffer_.length());
  
  // SSE format: lines starting with "data: " separated by double newlines
  // Process complete events (separated by \n\n)
  size_t pos = 0;
  int eventsProcessed = 0;
  while ((pos = sseBuffer_.find("\n\n")) != std::string::npos) {
    std::string event = sseBuffer_.substr(0, pos);
    sseBuffer_.erase(0, pos + 2);
    eventsProcessed++;
    
    wxLogMessage("HttpClient::ProcessSSEChunk: Processing event #%d, length=%zu", 
                 eventsProcessed, event.length());
    
    // Parse the event
    std::istringstream stream(event);
    std::string line;
    std::string data;
    
    while (std::getline(stream, line)) {
      // Remove trailing \r if present (for CRLF)
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      
      wxLogMessage("HttpClient::ProcessSSEChunk: Line: '%s'", 
                   wxString::FromUTF8(line).c_str());
      
      // Check if line starts with "data: "
      if (line.substr(0, 6) == "data: ") {
        data = line.substr(6);
        
        wxLogMessage("HttpClient::ProcessSSEChunk: Data line found: '%s'", 
                     wxString::FromUTF8(data.substr(0, 50)).c_str());
        
        // Check for [DONE] marker
        if (data == "[DONE]") {
          wxLogMessage("HttpClient::ProcessSSEChunk: Received [DONE] marker");
          continue;
        }
        
        // Parse the JSON data
        try {
          json j = json::parse(data);
          
          wxLogMessage("HttpClient::ProcessSSEChunk: JSON parsed successfully");
          
          // Extract delta content from choices
          if (j.contains("choices") && !j["choices"].empty()) {
            const auto& choice = j["choices"][0];

            // Track finish_reason
            if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
              streamFinishReason_ = choice["finish_reason"].get<std::string>();
            }

            if (choice.contains("delta")) {
              const auto& delta = choice["delta"];
              std::string content;

              // Extract reasoning (thinking) content — providers use different field names
              bool isThinking = false;
              std::string reasoning;
              for (const auto& field : {"reasoning_content", "reasoning", "reasoning_text"}) {
                if (delta.contains(field) && delta[field].is_string()) {
                  reasoning = delta[field].get<std::string>();
                  break;
                }
              }

              // Extract regular content (only if it's a non-empty string)
              if (delta.contains("content") && delta["content"].is_string()) {
                content = delta["content"].get<std::string>();
              }

              // Prefer reasoning over empty content during thinking phase
              if (!reasoning.empty()) {
                content = reasoning;
                isThinking = true;
              }

              if (!content.empty()) {
                if (!isThinking) {
                  accumulatedContent_ += content;
                }
                if (streamingChunkCallback_) {
                  streamingChunkCallback_(content, isThinking);
                }
              }

              // Accumulate tool calls (arguments stream across multiple chunks)
              if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                for (const auto& tc : delta["tool_calls"]) {
                  int idx = tc.value("index", 0);
                  if (idx >= static_cast<int>(pendingToolCalls_.size())) {
                    pendingToolCalls_.resize(idx + 1);
                  }
                  if (tc.contains("id") && tc["id"].is_string()) {
                    pendingToolCalls_[idx].id = tc["id"].get<std::string>();
                  }
                  if (tc.contains("function") && tc["function"].is_object()) {
                    const auto& fn = tc["function"];
                    if (fn.contains("name") && fn["name"].is_string()) {
                      pendingToolCalls_[idx].name = fn["name"].get<std::string>();
                    }
                    if (fn.contains("arguments") && fn["arguments"].is_string()) {
                      pendingToolCalls_[idx].arguments += fn["arguments"].get<std::string>();
                    }
                  }
                }
              }
            }
          }
        } catch (const json::exception& e) {
          wxLogWarning("HttpClient::ProcessSSEChunk: Failed to parse JSON: %s", e.what());
        }
      }
    }
  }
  
  wxLogMessage("HttpClient::ProcessSSEChunk: Finished processing, %d events handled, buffer remaining: %zu bytes", 
               eventsProcessed, sseBuffer_.length());
}

} // namespace fcn::network
