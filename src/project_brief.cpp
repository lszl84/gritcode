#include "project_brief.h"
#include "streaming_web_request.h"
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxEntries = 150;
constexpr size_t kMaxFileChars = 12000;

const char* const kBriefPrompt =
    "You write the project brief for a folder a coding assistant works in. "
    "Each new conversation in this folder starts with the brief instead of the "
    "history of earlier conversations, so it should tell the assistant what it "
    "needs to get going: what the project is and does, how it is laid out, how "
    "to build, run and test it, the conventions to follow, and what is being "
    "worked on now (the thread titles hint at that). Write 2 to 4 short "
    "paragraphs, at most about 250 words, as plain text or light markdown "
    "without headings. Use only facts from the material; leave out what it "
    "doesn't tell. Output only the brief.";

std::string Capped(std::string s, size_t max) {
    if (s.size() > max) s = s.substr(0, max) + "\n[truncated]";
    return s;
}

std::string Trimmed(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos) return std::string();
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

WebRequestSpec BriefRequest(const BriefModel& model, const std::string& material) {
    nlohmann::json req;
    req["model"] = model.model;
    req["stream"] = true;
    req["max_tokens"] = 2000;
    if (model.deepseek) req["thinking"] = {{"type", "disabled"}};
    req["messages"] = nlohmann::json::array({
        {{"role", "system"}, {"content", kBriefPrompt}},
        {{"role", "user"}, {"content", material}},
    });
    WebRequestSpec spec;
    spec.url = model.url;
    spec.method = "POST";
    spec.body = req.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    spec.headers.push_back({"Accept", "text/event-stream"});
    if (!model.apiKey.empty())
        spec.headers.push_back({"Authorization", "Bearer " + model.apiKey});
    spec.idleTimeoutSeconds = 60;
    return spec;
}

// The text of one streamed chat completion event, empty for anything else.
std::string DeltaText(const std::string& payload) {
    try {
        const auto j = nlohmann::json::parse(payload);
        const auto& delta = j.at("choices").at(0).at("delta");
        if (delta.contains("content") && delta["content"].is_string())
            return delta["content"].get<std::string>();
    } catch (...) {
        // [DONE], keepalives and other shapes carry no text.
    }
    return std::string();
}

// The message of a JSON error body ({"error": {"message": ...}}), else the
// body itself, shortened.
std::string ErrorDetail(const std::string& body) {
    try {
        const auto j = nlohmann::json::parse(body);
        const auto& err = j.at("error");
        if (err.is_string()) return err.get<std::string>();
        return err.at("message").get<std::string>();
    } catch (...) {
    }
    return Capped(Trimmed(body), 300);
}

}  // namespace

std::string BriefMaterial(const std::string& cwd, const std::string& agents,
                          const std::vector<std::string>& threadTitles) {
    std::vector<std::string> names;
    std::string readme;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(cwd, ec)) {
        const std::string name = e.path().filename().string();
        if (name.starts_with('.')) continue;
        const bool dir = e.is_directory(ec);
        names.push_back(dir ? name + "/" : name);
        std::string stem = e.path().stem().string();
        std::ranges::transform(stem, stem.begin(), ::tolower);
        if (!dir && stem == "readme" && (readme.empty() || name < readme)) readme = name;
    }
    std::ranges::sort(names);

    std::string out = "Folder: " + cwd + "\n\nTop-level entries:\n";
    for (size_t i = 0; i < names.size() && i < kMaxEntries; ++i) out += names[i] + "\n";
    if (names.size() > kMaxEntries) out += "[and more]\n";
    if (!readme.empty()) {
        std::ifstream f(cwd + "/" + readme, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        out += "\n" + readme + ":\n" + Capped(ss.str(), kMaxFileChars) + "\n";
    }
    if (!agents.empty())
        out += "\nAGENTS.md instructions:\n" + Capped(agents, kMaxFileChars) + "\n";
    if (!threadTitles.empty()) {
        out += "\nTitles of the folder's conversation threads, most recent first:\n";
        for (const auto& t : threadTitles) out += "- " + t + "\n";
    }
    return out;
}

std::optional<std::string> ShowBriefDialog(wxWindow* parent, const wxString& folder,
                                           const std::string& brief,
                                           const BriefModel& model,
                                           const std::string& material) {
    constexpr int kWrap = 600;
    wxDialog dlg(parent, wxID_ANY, "Project brief", wxDefaultPosition,
                 wxSize(kWrap + 40, 520), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* intro = new wxStaticText(&dlg, wxID_ANY,
        "What the project in " + folder + " is about: what it is, its layout "
        "and conventions, and the current work. Every thread in this folder "
        "gets it in its system prompt, so a new thread can help without the "
        "history of the others. Leave it empty for no brief.");
    intro->Wrap(kWrap);
    sizer->Add(intro, 0, wxALL, 12);
    auto* text = new wxTextCtrl(&dlg, wxID_ANY, wxString::FromUTF8(brief),
                                wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE);
    sizer->Add(text, 1, wxEXPAND | wxLEFT | wxRIGHT, 12);
    auto* status = new wxStaticText(&dlg, wxID_ANY, wxString());
    sizer->Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* generate = new wxButton(&dlg, wxID_ANY, "Generate");
    generate->SetToolTip("Ask " + model.label + " to write the brief from the "
                         "README, AGENTS.md, the top-level files and the "
                         "thread titles");
    auto* save = new wxButton(&dlg, wxID_OK, "Save");
    buttons->Add(generate);
    buttons->AddStretchSpacer();
    buttons->Add(new wxButton(&dlg, wxID_CANCEL, "Cancel"), 0, wxRIGHT, 8);
    buttons->Add(save);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, 12);
    dlg.SetSizer(sizer);

    // Declared after the dialog, so it is cancelled and joined first; the
    // dialog then drops any of its callbacks still queued.
    StreamingWebRequest request;
    std::string sseBuf;
    wxString before;  // the text Generate replaced, back if it fails

    auto setStatus = [&](const wxString& msg, bool failed) {
        status->SetLabel(msg);
        status->SetForegroundColour(failed ? *wxRED : wxNullColour);
        status->Wrap(kWrap);
        dlg.Layout();
    };
    auto append = [&](std::string_view chunk, bool ended) {
        for (const std::string& payload : TakeSseEvents(sseBuf, chunk, ended))
            text->AppendText(wxString::FromUTF8(DeltaText(payload)));
    };
    auto done = [&](WebResponse resp) {
        if (resp.ok) append({}, true);
        generate->Enable();
        save->Enable();
        std::string error;
        if (!resp.ok) error = resp.error + (sseBuf.empty() ? "" : ": " + ErrorDetail(sseBuf));
        else if (Trimmed(text->GetValue().utf8_string()).empty())
            error = "the model returned no text";
        if (error.empty()) {
            setStatus("Written by " + model.label + ". Check it and edit it if "
                      "needed, then Save.", false);
            return;
        }
        text->SetValue(before);
        setStatus(wxString::FromUTF8("\xE2\x9A\xA0 Generation failed: " + error +
                                     ". The previous text is back."), true);
    };
    generate->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        before = text->GetValue();
        text->Clear();
        sseBuf.clear();
        generate->Disable();
        save->Disable();
        setStatus("Generating with " + model.label + wxString::FromUTF8("\xE2\x80\xA6"), false);
        request = StreamingWebRequest(&dlg, BriefRequest(model, material),
                                      [&](std::string_view chunk) { append(chunk, false); },
                                      done);
    });

    if (dlg.ShowModal() != wxID_OK) return std::nullopt;
    return Trimmed(text->GetValue().utf8_string());
}
