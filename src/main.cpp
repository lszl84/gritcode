#include <wx/wx.h>
#include "chat_frame.h"
#include "shell_env.h"
#include "preferences.h"
#include "memory.h"
#include "mcp_stdio.h"
#include "perf_log.h"

#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#ifndef NDEBUG
#include <typeinfo>
#ifndef _WIN32
#include <execinfo.h>
#endif
#endif
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <wx/msw/private.h>

namespace {
// Startup diagnostics. WIN32-subsystem apps have no console, so an early
// failure is invisible: log the startup path and any crash to
// %TEMP%\gritcode-startup.log.
FILE* StartupLogFile() {
    static FILE* f = []() -> FILE* {
        char path[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, path);
        if (n == 0 || n >= MAX_PATH) return nullptr;
        std::strncat(path, "gritcode-startup.log", MAX_PATH - n - 1);
        return std::fopen(path, "a");
    }();
    return f;
}

void StartupLog(const char* msg) {
    FILE* f = StartupLogFile();
    if (f) {
        std::fprintf(f, "[%lu] %s\n", (unsigned long)GetTickCount(), msg);
        std::fflush(f);
    }
}

// Unhandled native exceptions (access violations etc.) — the wx-side handler
// is NDEBUG-gated, so this one covers Release builds too.
LONG WINAPI SehCrashFilter(EXCEPTION_POINTERS* ep) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "CRASH: SEH exception code 0x%08lX at %p",
                  ep->ExceptionRecord->ExceptionCode,
                  ep->ExceptionRecord->ExceptionAddress);
    StartupLog(buf);
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace
#endif  // _WIN32

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
        std::printf("  indexed %s (%zu turns) - %s\n",
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
        PERF_SCOPE("OnInit");
#ifdef _WIN32
        StartupLog("OnInit: enter");
#endif
        SetAppName("gritcode");
        // Register JPEG/PNG/GIF/etc. decoders so image attachments load.
        wxInitAllImageHandlers();
#if wxCHECK_VERSION(3, 3, 0)
        // On Windows, wx 3.3 defaults to light mode — the app must
        // explicitly opt in to follow the system appearance.
        SetAppearance(Appearance::System);
#endif
        Preferences::Init();
#ifdef _WIN32
        StartupLog("OnInit: Preferences::Init done");
#endif

        // Pull PATH and friends from the user's login shell so tool subprocesses
        // see the same env they'd see in a terminal — matters when launched
        // from a .desktop file or DE menu where rc-files never ran.
        { PERF_SCOPE("ImportShellEnv"); ImportShellEnv(); }
#ifdef _WIN32
        StartupLog("OnInit: ImportShellEnv done");
#endif

        ChatFrame* frame;
        { PERF_SCOPE("new ChatFrame"); frame = new ChatFrame(); }
#ifdef _WIN32
        StartupLog("OnInit: ChatFrame constructed");
#endif
        frame->Show(true);
#ifdef _WIN32
        StartupLog("OnInit: frame shown, entering main loop");
#endif
        return true;
    }

#ifndef NDEBUG
    bool OnExceptionInMainLoop() override {
        try {
            throw;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "EXCEPTION std [%s]: %s\n",
                         typeid(e).name(), e.what());
        } catch (...) {
            std::fprintf(stderr, "EXCEPTION unknown type\n");
        }
#ifndef _WIN32
        void* frames[32];
        int n = backtrace(frames, 32);
        char** syms = backtrace_symbols(frames, n);
        if (syms) {
            for (int i = 0; i < n; ++i) std::fprintf(stderr, "  %s\n", syms[i]);
            free(syms);
        }
#endif
        return false;  // stop the loop; we've logged the type
    }
#endif
};

wxIMPLEMENT_APP_NO_MAIN(App);

static int RunApp(int argc, char* argv[]) {
    // Intercept service-mode flags before wx initializes a GUI. Both modes
    // are headless (no window, no MCP TCP server) and exit on their own.
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--mcp-stdio") == 0) {
            for (int j = i + 1; j + 1 < argc; j++) {
                if (std::strcmp(argv[j], "--run-project") == 0)
                    return RunMcpStdioServer(argv[j + 1]);
            }
            return RunMcpStdioServer();
        }
        if (std::strcmp(argv[i], "--reindex") == 0) {
            return RunReindex();
        }
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "Usage: gritcode [OPTIONS]\n\n"
                "Options:\n"
                "  --reindex      Rebuild the memory index from session history on disk\n"
                "  --mcp-stdio    Run as a stdio MCP server exposing grit_history_search/fetch\n"
                "  --mcp-stdio --run-project DIR\n"
                "                 Run as a stdio MCP server exposing only run_project for DIR\n"
                "  --help, -h     Show this help\n");
            return 0;
        }
    }

    return wxEntry(argc, argv);
}

#ifndef _WIN32
int main(int argc, char* argv[]) {
    return RunApp(argc, argv);
}
#else
// WIN32 (GUI) subsystem: the CRT's entry point is WinMain, not main.
// __argc/__argv are populated by the CRT before the entry point runs.
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    SetUnhandledExceptionFilter(SehCrashFilter);
    StartupLog("WinMain: entering");
    // wxApp::Initialize() self-heals a null instance handle, but set it
    // explicitly anyway — some early paths (resources, DPI) read it first.
    wxSetInstance(hInstance);
    int rc = RunApp(__argc, __argv);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "WinMain: wxEntry returned rc=%d", rc);
    StartupLog(buf);
    return rc;
}
#endif
