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
#include <functional>
#include <string>
#include <cstdint>

struct RectI { int x, y, w, h; };

class GlfwWindow {
public:
    GlfwWindow();
    ~GlfwWindow();

    bool Init(int width, int height, const char* title);
    void Show();
    void SetTitle(const char* title);
    bool ShouldClose() const;
    void RequestClose();
    void PollEvents();
    void WaitEvents();
    void WaitEventsTimeout(double timeout);
    void SwapBuffers();

    int Width() const;
    int Height() const;
    float Scale() const;

    // CSD framebuffer accounting. With shadow+rounded-corner CSD, the GL
    // framebuffer is larger than the logical window (shadow padding on all
    // sides) and the app renders into a content sub-rect that lives inside
    // the shadow and below the titlebar. Width()/Height() describe that
    // content rect; these four return the viewport math the renderer
    // needs (bottom-left origin, framebuffer pixels). Non-CSD cases
    // collapse to (0, 0) and framebuffer height == Height().
    int ViewportX() const;
    int ViewportY() const;
    int FramebufferH() const;

    // Bind the CSD FBO (if any) so the app renders into it; no-op when
    // SSD is active. Must be called before GLRenderer::BeginFrame so the
    // renderer's viewport/clear apply to the right target.
    void BeginFrame();

    void SetClipboard(const std::string& text);

    void Minimize();
    void ToggleMaximize();
    bool IsMaximized() const;
    void SetTitlebarConfigPx(int height, const RectI& closeBtn);

    static void PostEmptyEvent();

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

    struct Impl;

    ResizeCb resizeCb_;
    MouseBtnCb mouseBtnCb_;
    MouseMoveCb mouseMoveCb_;
    ScrollCb scrollCb_;
    KeyCb keyCb_;
    CharCb charCb_;

private:
    Impl* impl_ = nullptr;
};
