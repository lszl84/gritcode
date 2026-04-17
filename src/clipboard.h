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

#pragma once
#include <GLFW/glfw3.h>
#include <string>

namespace Clipboard {

// Initialize clipboard with a window handle. Must be called before Copy/Paste.
void Init(GLFWwindow* window);

// Copy text to the system clipboard.
void Copy(const std::string& text);

// Get text from the system clipboard.
std::string Paste();

}  // namespace Clipboard
