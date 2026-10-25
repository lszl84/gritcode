#include "project_brief.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
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

StreamingWebRequest WriteBrief(wxEvtHandler* target, const BriefModel& model,
                               const std::string& material,
                               std::function<void(std::string brief, std::string error)> done) {
    // Streamed events, and the raw body when the request fails.
    auto sse = std::make_shared<std::string>();
    auto text = std::make_shared<std::string>();
    auto take = [sse, text](std::string_view chunk, bool ended) {
        for (const std::string& payload : TakeSseEvents(*sse, chunk, ended))
            *text += DeltaText(payload);
    };
    return StreamingWebRequest(
        target, BriefRequest(model, material),
        [take](std::string_view chunk) { take(chunk, false); },
        [take, sse, text, done = std::move(done)](WebResponse resp) {
            if (!resp.ok) {
                done({}, resp.error + (sse->empty() ? "" : ": " + ErrorDetail(*sse)));
                return;
            }
            take({}, true);
            std::string brief = Trimmed(*text);
            if (brief.empty()) done({}, "the model returned no text");
            else done(std::move(brief), {});
        });
}
