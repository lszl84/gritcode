#include "omarchy_theme.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <wx/filename.h>
#include <wx/utils.h>

namespace omarchy {
namespace {

std::optional<Theme> g_theme;
bool g_loaded = false;

double Luminance(const wxColour& c) {
    auto lin = [](int v) {
        double s = v / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.Red()) + 0.7152 * lin(c.Green()) + 0.0722 * lin(c.Blue());
}

double Contrast(const wxColour& a, const wxColour& b) {
    double la = Luminance(a), lb = Luminance(b);
    if (la < lb) std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

// Omarchy's current-theme directory. Recent releases keep it under the XDG
// state dir; older ones used ~/.config/omarchy/current.
wxString FindCurrentDir() {
#ifdef __linux__
    wxString home = wxGetHomeDir();
    const char* xdgState = std::getenv("XDG_STATE_HOME");
    wxString state = (xdgState && *xdgState) ? wxString::FromUTF8(xdgState)
                                             : home + "/.local/state";
    for (const wxString& dir : {state + "/omarchy/current",
                                home + "/.config/omarchy/current"}) {
        if (wxFileName::FileExists(dir + "/theme/colors.toml")) return dir;
    }
#endif
    return {};
}

// colors.toml is flat `key = "value"` lines. Only that subset is parsed, so a
// full TOML library isn't needed; anything unexpected is skipped.
std::optional<Theme> Load() {
    wxString dir = FindCurrentDir();
    if (dir.empty()) return std::nullopt;
    std::ifstream in((dir + "/theme/colors.toml").ToStdString(wxConvUTF8));
    if (!in) return std::nullopt;

    Theme t;
    std::string mode;
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        if (key.empty() || key[0] == '#' || key[0] == '[') continue;
        std::string val = Trim(line.substr(eq + 1));
        if (val.size() >= 2 && val[0] == '"') {
            size_t close = val.find('"', 1);
            if (close == std::string::npos) continue;
            val = val.substr(1, close - 1);
        }
        if (key == "mode") {
            mode = val;
            continue;
        }
        wxColour c(wxString::FromUTF8(val));
        if (c.IsOk()) t.colors[key] = c;
    }

    // Everything the app derives from needs these two.
    if (!t.Get("background").IsOk() || !t.Get("foreground").IsOk())
        return std::nullopt;
    if (mode == "light") t.dark = false;
    else if (mode == "dark") t.dark = true;
    else t.dark = Luminance(t.Get("background")) < 0.18;
    return t;
}

bool Same(const std::optional<Theme>& a, const std::optional<Theme>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a) return true;
    return a->dark == b->dark && a->colors == b->colors;
}

}  // namespace

wxColour Theme::Get(const std::string& key) const {
    auto it = colors.find(key);
    return it == colors.end() ? wxColour() : it->second;
}

const Theme* Current() {
    if (!g_loaded) {
        g_theme = Load();
        g_loaded = true;
    }
    return g_theme ? &*g_theme : nullptr;
}

bool Reload() {
    std::optional<Theme> fresh = Load();
    bool changed = !g_loaded || !Same(fresh, g_theme);
    g_theme = std::move(fresh);
    g_loaded = true;
    return changed;
}

wxString WatchDir() {
    return FindCurrentDir();
}

wxColour Mix(const wxColour& a, const wxColour& b, double t) {
    auto ch = [t](int x, int y) {
        return (unsigned char)std::lround(x + (y - x) * t);
    };
    return wxColour(ch(a.Red(), b.Red()), ch(a.Green(), b.Green()),
                    ch(a.Blue(), b.Blue()));
}

wxColour Readable(const wxColour& c, const wxColour& bg, double minRatio) {
    const wxColour target = Luminance(bg) < 0.18 ? *wxWHITE : *wxBLACK;
    wxColour out = c;
    for (int i = 0; i < 10 && Contrast(out, bg) < minRatio; ++i)
        out = Mix(out, target, 0.15);
    return out;
}

}  // namespace omarchy
