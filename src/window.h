// Gritcode — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <string>
#include <cstdint>

struct RectI {
    int x = 0, y = 0, w = 0, h = 0;
};

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

    void SetClipboard(const std::string& text);

    // Window controls for custom CSD
    void Minimize();
    void ToggleMaximize();
    bool IsMaximized() const;

    // Hit-test config
    void SetTitlebarConfigPx(int titlebarHeight, const RectI& dragExclusion);

    // Wake WaitEvents
    static void PostEmptyEvent();

    // Callbacks
    using ResizeCb = std::function<void(int, int, float)>;
    using MouseBtnCb = std::function<void(float, float, bool, bool)>;
    using MouseMoveCb = std::function<void(float, float, bool)>;
    using ScrollCb = std::function<void(float)>;
    using KeyCb = std::function<void(int key, int mods, bool pressed)>;
    using CharCb = std::function<void(uint32_t codepoint)>;

    void OnResize(ResizeCb cb) { resizeCb_ = std::move(cb); }
    void OnMouseButton(MouseBtnCb cb) { mouseBtnCb_ = std::move(cb); }
    void OnMouseMove(MouseMoveCb cb) { mouseMoveCb_ = std::move(cb); }
    void OnScrollEvent(ScrollCb cb) { scrollCb_ = std::move(cb); }
    void OnKeyEvent(KeyCb cb) { keyCb_ = std::move(cb); }
    void OnCharEvent(CharCb cb) { charCb_ = std::move(cb); }

private:
    struct Impl;
    Impl* impl_;

    ResizeCb resizeCb_;
    MouseBtnCb mouseBtnCb_;
    MouseMoveCb mouseMoveCb_;
    ScrollCb scrollCb_;
    KeyCb keyCb_;
    CharCb charCb_;
};