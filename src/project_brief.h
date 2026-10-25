#pragma once
#include "streaming_web_request.h"
#include <wx/event.h>
#include <wx/string.h>
#include <functional>
#include <string>
#include <vector>

// A project brief is a few paragraphs on what a folder's project is, its
// layout and conventions, and the current work. A model writes it when the
// user starts a new thread in a folder without one; every later new thread
// there gets it in its system prompt (SessionStore keeps it).

// The model that writes it: an OpenAI-style chat completions endpoint.
struct BriefModel {
    std::string url;
    std::string model;
    std::string apiKey;     // empty: no Authorization header
    bool deepseek = false;  // DeepSeek: thinking off, this is a writing task
    wxString label;         // named in the notices
};

// What the model writes the brief from: the folder's top-level entries, its
// README, the AGENTS.md text and the titles of the folder's threads.
std::string BriefMaterial(const std::string& cwd, const std::string& agents,
                          const std::vector<std::string>& threadTitles);

// Asks `model` for the brief. `done` runs on `target` with the brief
// (trimmed), or an empty brief and why it failed.
StreamingWebRequest WriteBrief(wxEvtHandler* target, const BriefModel& model,
                               const std::string& material,
                               std::function<void(std::string brief, std::string error)> done);
