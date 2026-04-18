// Gritcode — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com

#pragma once
#include <string>

namespace Clipboard {

void Init(void* window);
void Copy(const std::string& text);
std::string Paste();

}  // namespace Clipboard
