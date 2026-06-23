#include <wx/wx.h>
#include "chat_frame.h"
#include "shell_env.h"
#include "preferences.h"
#include "memory.h"
#include "mcp_stdio.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

// One-shot --reindex: walk every session file on disk and re-index its
// turns into the FTS5 database. Safe to re-run (RebuildSession deletes and
// re-inserts the session's rows).
static int RunReindex() {
    MemoryDB memory;
    if (!memory.Open(MemoryDB::DefaultPath())) {
        std::fprintf(stderr, "--reindex: failed to open memory DB at %s\n",
                     MemoryDB::DefaultPath().c_str());
        return 1;
    }

    std::string dir = MemoryDB::SessionsDir();
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "--reindex: no sessions dir at %s (nothing to do)\n",
                     dir.c_str());
        return 0;
    }

    int indexed = 0, skipped = 0;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        std::ifstream f(entry.path());
        json j;
        try { f >> j; }
        catch (...) {
            std::fprintf(stderr, "  skip (parse error): %s\n",
                         entry.path().c_str());
            skipped++;
            continue;
        }

        std::string sessionId = entry.path().stem().string();
        std::string cwd = j.value("cwd", "");
        std::string timestamp = j.value("lastUsed", "");
        const json& messages = j.contains("messages") && j["messages"].is_array()
                               ? j["messages"] : json::array();

        if (!memory.RebuildSession(sessionId, cwd, messages, timestamp)) {
            std::fprintf(stderr, "  skip (index error): %s\n",
                         entry.path().c_str());
            skipped++;
            continue;
        }
        std::printf("  indexed %s (%zu turns) — %s\n",
                    sessionId.c_str(), messages.size(), cwd.c_str());
        indexed++;
    }

    std::printf("\nReindex complete: %d sessions indexed, %d skipped.\nDB: %s\n",
                indexed, skipped, MemoryDB::DefaultPath().c_str());
    return 0;
}

class App : public wxApp {
public:
    bool OnInit() override {
        SetAppName("wx_gritcode");
        // Register embedded icon SVGs into the memory: virtual filesystem.
        // Must happen after wx is initialized, before any icon loading.
        RegisterEmbeddedIcons();
#if wxCHECK_VERSION(3, 3, 0)
        // On Windows, wx 3.3 defaults to light mode — the app must
        // explicitly opt in to follow the system appearance.
        SetAppearance(Appearance::System);
#endif
        Preferences::Init();

        // Pull PATH and friends from the user's login shell so tool subprocesses
        // see the same env they'd see in a terminal — matters when launched
        // from a .desktop file or DE menu where rc-files never ran.
        ImportShellEnv();

        auto* frame = new ChatFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP_NO_MAIN(App);

int main(int argc, char* argv[]) {
    // Intercept service-mode flags before wx initializes a GUI. Both modes
    // are headless (no window, no MCP TCP server) and exit on their own.
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--mcp-stdio") == 0) {
            return RunMcpStdioServer();
        }
        if (std::strcmp(argv[i], "--reindex") == 0) {
            return RunReindex();
        }
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "Usage: wx_gritcode [OPTIONS]\n\n"
                "Options:\n"
                "  --reindex      Rebuild the memory index from session history on disk\n"
                "  --mcp-stdio    Run as a stdio MCP server exposing grit_history_search/fetch\n"
                "  --help, -h     Show this help\n");
            return 0;
        }
    }

    return wxEntry(argc, argv);
}
