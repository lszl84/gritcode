// Gritcode — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com

#include "clipboard.h"
#include <SDL3/SDL.h>

namespace Clipboard {

void Init(void*) {}

void Copy(const std::string& text) {
    if (!text.empty()) SDL_SetClipboardText(text.c_str());
}

std::string Paste() {
    char* text = SDL_GetClipboardText();
    if (!text) return "";
    std::string out(text);
    SDL_free(text);
    return out;
}

}  // namespace Clipboard
