// Gritcode — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "clipboard.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// X11 clipboard utilities using xclip (preferred) or xsel (fallback)
static bool xclip_copy(const char* text, size_t len) {
    FILE* p = popen("xclip -selection clipboard -i 2>/dev/null", "w");
    if (!p) return false;
    bool ok = (fwrite(text, 1, len, p) == len);
    pclose(p);
    return ok;
}

static std::string xclip_paste() {
    FILE* p = popen("xclip -selection clipboard -o 2>/dev/null", "r");
    if (!p) return "";
    std::string result;
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), p)) {
        result.append(buf, n);
    }
    pclose(p);
    // Strip trailing newline that xclip often adds
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

// xsel fallback for X11
static bool xsel_copy(const char* text, size_t len) {
    FILE* p = popen("xsel --clipboard --input 2>/dev/null", "w");
    if (!p) return false;
    bool ok = (fwrite(text, 1, len, p) == len);
    pclose(p);
    return ok;
}

static std::string xsel_paste() {
    FILE* p = popen("xsel --clipboard --output 2>/dev/null", "r");
    if (!p) return "";
    std::string result;
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), p)) {
        result.append(buf, n);
    }
    pclose(p);
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

// Wayland clipboard using wl-copy/wl-paste
static bool wayland_copy(const char* text, size_t len) {
    FILE* p = popen("wl-copy 2>/dev/null", "w");
    if (!p) return false;
    bool ok = (fwrite(text, 1, len, p) == len);
    pclose(p);
    return ok;
}

static std::string wayland_paste() {
    FILE* p = popen("wl-paste --no-newline 2>/dev/null", "r");
    if (!p) return "";
    std::string result;
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), p)) {
        result.append(buf, n);
    }
    pclose(p);
    return result;
}

// macOS clipboard using pbcopy/pbpaste
static bool macos_copy(const char* text, size_t len) {
    FILE* p = popen("pbcopy 2>/dev/null", "w");
    if (!p) return false;
    bool ok = (fwrite(text, 1, len, p) == len);
    pclose(p);
    return ok;
}

static std::string macos_paste() {
    FILE* p = popen("pbpaste 2>/dev/null", "r");
    if (!p) return "";
    std::string result;
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), p)) {
        result.append(buf, n);
    }
    pclose(p);
    return result;
}

// Detect the current desktop environment
// Returns: "wayland", "x11", or "macos"
static const char* detect_desktop() {
    // Check for Wayland first
    const char* wayland = getenv("WAYLAND_DISPLAY");
    if (wayland && wayland[0]) {
        return "wayland";
    }

    // Check for X11
    const char* display = getenv("DISPLAY");
    if (display && display[0]) {
        return "x11";
    }

    // Check for macOS
#ifdef __APPLE__
    return "macos";
#endif

    return "unknown";
}

// Check if a command exists
static bool command_exists(const char* cmd) {
    char buf[256];
    snprintf(buf, sizeof(buf), "command -v %s >/dev/null 2>&1", cmd);
    return (system(buf) == 0);
}

namespace Clipboard {

bool Copy(const std::string& text) {
    if (text.empty()) return true;

    const char* desktop = detect_desktop();
    size_t len = text.size();

    if (strcmp(desktop, "wayland") == 0) {
        if (wayland_copy(text.c_str(), len)) return true;
    } else if (strcmp(desktop, "x11") == 0) {
        // Try xclip first, then xsel
        if (command_exists("xclip")) {
            if (xclip_copy(text.c_str(), len)) return true;
        }
        if (command_exists("xsel")) {
            if (xsel_copy(text.c_str(), len)) return true;
        }
    } else if (strcmp(desktop, "macos") == 0) {
        if (macos_copy(text.c_str(), len)) return true;
    }

    return false;
}

std::string Paste() {
    const char* desktop = detect_desktop();

    if (strcmp(desktop, "wayland") == 0) {
        return wayland_paste();
    } else if (strcmp(desktop, "x11") == 0) {
        // Try xclip first, then xsel
        if (command_exists("xclip")) {
            std::string result = xclip_paste();
            if (!result.empty()) return result;
        }
        if (command_exists("xsel")) {
            return xsel_paste();
        }
    } else if (strcmp(desktop, "macos") == 0) {
        return macos_paste();
    }

    return "";
}

}  // namespace Clipboard
