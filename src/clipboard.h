// Gritcode — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

namespace Clipboard {

void Copy(const std::string& text);
std::string Paste();

}  // namespace Clipboard