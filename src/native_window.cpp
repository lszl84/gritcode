// Native window backend for Gritcode — replaces GLFW with direct Wayland/X11.
// Architecture from opengl-smooth (Wayland frame-callback paced rendering,
// X11 _NET_WM_SYNC_REQUEST with extended counter parity for smooth resize).
//
// Copyright (C) 2026 luke@devmindscape.com
// SPDX-License-Identifier: GPL-3.0-or-later

#include "glfw_window.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <poll.h>

// ---------------------------------------------------------------------------
// Wayland backend
// ---------------------------------------------------------------------------

#ifdef HAVE_WAYLAND
#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>
#include <EGL/egl.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

struct WlState {
    wl_display*        display    = nullptr;
    wl_compositor*     compositor = nullptr;
    xdg_wm_base*       wm_base    = nullptr;
    zxdg_decoration_manager_v1* deco_mgr = nullptr;
    wl_seat*            seat       = nullptr;
    wl_pointer*         pointer    = nullptr;
    wl_keyboard*        keyboard   = nullptr;
    wl_shm*             shm        = nullptr;

    wl_surface*         surface    = nullptr;
    xdg_surface*        xsurface   = nullptr;
    xdg_toplevel*       toplevel   = nullptr;
    zxdg_toplevel_decoration_v1* deco = nullptr;

    wl_egl_window*      egl_window  = nullptr;
    EGLDisplay           egl_display = EGL_NO_DISPLAY;
    EGLContext           egl_context = EGL_NO_CONTEXT;
    EGLSurface           egl_surface = EGL_NO_SURFACE;
    EGLConfig            egl_config  = nullptr;

    wl_cursor_theme*     cursor_theme   = nullptr;
    wl_surface*          cursor_surface = nullptr;
    uint32_t             last_enter_serial = 0;
    const char*          current_cursor  = nullptr;

    int width  = 900;
    int height = 700;
    int pending_width  = 0;
    int pending_height = 0;
    int scale = 1;

    bool egl_ready     = false;
    bool running       = true;
    bool dirty         = true;
    bool frame_pending = false;
    bool configured    = false;

    // Callbacks into GlfwWindow
    GlfwWindow* owner = nullptr;

    // Keyboard state
    xkb_context*     xkb_ctx  = nullptr;
    xkb_keymap*      xkb_keymap = nullptr;
    xkb_state*       xkb_state  = nullptr;
    uint32_t         mods_depressed = 0;
    uint32_t         mods_latched = 0;
    uint32_t         mods_locked  = 0;
};

// ---------------------------------------------------------------------------
// X11 backend
// ---------------------------------------------------------------------------

#elif defined(HAVE_X11)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <EGL/egl.h>

struct X11State {
    Display*    dpy = nullptr;
    Window      win = 0;
    EGLDisplay  egl_dpy = EGL_NO_DISPLAY;
    EGLContext  egl_ctx = EGL_NO_CONTEXT;
    EGLSurface  egl_srf = EGL_NO_SURFACE;
    EGLConfig   egl_cfg = nullptr;

    Atom wm_protocols;
    Atom wm_delete;
    Atom net_wm_sync_request;
    Atom net_wm_sync_counter;

    int width = 900;
    int height = 700;
    bool running = true;
    GlfwWindow* owner = nullptr;
};

#endif

// ---------------------------------------------------------------------------
// GlfwWindow implementation — delegates to Wayland or X11 backend
// ---------------------------------------------------------------------------

struct GlfwWindow::Impl {
#ifdef HAVE_WAYLAND
    WlState wl;
#endif
#ifdef HAVE_X11
    X11State x11;
#endif
    bool use_wayland = false;
};

// We'll fill these in progressively. For now, stubs.

GlfwWindow::GlfwWindow() : impl_(new Impl()) {}
GlfwWindow::~GlfwWindow() { delete impl_; }

bool GlfwWindow::Init(int, int, const char*) { return false; }
void GlfwWindow::Show() {}
void GlfwWindow::SetTitle(const char*) {}
bool GlfwWindow::ShouldClose() const { return true; }
void GlfwWindow::RequestClose() {}
void GlfwWindow::PollEvents() {}
void GlfwWindow::WaitEvents() {}
void GlfwWindow::WaitEventsTimeout(double) {}
void GlfwWindow::SwapBuffers() {}
int GlfwWindow::Width() const { return 0; }
int GlfwWindow::Height() const { return 0; }
float GlfwWindow::Scale() const { return 1.0f; }
void GlfwWindow::SetClipboard(const std::string&) {}
void GlfwWindow::Minimize() {}
void GlfwWindow::ToggleMaximize() {}
bool GlfwWindow::IsMaximized() const { return false; }
void GlfwWindow::SetTitlebarConfigPx(int, const RectI&) {}
void GlfwWindow::PostEmptyEvent() {}