// Gritcode — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com
// SPDX-License-Identifier: GPL-3.0-or-later

#include "clipboard.h"

// Clipboard implementation — will use Wayland wl_data_device and X11 selections.
// For now, stub implementations that will be filled in when the native
// window backends are wired up.

namespace Clipboard {

void Copy(const std::string& text) {
    (void)text;
    // TODO: wl_data_device (Wayland) or X11 selection
}

std::string Paste() {
    // TODO: wl_data_device (Wayland) or X11 selection
    return "";
}

}  // namespace Clipboard