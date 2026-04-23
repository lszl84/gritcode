// Gritcode — Platform-native window (Linux: Wayland + X11)
// Adapted from tt's main.cpp — Wayland primary, X11 fallback.

#ifdef __APPLE__
#error "This file is for Linux only"
#endif

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

// Wayland
#ifdef HAVE_WAYLAND
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <wayland-cursor.h>
#include "xdg-shell-client-protocol.h"
#include <xkbcommon/xkbcommon.h>
#endif

// X11
#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/sync.h>
#include <X11/keysym.h>
#include <EGL/egl.h>
#endif

#include <poll.h>
#include <cerrno>
#include <unistd.h>
#include <sys/mman.h>

#include <cstring>
#include <cstdlib>
#include <cstdio>

#include "window.h"
#include "csd.h"

// ============================================================================
// Keycode translation (shared)
// ============================================================================

static int TranslateKey(int keycode, int mods) {
    bool shift = mods & 1;
    bool ctrl  = mods & 4;
    // Letters
    if (keycode >= 'a' && keycode <= 'z') {
        if (ctrl) return keycode - 'a' + 1;  // ^A=1, ^B=2, ...
        return shift ? (keycode - 'a' + 'A') : keycode;
    }
    // Digits with shift
    if (shift) {
        switch (keycode) {
        case '1': return '!'; case '2': return '@'; case '3': return '#';
        case '4': return '$'; case '5': return '%'; case '6': return '^';
        case '7': return '&'; case '8': return '*'; case '9': return '(';
        case '0': return ')';
        case '-': return '_'; case '=': return '+';
        case '[': return '{'; case ']': return '}'; case '\\': return '|';
        case ';': return ':'; case '\'': return '"';
        case ',': return '<'; case '.': return '>'; case '/': return '?';
        case '`': return '~';
        }
    }
    return keycode;
}

// ============================================================================
// Wayland backend
// ============================================================================

#ifdef HAVE_WAYLAND

struct WlState {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm_base = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_shm* shm = nullptr;

    wl_surface* surface = nullptr;
    xdg_surface* xsurface = nullptr;
    xdg_toplevel* toplevel = nullptr;

    wl_egl_window* egl_window = nullptr;
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLConfig egl_config = nullptr;

    int width = 1000, height = 750;
    int pending_width = 0, pending_height = 0;
    int scale = 1;
    int applied_buffer_w = 0, applied_buffer_h = 0;

    bool running = true;
    bool dirty = true;
    bool frame_pending = false;
    bool keyboard_focus = false;

    double px = 0, py = 0;
    bool left_down = false;

    CsdCompositor* csd = nullptr;  // pointer to AppWindow::Impl's CSD
    bool has_csd = false;

    // Callbacks
    AppWindow::ResizeCb resizeCb;
    AppWindow::MouseBtnCb mouseBtnCb;
    AppWindow::MouseMoveCb mouseMoveCb;
    AppWindow::ScrollCb scrollCb;
    AppWindow::KeyCb keyCb;
    AppWindow::CharCb charCb;

    // Keyboard
    xkb_context* xkb_ctx = nullptr;
    xkb_keymap* xkb_km = nullptr;
    xkb_state* xkb_st = nullptr;
    uint32_t mods = 0;
};

static void registry_add(void* data, wl_registry* reg, uint32_t id,
                         const char* iface, uint32_t ver) {
    auto* st = static_cast<WlState*>(data);
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        st->compositor = (wl_compositor*)wl_registry_bind(reg, id, &wl_compositor_interface, 4);
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
        st->wm_base = (xdg_wm_base*)wl_registry_bind(reg, id, &xdg_wm_base_interface, 1);
    else if (strcmp(iface, wl_seat_interface.name) == 0)
        st->seat = (wl_seat*)wl_registry_bind(reg, id, &wl_seat_interface, ver < 5 ? ver : 5);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        st->shm = (wl_shm*)wl_registry_bind(reg, id, &wl_shm_interface, 1);
}
static void registry_remove(void*, wl_registry*, uint32_t) {}
static wl_registry_listener registry_listener = { registry_add, registry_remove };

static void xdg_wm_base_ping(void*, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
static xdg_wm_base_listener wm_base_listener = { xdg_wm_base_ping };

static void xdg_surface_configure(void* data, xdg_surface* s, uint32_t serial) {
    auto* st = static_cast<WlState*>(data);
    xdg_surface_ack_configure(s, serial);
    st->dirty = true;
}
static xdg_surface_listener xdg_surface_listener_impl = { xdg_surface_configure };

static void toplevel_configure(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
    auto* st = static_cast<WlState*>(data);
    if (w > 0) st->pending_width = w;
    if (h > 0) st->pending_height = h;
}
static void toplevel_close(void* data, xdg_toplevel*) {
    static_cast<WlState*>(data)->running = false;
}
static xdg_toplevel_listener toplevel_listener_impl = { toplevel_configure, toplevel_close };

static void surface_preferred_buffer_scale(void* data, wl_surface*, int32_t factor) {
    auto* st = static_cast<WlState*>(data);
    if (factor < 1) factor = 1;
    st->scale = factor;
    wl_surface_set_buffer_scale(st->surface, factor);
    st->applied_buffer_w = 0;  // force resize
    st->dirty = true;
}
static void surface_enter(void*, wl_surface*, wl_output*) {}
static void surface_leave(void*, wl_surface*, wl_output*) {}
static wl_surface_listener surface_listener_impl = {
    surface_enter, surface_leave,
    surface_preferred_buffer_scale, nullptr
};

static void pointer_enter(void* data, wl_pointer*, uint32_t serial,
                          wl_surface*, wl_fixed_t sx, wl_fixed_t sy) {
    auto* st = static_cast<WlState*>(data);
    st->px = wl_fixed_to_double(sx);
    st->py = wl_fixed_to_double(sy);
    if (st->has_csd && st->csd) {
        st->csd->SetCloseHover(st->csd->InCloseButton((int)st->px, (int)st->py, st->width, st->scale));
    }
    double app_y = st->py - (st->has_csd ? CsdCompositor::TITLEBAR_H : 0);
    if (app_y >= 0 && st->mouseMoveCb) st->mouseMoveCb(st->px, app_y, st->left_down);
}
static void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    auto* st = static_cast<WlState*>(data);
    if (st->has_csd && st->csd) {
        st->csd->SetCloseHover(false);
    }
}
static void pointer_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    auto* st = static_cast<WlState*>(data);
    st->px = wl_fixed_to_double(sx);
    st->py = wl_fixed_to_double(sy);
    if (st->has_csd && st->csd) {
        bool in_close = st->csd->InCloseButton((int)st->px, (int)st->py, st->width, st->scale);
        st->csd->SetCloseHover(in_close);
    }
    double app_y = st->py - (st->has_csd ? CsdCompositor::TITLEBAR_H : 0);
    if (app_y >= 0 && st->mouseMoveCb) st->mouseMoveCb(st->px, app_y, st->left_down);
}
static void pointer_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    auto* st = static_cast<WlState*>(data);
    if (button == 272) {  // BTN_LEFT
        st->left_down = (state == 1);
        if (st->has_csd && st->csd && state == 1) {
            // Check close button click
            if (st->csd->InCloseButton((int)st->px, (int)st->py, st->width, st->scale)) {
                st->running = false;
                return;
            }
            // Ignore clicks in titlebar (not on close button)
            if (st->csd->InTitlebar((int)st->px, (int)st->py, st->scale)) {
                return;
            }
        }
        double app_y = st->py - (st->has_csd ? CsdCompositor::TITLEBAR_H : 0);
        if (st->mouseBtnCb) st->mouseBtnCb(st->px, app_y, st->left_down, false);
    }
}
static void pointer_axis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    auto* st = static_cast<WlState*>(data);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && st->scrollCb)
        st->scrollCb(wl_fixed_to_double(value) / 120.0);
}
static wl_pointer_listener pointer_listener_impl = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button,
    pointer_axis, nullptr, nullptr, nullptr, nullptr
};

static void keyboard_keymap(void* data, wl_keyboard*, uint32_t fmt, int32_t fd, uint32_t sz) {
    auto* st = static_cast<WlState*>(data);
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char* map = (char*)mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }
    if (!st->xkb_ctx) st->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (st->xkb_km) xkb_keymap_unref(st->xkb_km);
    st->xkb_km = xkb_keymap_new_from_string(st->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, sz); close(fd);
    if (st->xkb_st) xkb_state_unref(st->xkb_st);
    st->xkb_st = st->xkb_km ? xkb_state_new(st->xkb_km) : nullptr;
}
static void keyboard_enter(void* data, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {
    static_cast<WlState*>(data)->keyboard_focus = true;
}
static void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
    static_cast<WlState*>(data)->keyboard_focus = false;
}
static void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    auto* st = static_cast<WlState*>(data);
    if (!st->xkb_st) return;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(st->xkb_st, key + 8);
    int keycode = 0;
    // Map common keysyms
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) keycode = 200 + (sym - XKB_KEY_F1);
    else if (sym == XKB_KEY_Escape) keycode = 27;
    else if (sym == XKB_KEY_BackSpace) keycode = 127;
    else if (sym == XKB_KEY_Delete) keycode = 300;
    else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) keycode = 13;
    else if (sym == XKB_KEY_Tab) keycode = 9;
    else if (sym == XKB_KEY_Left) keycode = 301;
    else if (sym == XKB_KEY_Right) keycode = 302;
    else if (sym == XKB_KEY_Up) keycode = 303;
    else if (sym == XKB_KEY_Down) keycode = 304;
    else if (sym == XKB_KEY_Home) keycode = 305;
    else if (sym == XKB_KEY_End) keycode = 306;
    else if (sym == XKB_KEY_Shift_L || sym == XKB_KEY_Shift_R) ;
    else if (sym == XKB_KEY_Control_L || sym == XKB_KEY_Control_R) ;
    else if (sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R) ;
    else if (sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) ;
    else keycode = TranslateKey((int)sym, st->mods);

    if (keycode && st->keyCb)
        st->keyCb(keycode, st->mods, state == 1);
}
static void keyboard_mods(void* data, wl_keyboard*, uint32_t, uint32_t depressed,
                          uint32_t latched, uint32_t locked, uint32_t group) {
    auto* st = static_cast<WlState*>(data);
    if (st->xkb_st) xkb_state_update_mask(st->xkb_st, depressed, latched, locked, 0, 0, group);
    st->mods = 0;
    if (depressed & (1 << 0)) st->mods |= 1;   // Shift
    if (depressed & (1 << 2)) st->mods |= 4;   // Ctrl
    if (depressed & (1 << 3)) st->mods |= 8;   // Alt
    if (depressed & (1 << 6)) st->mods |= 16;  // Super
}
static wl_keyboard_listener keyboard_listener_impl = {
    keyboard_keymap, keyboard_enter, keyboard_leave,
    keyboard_key, keyboard_mods, nullptr
};

static void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto* st = static_cast<WlState*>(data);
    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        if (!st->pointer) {
            st->pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(st->pointer, &pointer_listener_impl, st);
        }
    }
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (!st->keyboard) {
            st->keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(st->keyboard, &keyboard_listener_impl, st);
        }
    }
}
static wl_seat_listener seat_listener_impl = { seat_capabilities, nullptr };

static void frame_done(void* data, wl_callback* cb, uint32_t) {
    wl_callback_destroy(cb);
    static_cast<WlState*>(data)->frame_pending = false;
}
static wl_callback_listener frame_listener = { frame_done };

static bool wl_init_egl(WlState* st) {
    st->egl_display = eglGetDisplay((EGLNativeDisplayType)st->display);
    if (st->egl_display == EGL_NO_DISPLAY) return false;
    EGLint major, minor;
    if (!eglInitialize(st->egl_display, &major, &minor)) return false;

    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint n;
    if (!eglChooseConfig(st->egl_display, attribs, &st->egl_config, 1, &n) || n < 1) return false;

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    st->egl_context = eglCreateContext(st->egl_display, st->egl_config,
                                       EGL_NO_CONTEXT, ctx_attribs);
    if (st->egl_context == EGL_NO_CONTEXT) return false;

    st->egl_window = wl_egl_window_create(st->surface, st->width * st->scale, st->height * st->scale);
    st->egl_surface = eglCreateWindowSurface(st->egl_display, st->egl_config,
                                             (EGLNativeWindowType)st->egl_window, nullptr);
    if (st->egl_surface == EGL_NO_SURFACE) return false;

    eglMakeCurrent(st->egl_display, st->egl_surface, st->egl_surface, st->egl_context);
    eglSwapInterval(st->egl_display, 0);
    return true;
}

static void wl_commit(WlState* st) {
    if (!st->dirty || st->frame_pending) return;

    if (st->pending_width > 0 && st->pending_height > 0) {
        st->width = st->pending_width;
        st->height = st->pending_height;
        st->pending_width = 0;
        st->pending_height = 0;
    }
    int want_w = st->width * st->scale;
    int want_h = st->height * st->scale;
    if (want_w != st->applied_buffer_w || want_h != st->applied_buffer_h) {
        wl_egl_window_resize(st->egl_window, want_w, want_h, 0, 0);
        st->applied_buffer_w = want_w;
        st->applied_buffer_h = want_h;
    }

    wl_callback* cb = wl_surface_frame(st->surface);
    wl_callback_add_listener(cb, &frame_listener, st);
    st->frame_pending = true;
    eglSwapBuffers(st->egl_display, st->egl_surface);
    st->dirty = false;
}

#endif // HAVE_WAYLAND

// ============================================================================
// X11 backend
// ============================================================================

#ifdef HAVE_X11

struct X11State {
    Display* display = nullptr;
    Window window = 0;
    Atom wmDelete;
    Atom netWmSyncRequest;
    Atom netWmSyncRequestCounter;
    XSyncCounter syncCounter = 0;

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLConfig egl_config = nullptr;

    int width = 1000, height = 750;
    int fb_w = 1000, fb_h = 750;
    float scale = 1.0f;
    bool running = true;

    double px = 0, py = 0;
    bool left_down = false;

    AppWindow::ResizeCb resizeCb;
    AppWindow::MouseBtnCb mouseBtnCb;
    AppWindow::MouseMoveCb mouseMoveCb;
    AppWindow::ScrollCb scrollCb;
    AppWindow::KeyCb keyCb;
    AppWindow::CharCb charCb;
};

static bool x11_init_egl(X11State* st) {
    st->egl_display = eglGetDisplay((EGLNativeDisplayType)st->display);
    if (st->egl_display == EGL_NO_DISPLAY) return false;
    EGLint major, minor;
    if (!eglInitialize(st->egl_display, &major, &minor)) return false;

    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint n;
    if (!eglChooseConfig(st->egl_display, attribs, &st->egl_config, 1, &n) || n < 1) return false;

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    st->egl_context = eglCreateContext(st->egl_display, st->egl_config,
                                       EGL_NO_CONTEXT, ctx_attribs);
    if (st->egl_context == EGL_NO_CONTEXT) return false;

    st->egl_surface = eglCreateWindowSurface(st->egl_display, st->egl_config,
                                             (EGLNativeWindowType)st->window, nullptr);
    if (st->egl_surface == EGL_NO_SURFACE) return false;

    eglMakeCurrent(st->egl_display, st->egl_surface, st->egl_surface, st->egl_context);
    eglSwapInterval(st->egl_display, 0);
    return true;
}

static void x11_process_events(X11State* st) {
    XEvent e;
    while (XPending(st->display)) {
        XNextEvent(st->display, &e);
        switch (e.type) {
        case ConfigureNotify: {
            if (e.xconfigure.width != st->width || e.xconfigure.height != st->height) {
                st->width = e.xconfigure.width;
                st->height = e.xconfigure.height;
                st->fb_w = st->width;
                st->fb_h = st->height;
                if (st->resizeCb) st->resizeCb(st->width, st->height, st->scale);
            }
            break;
        }
        case ClientMessage: {
            if ((Atom)e.xclient.data.l[0] == st->wmDelete)
                st->running = false;
            break;
        }
        case ButtonPress: {
            if (e.xbutton.button == 1) {
                st->left_down = true;
                st->px = e.xbutton.x; st->py = e.xbutton.y;
                if (st->mouseBtnCb) st->mouseBtnCb(st->px, st->py, true, false);
            } else if (e.xbutton.button == 4 && st->scrollCb) {
                st->scrollCb(1.0);
            } else if (e.xbutton.button == 5 && st->scrollCb) {
                st->scrollCb(-1.0);
            }
            break;
        }
        case ButtonRelease: {
            if (e.xbutton.button == 1) {
                st->left_down = false;
                st->px = e.xbutton.x; st->py = e.xbutton.y;
                if (st->mouseBtnCb) st->mouseBtnCb(st->px, st->py, false, false);
            }
            break;
        }
        case MotionNotify: {
            st->px = e.xmotion.x; st->py = e.xmotion.y;
            if (st->mouseMoveCb) st->mouseMoveCb(st->px, st->py, st->left_down);
            break;
        }
        case KeyPress: {
            KeySym sym = XLookupKeysym(&e.xkey, 0);
            int mods = 0;
            if (e.xkey.state & ShiftMask) mods |= 1;
            if (e.xkey.state & ControlMask) mods |= 4;
            if (e.xkey.state & Mod1Mask) mods |= 8;
            int keycode = TranslateKey((int)sym, mods);
            if (keycode && st->keyCb) st->keyCb(keycode, mods, true);
            break;
        }
        case KeyRelease: {
            KeySym sym = XLookupKeysym(&e.xkey, 0);
            int mods = 0;
            if (e.xkey.state & ShiftMask) mods |= 1;
            if (e.xkey.state & ControlMask) mods |= 4;
            if (e.xkey.state & Mod1Mask) mods |= 8;
            int keycode = TranslateKey((int)sym, mods);
            if (keycode && st->keyCb) st->keyCb(keycode, mods, false);
            break;
        }
        }
    }
}

#endif // HAVE_X11

// ============================================================================
// Window implementation (Pimpl)
// ============================================================================

struct AppWindow::Impl {
#ifdef HAVE_WAYLAND
    WlState* wl = nullptr;
#endif
#ifdef HAVE_X11
    X11State* x11 = nullptr;
#endif
    bool use_wayland = false;
    CsdCompositor* csd = nullptr;
    bool has_csd = false;
};

AppWindow::AppWindow() : impl_(new Impl()) {}
AppWindow::~AppWindow() {
    if (impl_->csd) {
        impl_->csd->Shutdown();
        delete impl_->csd;
    }
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        auto* st = impl_->wl;
        eglMakeCurrent(st->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(st->egl_display, st->egl_surface);
        eglDestroyContext(st->egl_display, st->egl_context);
        wl_egl_window_destroy(st->egl_window);
        eglTerminate(st->egl_display);
        if (st->xkb_st) xkb_state_unref(st->xkb_st);
        if (st->xkb_km) xkb_keymap_unref(st->xkb_km);
        if (st->xkb_ctx) xkb_context_unref(st->xkb_ctx);
        delete st;
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) {
        auto* st = impl_->x11;
        eglMakeCurrent(st->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(st->egl_display, st->egl_surface);
        eglDestroyContext(st->egl_display, st->egl_context);
        eglTerminate(st->egl_display);
        XDestroyWindow(st->display, st->window);
        XCloseDisplay(st->display);
        delete st;
    }
#endif
    delete impl_;
}

bool AppWindow::Init(int width, int height, const char* title) {
#ifdef HAVE_WAYLAND
    if (const char* d = getenv("WAYLAND_DISPLAY")) {
        auto* st = new WlState();
        st->width = width; st->height = height;

        st->display = wl_display_connect(nullptr);
        if (!st->display) { delete st; goto try_x11; }

        wl_registry* reg = wl_display_get_registry(st->display);
        wl_registry_add_listener(reg, &registry_listener, st);
        wl_display_roundtrip(st->display);

        if (!st->compositor || !st->wm_base) {
            delete st; goto try_x11;
        }

        xdg_wm_base_add_listener(st->wm_base, &wm_base_listener, st);

        st->surface = wl_compositor_create_surface(st->compositor);
        wl_surface_add_listener(st->surface, &surface_listener_impl, st);
        wl_surface_set_buffer_scale(st->surface, st->scale);

        st->xsurface = xdg_wm_base_get_xdg_surface(st->wm_base, st->surface);
        xdg_surface_add_listener(st->xsurface, &xdg_surface_listener_impl, st);
        st->toplevel = xdg_surface_get_toplevel(st->xsurface);
        xdg_toplevel_add_listener(st->toplevel, &toplevel_listener_impl, st);
        xdg_toplevel_set_title(st->toplevel, title);
        xdg_toplevel_set_app_id(st->toplevel, "com.devmindscape.gritcode");
        wl_surface_commit(st->surface);

        // Wait for configure
        wl_display_roundtrip(st->display);

        if (!wl_init_egl(st)) {
            delete st; goto try_x11;
        }

        // CSD disabled: Mesa driver crashes on our shaders (gallium bug).
        // Re-enable after shader debugging.
        (void)0;

        impl_->wl = st;
        impl_->use_wayland = true;
        return true;
    }
try_x11:
#endif

#ifdef HAVE_X11
    {
        auto* st = new X11State();
        st->width = width; st->height = height;
        st->fb_w = width; st->fb_h = height;

        st->display = XOpenDisplay(nullptr);
        if (!st->display) { delete st; return false; }

        int screen = DefaultScreen(st->display);
        st->window = XCreateSimpleWindow(st->display, RootWindow(st->display, screen),
                                          100, 100, width, height, 0,
                                          BlackPixel(st->display, screen),
                                          WhitePixel(st->display, screen));

        st->wmDelete = XInternAtom(st->display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(st->display, st->window, &st->wmDelete, 1);
        XSelectInput(st->display, st->window,
                     ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
        XStoreName(st->display, st->window, title);

        if (!x11_init_egl(st)) {
            XDestroyWindow(st->display, st->window);
            XCloseDisplay(st->display);
            delete st;
            return false;
        }

        impl_->x11 = st;
        return true;
    }
#endif

    return false;
}

void AppWindow::Show() {
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        wl_surface_commit(impl_->wl->surface);
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) {
        XMapWindow(impl_->x11->display, impl_->x11->window);
    }
#endif
}

void AppWindow::SetTitle(const char* title) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) xdg_toplevel_set_title(impl_->wl->toplevel, title);
#endif
#ifdef HAVE_X11
    if (impl_->x11) XStoreName(impl_->x11->display, impl_->x11->window, title);
#endif
}

bool AppWindow::ShouldClose() const {
#ifdef HAVE_WAYLAND
    if (impl_->wl) return !impl_->wl->running;
#endif
#ifdef HAVE_X11
    if (impl_->x11) return !impl_->x11->running;
#endif
    return true;
}

void AppWindow::SetShouldClose(bool close) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->running = !close;
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->running = !close;
#endif
}

void AppWindow::PollEvents() {
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        wl_display_dispatch_pending(impl_->wl->display);
        wl_display_flush(impl_->wl->display);
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) x11_process_events(impl_->x11);
#endif
}

void AppWindow::WaitEvents() {
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        wl_display_dispatch(impl_->wl->display);
        wl_display_flush(impl_->wl->display);
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) {
        XPeekEvent(impl_->x11->display, nullptr);  // blocks
        x11_process_events(impl_->x11);
    }
#endif
}

void AppWindow::WaitEventsTimeout(double timeout) {
    int ms = (int)(timeout * 1000);
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        wl_display_dispatch_pending(impl_->wl->display);
        wl_display_flush(impl_->wl->display);
        poll(nullptr, 0, ms);
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) {
        x11_process_events(impl_->x11);
        poll(nullptr, 0, ms);
    }
#endif
}

AppWindow::ContentFrame AppWindow::BeginContentFrame() {
    if (impl_->has_csd && impl_->wl) {
        auto f = impl_->csd->BeginFrame(impl_->wl->width, impl_->wl->height, impl_->wl->scale);
        return {f.width, f.height, impl_->wl->width, impl_->wl->height - CsdCompositor::TITLEBAR_H, CsdCompositor::TITLEBAR_H};
    }
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        int w = impl_->wl->width * impl_->wl->scale;
        int h = impl_->wl->height * impl_->wl->scale;
        return {w, h, impl_->wl->width, impl_->wl->height, 0};
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) {
        return {impl_->x11->fb_w, impl_->x11->fb_h, impl_->x11->width, impl_->x11->height, 0};
    }
#endif
    return {0, 0, 0, 0, 0};
}

void AppWindow::EndContentFrame() {
    if (impl_->has_csd && impl_->wl) {
        impl_->csd->EndFrame(impl_->wl->width, impl_->wl->height, impl_->wl->scale);
        eglSwapBuffers(impl_->wl->egl_display, impl_->wl->egl_surface);
        return;
    }
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        wl_callback* cb = wl_surface_frame(impl_->wl->surface);
        wl_callback_add_listener(cb, &frame_listener, impl_->wl);
        impl_->wl->frame_pending = true;
        eglSwapBuffers(impl_->wl->egl_display, impl_->wl->egl_surface);
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) eglSwapBuffers(impl_->x11->egl_display, impl_->x11->egl_surface);
#endif
}

void AppWindow::SwapBuffers() {
    // Deprecated: use BeginContentFrame/EndContentFrame instead
    EndContentFrame();
}

int AppWindow::Width() const {
    // Content width (excluding titlebar)
#ifdef HAVE_WAYLAND
    if (impl_->wl) return impl_->wl->width * impl_->wl->scale;
#endif
#ifdef HAVE_X11
    if (impl_->x11) return impl_->x11->fb_w;
#endif
    return 0;
}
int AppWindow::Height() const {
    // Content height (excluding titlebar)
#ifdef HAVE_WAYLAND
    if (impl_->wl) return (impl_->wl->height - (impl_->has_csd ? CsdCompositor::TITLEBAR_H : 0)) * impl_->wl->scale;
#endif
#ifdef HAVE_X11
    if (impl_->x11) return impl_->x11->fb_h;
#endif
    return 0;
}
int AppWindow::FullWidth() const {
#ifdef HAVE_WAYLAND
    if (impl_->wl) return impl_->wl->width;
#endif
#ifdef HAVE_X11
    if (impl_->x11) return impl_->x11->width;
#endif
    return 0;
}
int AppWindow::FullHeight() const {
#ifdef HAVE_WAYLAND
    if (impl_->wl) return impl_->wl->height;
#endif
#ifdef HAVE_X11
    if (impl_->x11) return impl_->x11->height;
#endif
    return 0;
}
int AppWindow::LogicalW() const {
#ifdef HAVE_WAYLAND
    if (impl_->wl) return impl_->wl->width;
#endif
#ifdef HAVE_X11
    if (impl_->x11) return impl_->x11->width;
#endif
    return 0;
}
int AppWindow::LogicalH() const {
#ifdef HAVE_WAYLAND
    if (impl_->wl) return impl_->wl->height - (impl_->has_csd ? CsdCompositor::TITLEBAR_H : 0);
#endif
#ifdef HAVE_X11
    if (impl_->x11) return impl_->x11->height;
#endif
    return 0;
}
float AppWindow::Scale() const {
#ifdef HAVE_WAYLAND
    if (impl_->wl) return impl_->wl->scale;
#endif
#ifdef HAVE_X11
    if (impl_->x11) return impl_->x11->scale;
#endif
    return 1.0f;
}

void AppWindow::OnResize(ResizeCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->resizeCb = cb;
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->resizeCb = cb;
#endif
}
void AppWindow::OnMouseButton(MouseBtnCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->mouseBtnCb = cb;
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->mouseBtnCb = cb;
#endif
}
void AppWindow::OnMouseMove(MouseMoveCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->mouseMoveCb = cb;
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->mouseMoveCb = cb;
#endif
}
void AppWindow::OnScrollEvent(ScrollCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->scrollCb = cb;
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->scrollCb = cb;
#endif
}
void AppWindow::OnKeyEvent(KeyCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->keyCb = cb;
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->keyCb = cb;
#endif
}
void AppWindow::OnCharEvent(CharCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->charCb = cb;
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->charCb = cb;
#endif
}

void AppWindow::SetClipboard(const std::string& text) {
#ifdef HAVE_X11
    if (impl_->x11) {
        Atom clipboard = XInternAtom(impl_->x11->display, "CLIPBOARD", False);
        XSetSelectionOwner(impl_->x11->display, clipboard, impl_->x11->window, CurrentTime);
        // TODO: respond to SelectionRequest
    }
#endif
}
std::string AppWindow::GetClipboard() {
#ifdef HAVE_X11
    if (impl_->x11) {
        Atom clipboard = XInternAtom(impl_->x11->display, "CLIPBOARD", False);
        Atom utf8 = XInternAtom(impl_->x11->display, "UTF8_STRING", False);
        Window owner = XGetSelectionOwner(impl_->x11->display, clipboard);
        if (owner == None) return "";
        XConvertSelection(impl_->x11->display, clipboard, utf8, utf8, impl_->x11->window, CurrentTime);
        XFlush(impl_->x11->display);
        // TODO: read SelectionNotify
    }
#endif
    return "";
}

void* AppWindow::NativeHandle() const {
#ifdef HAVE_WAYLAND
    if (impl_->wl) return impl_->wl->surface;
#endif
#ifdef HAVE_X11
    if (impl_->x11) return (void*)impl_->x11->window;
#endif
    return nullptr;
}
