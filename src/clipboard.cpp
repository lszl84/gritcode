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
#include <GLFW/glfw3.h>

static GLFWwindow* g_window = nullptr;

namespace Clipboard {

void Init(GLFWwindow* window) {
    g_window = window;
}

void Copy(const std::string& text) {
    if (g_window && !text.empty()) {
        glfwSetClipboardString(g_window, text.c_str());
    }
}

std::string Paste() {
    if (!g_window) return "";
    const char* text = glfwGetClipboardString(g_window);
    return text ? std::string(text) : "";
}

}  // namespace Clipboard
