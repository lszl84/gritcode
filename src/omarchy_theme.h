#pragma once

#include <map>
#include <string>
#include <wx/colour.h>
#include <wx/string.h>

// Omarchy (https://omarchy.org) theme integration. Omarchy writes the active
// theme's palette to <state>/omarchy/current/theme/colors.toml; when that file
// exists the app derives its custom colours from it instead of the built-in
// dark/light palettes. Everything here degrades to "no theme" on systems
// without Omarchy (and always on non-Linux platforms).
namespace omarchy {

struct Theme {
    bool dark = true;                         // `mode`, else background luminance
    std::map<std::string, wxColour> colors;   // colors.toml key -> colour

    // The named colour, or an invalid wxColour when the theme lacks it.
    wxColour Get(const std::string& key) const;
};

// The active Omarchy theme, or nullptr when there is none. Loaded lazily on
// first use and cached until Reload().
const Theme* Current();

// Re-read colors.toml. Returns true when the result differs from the cached
// theme (a switch, or Omarchy appearing/disappearing).
bool Reload();

// Directory whose changes signal a theme switch (it holds theme.name and the
// theme/ folder), or empty when Omarchy isn't installed.
wxString WatchDir();

// Linear blend: t = 0 gives `a`, t = 1 gives `b`.
wxColour Mix(const wxColour& a, const wxColour& b, double t);

// `c`, pushed toward white (dark bg) or black (light bg) until it reaches the
// WCAG contrast ratio `minRatio` against `bg`.
wxColour Readable(const wxColour& c, const wxColour& bg, double minRatio);

}  // namespace omarchy
