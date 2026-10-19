#pragma once
#include <wx/string.h>
#include <wx/window.h>
#include <optional>
#include <string>
#include <vector>

// A project brief is a few paragraphs on what a folder's project is, its
// layout and conventions, and the current work. Every thread in the folder
// gets it in its system prompt (SessionStore keeps it).

// The model "Generate" asks: an OpenAI-style chat completions endpoint.
struct BriefModel {
    std::string url;
    std::string model;
    std::string apiKey;     // empty: no Authorization header
    bool deepseek = false;  // DeepSeek: thinking off, this is a writing task
    wxString label;         // shown while generating
};

// What the model writes the brief from: the folder's top-level entries, its
// README, the AGENTS.md text and the titles of the folder's threads.
std::string BriefMaterial(const std::string& cwd, const std::string& agents,
                          const std::vector<std::string>& threadTitles);

// Modal "Project brief" dialog for `folder`: edit the brief, or let "Generate"
// write it from `material`. Returns the text to save (trimmed, empty for no
// brief) when the user pressed Save, std::nullopt when cancelled.
std::optional<std::string> ShowBriefDialog(wxWindow* parent, const wxString& folder,
                                           const std::string& brief,
                                           const BriefModel& model,
                                           const std::string& material);
