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

#include <cstdint>
#include <functional>
#include <string>

class FontManager;

// Platform-native window abstraction. Replaces GLFW.
// Linux: Wayland (primary) with X11 fallback.
// macOS: NSWindow + NSOpenGLView.
class AppWindow {
public:
    AppWindow();
    ~AppWindow();

    bool Init(int width, int height, const char* title);
    void Show();
    void SetTitle(const char* title);
    bool ShouldClose() const;
    void SetShouldClose(bool close);
    void PollEvents();
    void WaitEvents();
    void WaitEventsTimeout(double timeout);

    // CSD-enabled frame rendering. Call BeginContentFrame() before the app
    // renders, then EndContentFrame() after. This binds the content FBO and
    // later composites titlebar/shadow/rounded corners.
    struct ContentFrame {
        int width;   // content width in pixels (fb size)
        int height;  // content height in pixels (fb size)
        int logicalW;
        int logicalH;
        int offsetY; // titlebar height in logical coords
    };
    ContentFrame BeginContentFrame();
    void EndContentFrame();
    void SwapBuffers();  // deprecated, calls EndContentFrame

    int Width() const;       // content width in pixels (excludes titlebar)
    int Height() const;      // content height in pixels (excludes titlebar)
    int FullWidth() const;   // full window logical width
    int FullHeight() const;  // full window logical height (includes titlebar)
    int LogicalW() const;    // logical content width
    int LogicalH() const;    // logical content height
    float Scale() const;     // UI scale factor (HiDPI)

    using ResizeCb = std::function<void(int, int, float)>;
    using MouseBtnCb = std::function<void(float, float, bool, bool)>;
    using MouseMoveCb = std::function<void(float, float, bool)>;
    using ScrollCb = std::function<void(float)>;
    using KeyCb = std::function<void(int key, int mods, bool pressed)>;
    using CharCb = std::function<void(uint32_t codepoint)>;
    using FocusCb = std::function<void(bool focused)>;

    void OnResize(ResizeCb cb);
    void OnMouseButton(MouseBtnCb cb);
    void OnMouseMove(MouseMoveCb cb);
    void OnScrollEvent(ScrollCb cb);
    void OnKeyEvent(KeyCb cb);
    void OnCharEvent(CharCb cb);
    void OnFocusChange(FocusCb cb);

    void SetClipboard(const std::string& text);
    std::string GetClipboard();

    // Provide the app's FontManager so CSD can render the titlebar title.
    // No-op when CSD isn't active (X11/macOS).
    void SetFontManager(const FontManager* fm);

    // True when the window has internal animation in flight that needs a
    // follow-up frame (e.g. CSD activate/backdrop colour transition). The app
    // loop should keep polling/rendering while this returns true.
    bool NeedsRedraw() const;

    // Platform-specific handle (void* to avoid exposing platform types)
    void* NativeHandle() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
