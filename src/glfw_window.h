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
#ifdef __APPLE__
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <functional>
#include <string>

class GlfwWindow {
public:
    GlfwWindow();
    ~GlfwWindow();

    bool Init(int width, int height, const char* title);
    void Show();
    void SetTitle(const char* title);
    bool ShouldClose() const;
    void PollEvents();
    void WaitEvents();
    void SwapBuffers();

    int Width() const { return fbW_; }       // framebuffer pixels
    int Height() const { return fbH_; }
    int LogicalW() const { return winW_; }
    int LogicalH() const { return winH_; }
    // UI scale factor (for sizing fonts, widgets, padding). Derived from
    // GLFW's content-scale query, which on Wayland/macOS is the compositor
    // buffer scale and on X11 is the Xft.dpi / XSETTINGS / XRandR-derived
    // DPI ratio. Previously this returned fbW/winW, which is always 1 on
    // X11, so grit rendered at 96dpi regardless of the user's HiDPI setting.
    float Scale() const { return contentScale_; }

    using ResizeCb = std::function<void(int, int, float)>;
    using MouseBtnCb = std::function<void(float, float, bool, bool)>;
    using MouseMoveCb = std::function<void(float, float, bool)>;
    using ScrollCb = std::function<void(float)>;
    using KeyCb = std::function<void(int key, int mods, bool pressed)>;
    using CharCb = std::function<void(uint32_t codepoint)>;

    void OnResize(ResizeCb cb) { resizeCb_ = cb; }
    void OnMouseButton(MouseBtnCb cb) { mouseBtnCb_ = cb; }
    void OnMouseMove(MouseMoveCb cb) { mouseMoveCb_ = cb; }
    void OnScrollEvent(ScrollCb cb) { scrollCb_ = cb; }
    void OnKeyEvent(KeyCb cb) { keyCb_ = cb; }
    void OnCharEvent(CharCb cb) { charCb_ = cb; }

    void SetClipboard(const std::string& text);

    GLFWwindow* Handle() { return window_; }

private:
    GLFWwindow* window_ = nullptr;
    int winW_ = 0, winH_ = 0;    // logical
    int fbW_ = 0, fbH_ = 0;      // framebuffer
    // UI scale — derived from glfwGetWindowContentScale. What every widget
    // sizing site wants. On Wayland/macOS equals the compositor buffer
    // scale; on X11 equals Xft.dpi/96 (or whatever XSETTINGS reports).
    float contentScale_ = 1.0f;
    // Framebuffer-to-logical ratio, needed only to map GLFW's logical
    // cursor coordinates onto the larger framebuffer on Wayland/macOS.
    // On X11 this is always 1 (GLFW's "logical" coords are already pixels).
    float bufferScale_ = 1.0f;
    double mouseX_ = 0, mouseY_ = 0;
    bool leftDown_ = false;
    bool superHeld_ = false;  // Track Cmd/Super for suppressing char events

    ResizeCb resizeCb_;
    MouseBtnCb mouseBtnCb_;
    MouseMoveCb mouseMoveCb_;
    ScrollCb scrollCb_;
    KeyCb keyCb_;
    CharCb charCb_;

    void UpdateScale();

    static void FramebufferSizeCb(GLFWwindow*, int, int);
    static void WindowContentScaleCb(GLFWwindow*, float, float);
    static void MouseButtonCb(GLFWwindow*, int, int, int);
    static void CursorPosCb(GLFWwindow*, double, double);
    static void ScrollCallbackCb(GLFWwindow*, double, double);
    static void KeyCallbackCb(GLFWwindow*, int, int, int, int);
    static void CharCallbackCb(GLFWwindow*, unsigned int);
};
