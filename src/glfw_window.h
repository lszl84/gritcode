// Gritcode — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com

#pragma once

#include <SDL3/SDL.h>
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

    int Width() const { return fbW_; }
    int Height() const { return fbH_; }
    float Scale() const { return contentScale_; }

    void SetClipboard(const std::string& text);

    // True when the compositor lacks server-side decorations and we draw our own.
    bool UseCSD() const { return useCSD_; }

    // Window controls for custom CSD buttons
    void Minimize();
    void ToggleMaximize();
    bool IsMaximized() const;

    // Custom titlebar hit-test config (values in framebuffer pixels).
    // Only meaningful when UseCSD() is true.
    void SetTitlebarConfigPx(int titlebarHeightPx, const RectI& dragExclusionPx);

    // Wake WaitEvents from bg thread
    static void PostEmptyEvent();

    // Callbacks (all coordinates in framebuffer pixels)
    void OnResize(std::function<void(int, int, float)> cb) { resizeCb_ = std::move(cb); }
    void OnMouseButton(std::function<void(float, float, bool, bool)> cb) { mouseBtnCb_ = std::move(cb); }
    void OnMouseMove(std::function<void(float, float, bool)> cb) { mouseMoveCb_ = std::move(cb); }
    void OnScrollEvent(std::function<void(float)> cb) { scrollCb_ = std::move(cb); }
    void OnKeyEvent(std::function<void(int, int, bool)> cb) { keyCb_ = std::move(cb); }
    void OnCharEvent(std::function<void(uint32_t)> cb) { charCb_ = std::move(cb); }

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext glCtx_ = nullptr;
    bool shouldClose_ = false;

    int winW_ = 0, winH_ = 0; // logical size
    int fbW_ = 0, fbH_ = 0;   // drawable/pixel size
    float contentScale_ = 1.0f;
    float bufferScale_ = 1.0f;

    bool leftDown_ = false;
    float mouseX_ = 0.0f, mouseY_ = 0.0f; // framebuffer pixels
    bool useCSD_ = false;

    int titlebarHeight_ = 0; // window-coordinate units (0 when SSD)
    RectI dragExclusion_{};   // window-coordinate units

    std::function<void(int, int, float)> resizeCb_;
    std::function<void(float, float, bool, bool)> mouseBtnCb_;
    std::function<void(float, float, bool)> mouseMoveCb_;
    std::function<void(float)> scrollCb_;
    std::function<void(int, int, bool)> keyCb_;
    std::function<void(uint32_t)> charCb_;

    void UpdateSizes();
    void DispatchEvent(const SDL_Event& ev);
    static SDL_HitTestResult SDLCALL HitTest(SDL_Window* win, const SDL_Point* area, void* data);
};
