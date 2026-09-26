#pragma once

#include <optional>

// Hyprland integration. Tiling compositors dictate window sizes, so the app
// must not try to grow its window there; Hyprland's IPC socket is the only
// reliable way to tell (it reports every window to GTK as tiled + maximized,
// floating or not).
namespace hyprland {

// Whether this process's window is tiled (or fullscreen) under Hyprland.
// nullopt when not running on Hyprland or the query fails; callers then keep
// their normal behaviour. Synchronous; a local socket round trip (~1 ms).
std::optional<bool> IsTiled();

}  // namespace hyprland
