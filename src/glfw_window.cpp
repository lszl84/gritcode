// Gritcode — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com

#include "glfw_window.h"
#include "types.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include "keysyms.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>

#ifdef GRIT_LINUX
#include <wayland-client.h>
#endif

#ifdef GRIT_MACOS
extern "C" void MacStyleWindowChrome(void*, float, float, float);
#endif

#ifdef GRIT_LINUX
// Query the Wayland compositor directly for xdg-decoration support.
// Opens an independent connection, enumerates globals, and disconnects.
// Returns true if the compositor advertises zxdg_decoration_manager_v1
// (meaning it can provide server-side decorations).
static void WlRegistryGlobal(void* data, struct wl_registry*, uint32_t,
                              const char* interface, uint32_t) {
    if (strcmp(interface, "zxdg_decoration_manager_v1") == 0)
        *static_cast<bool*>(data) = true;
}
static void WlRegistryRemove(void*, struct wl_registry*, uint32_t) {}
static const struct wl_registry_listener kRegistryListener = {
    WlRegistryGlobal, WlRegistryRemove,
};

static bool WaylandHasSSD() {
    struct wl_display* dpy = wl_display_connect(nullptr);
    if (!dpy) return false;
    bool found = false;
    struct wl_registry* reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &kRegistryListener, &found);
    wl_display_roundtrip(dpy);
    wl_registry_destroy(reg);
    wl_display_disconnect(dpy);
    return found;
}
#endif

static int ToKeySym(SDL_Keycode key) {
    switch (key) {
    case SDLK_ESCAPE: return XKB_KEY_Escape;
    case SDLK_BACKSPACE: return XKB_KEY_BackSpace;
    case SDLK_DELETE: return XKB_KEY_Delete;
    case SDLK_RETURN: return XKB_KEY_Return;
    case SDLK_KP_ENTER: return XKB_KEY_KP_Enter;
    case SDLK_LEFT: return XKB_KEY_Left;
    case SDLK_RIGHT: return XKB_KEY_Right;
    case SDLK_UP: return XKB_KEY_Up;
    case SDLK_DOWN: return XKB_KEY_Down;
    case SDLK_HOME: return XKB_KEY_Home;
    case SDLK_END: return XKB_KEY_End;
    case SDLK_PAGEUP: return 0xff55;
    case SDLK_PAGEDOWN: return 0xff56;
    case SDLK_SPACE: return Key::Space;
    case SDLK_A: return Key::A;
    case SDLK_C: return Key::C;
    case SDLK_V: return 'V';
    case SDLK_J: return XKB_KEY_j;
    case SDLK_K: return XKB_KEY_k;
    default: return 0;
    }
}

GlfwWindow::GlfwWindow() = default;

GlfwWindow::~GlfwWindow() {
    if (glCtx_) SDL_GL_DestroyContext(glCtx_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool GlfwWindow::Init(int width, int height, const char* title) {
    SDL_SetAppMetadata("Gritcode", GRIT_VERSION, "ai.gritcode.app");
    SDL_SetHint(SDL_HINT_APP_ID, "ai.gritcode.app");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return false;
    }

    // Decide CSD vs SSD before creating the window.
    useCSD_ = false;
#ifdef GRIT_LINUX
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && strcmp(driver, "wayland") == 0) {
        useCSD_ = !WaylandHasSSD();
        fprintf(stderr, "wayland: compositor %s SSD → %s\n",
                useCSD_ ? "lacks" : "provides",
                useCSD_ ? "using custom CSD" : "using server decorations");
    }
    // X11: window manager always provides SSD.
#endif

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN
                 | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (useCSD_) flags |= SDL_WINDOW_BORDERLESS;

    window_ = SDL_CreateWindow(title, width, height, flags);
    if (!window_) {
        fprintf(stderr, "SDL window creation failed: %s\n", SDL_GetError());
        return false;
    }

    glCtx_ = SDL_GL_CreateContext(window_);
    if (!glCtx_) {
        fprintf(stderr, "SDL GL context creation failed: %s\n", SDL_GetError());
        return false;
    }

    if (useCSD_) {
        if (!SDL_SetWindowHitTest(window_, HitTest, this))
            fprintf(stderr, "SDL_SetWindowHitTest failed: %s\n", SDL_GetError());
    }

    if (!SDL_GL_SetSwapInterval(0)) {
        fprintf(stderr, "SDL_GL_SetSwapInterval(0) failed: %s\n", SDL_GetError());
    }

    UpdateSizes();

#ifdef GRIT_MACOS
    MacStyleWindowChrome(window_, 0.12f, 0.12f, 0.13f);
#endif

    return true;
}

void GlfwWindow::UpdateSizes() {
    SDL_GetWindowSize(window_, &winW_, &winH_);
    SDL_GetWindowSizeInPixels(window_, &fbW_, &fbH_);

    // SDL3 docs: use window display scale for content/UI scaling.
    contentScale_ = SDL_GetWindowDisplayScale(window_);
    if (contentScale_ <= 0.0f) contentScale_ = 1.0f;

    // SDL3 docs: use pixel density for converting event coords (window units)
    // to framebuffer coords (OpenGL pixel units).
    bufferScale_ = SDL_GetWindowPixelDensity(window_);
    if (bufferScale_ <= 0.0f) bufferScale_ = 1.0f;
}

void GlfwWindow::Show() { SDL_ShowWindow(window_); }
void GlfwWindow::SetTitle(const char* title) { SDL_SetWindowTitle(window_, title); }

bool GlfwWindow::ShouldClose() const { return shouldClose_; }
void GlfwWindow::RequestClose() { shouldClose_ = true; }

void GlfwWindow::PollEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) DispatchEvent(ev);
}

void GlfwWindow::WaitEvents() { WaitEventsTimeout(0.5); }

void GlfwWindow::WaitEventsTimeout(double timeout) {
    SDL_Event ev;
    Uint32 ms = (timeout <= 0.0) ? 0 : (Uint32)(timeout * 1000.0);
    if (SDL_WaitEventTimeout(&ev, (int)ms)) {
        DispatchEvent(ev);
        while (SDL_PollEvent(&ev)) DispatchEvent(ev);
    }
}

void GlfwWindow::SwapBuffers() { SDL_GL_SwapWindow(window_); }

void GlfwWindow::SetClipboard(const std::string& text) {
    SDL_SetClipboardText(text.c_str());
}

void GlfwWindow::Minimize() { SDL_MinimizeWindow(window_); }

void GlfwWindow::ToggleMaximize() {
    if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(window_);
    else SDL_MaximizeWindow(window_);
}

bool GlfwWindow::IsMaximized() const {
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
}

void GlfwWindow::SetTitlebarConfigPx(int titlebarHeightPx, const RectI& dragExclusionPx) {
    // Inputs are framebuffer pixels (app layout/render space).
    // SDL hit-test coordinates are window-space units.
    const float invScale = (bufferScale_ > 0.0f) ? (1.0f / bufferScale_) : 1.0f;

    // Slightly enlarge exclusion to avoid rounding gaps where hover works
    // but click gets captured by drag/resize hit-test.
    const int padPx = 2;
    const int exPxX = dragExclusionPx.x - padPx;
    const int exPxY = dragExclusionPx.y;
    const int exPxW = dragExclusionPx.w + padPx * 2;
    const int exPxH = dragExclusionPx.h + padPx;

    titlebarHeight_ = std::max(0, (int)std::ceil(titlebarHeightPx * invScale));
    dragExclusion_ = {
        (int)std::floor(exPxX * invScale),
        std::max(0, (int)std::floor(exPxY * invScale)),
        std::max(0, (int)std::ceil(exPxW * invScale)),
        std::max(0, (int)std::ceil(exPxH * invScale)),
    };
}

void GlfwWindow::PostEmptyEvent() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    SDL_PushEvent(&ev);
}

enum SDL_HitTestResult GlfwWindow::HitTest(SDL_Window*, const SDL_Point* area, void* data) {
    auto* self = static_cast<GlfwWindow*>(data);
    if (!self || !area || !self->useCSD_) return SDL_HITTEST_NORMAL;

    // SDL hit-test callback gives window-coordinate units.
    const int x = area->x;
    const int y = area->y;

    const int w = self->winW_;
    const int h = self->winH_;
    // Border resize should be subtle and never steal top-bar button clicks.
    const int border = 4;

    // Invariant: full top bar draggable, except exact button area exclusion.
    if (y < self->titlebarHeight_) {
        const RectI ex = self->dragExclusion_;
        const bool inEx = (x >= ex.x && x < ex.x + ex.w && y >= ex.y && y < ex.y + ex.h);
        if (inEx) return SDL_HITTEST_NORMAL;      // let UI button click win
        return SDL_HITTEST_DRAGGABLE;
    }

    const bool left = x < border;
    const bool right = x >= (w - border);
    const bool top = y < border;
    const bool bottom = y >= (h - border);

    if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (left) return SDL_HITTEST_RESIZE_LEFT;
    if (right) return SDL_HITTEST_RESIZE_RIGHT;
    if (top) return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;

    return SDL_HITTEST_NORMAL;
}

void GlfwWindow::DispatchEvent(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        shouldClose_ = true;
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        UpdateSizes();
        if (resizeCb_) resizeCb_(fbW_, fbH_, contentScale_);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (ev.button.button != SDL_BUTTON_LEFT) break;
        leftDown_ = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        float px = ev.button.x * bufferScale_;
        float py = ev.button.y * bufferScale_;
        mouseX_ = px;
        mouseY_ = py;
        bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
        if (mouseBtnCb_) mouseBtnCb_(px, py, leftDown_, shift);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        float px = ev.motion.x * bufferScale_;
        float py = ev.motion.y * bufferScale_;
        mouseX_ = px;
        mouseY_ = py;
        if (mouseMoveCb_) mouseMoveCb_(px, py, leftDown_);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (scrollCb_) scrollCb_((float)ev.wheel.y);
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        int sym = ToKeySym(ev.key.key);
        if (!sym || !keyCb_) break;
        int mods = 0;
        SDL_Keymod m = SDL_GetModState();
        if (m & SDL_KMOD_CTRL) mods |= Mod::Ctrl;
#ifdef __APPLE__
        if (m & SDL_KMOD_GUI) mods |= Mod::Ctrl;
#endif
        if (m & SDL_KMOD_SHIFT) mods |= Mod::Shift;
        keyCb_(sym, mods, ev.type == SDL_EVENT_KEY_DOWN);
        break;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (charCb_ && ev.text.text[0]) {
            const unsigned char* s = (const unsigned char*)ev.text.text;
            uint32_t cp = 0;
            if (s[0] < 0x80) cp = s[0];
            else if ((s[0] & 0xE0) == 0xC0) cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            else if ((s[0] & 0xF0) == 0xE0) cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            else if ((s[0] & 0xF8) == 0xF0) cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
                                                      ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
            if (cp) charCb_(cp);
        }
        break;
    default:
        break;
    }
}
