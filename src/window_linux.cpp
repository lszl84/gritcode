// Gritcode — Platform-native window (Linux: Wayland + X11).
//
// Public interface in window.h. Two complete backends behind the same API:
//
//   Wayland: wl_display + xdg-shell + wayland-cursor + xkbcommon.
//            xdg-decoration requested server-side; if the compositor forces
//            client-side (GNOME), our CSD compositor draws a flat titlebar
//            with a close button.
//
//   X11:     Xlib + EGL + XSync for smooth resize.
//
// Copyright (C) 2026 luke@devmindscape.com. GPL v3.

#ifdef __APPLE__
#error "This file is for Linux / BSD only"
#endif

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <EGL/egl.h>

#include "window.h"
#include "csd.h"
#include "keysyms.h"

#ifdef HAVE_WAYLAND
#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include <xkbcommon/xkbcommon.h>
#endif

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/sync.h>
#endif

#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

// ============================================================================
// Modifier bits must match types.h:  Mod::Ctrl = 1, Mod::Shift = 2
// (plus optional bit 2 for Alt and bit 3 for Super; app.cpp only uses Ctrl/Shift)
// ============================================================================
static constexpr int MOD_CTRL  = 1;
static constexpr int MOD_SHIFT = 2;
static constexpr int MOD_ALT   = 4;
static constexpr int MOD_SUPER = 8;

// ============================================================================
// Wayland backend
// ============================================================================
#ifdef HAVE_WAYLAND

struct WlState {
    // Globals
    wl_display*                  display = nullptr;
    wl_compositor*               compositor = nullptr;
    xdg_wm_base*                 wm_base = nullptr;
    zxdg_decoration_manager_v1*  deco_mgr = nullptr;
    wl_seat*                     seat = nullptr;
    wl_shm*                      shm = nullptr;
    wl_data_device_manager*      data_device_mgr = nullptr;

    // Input
    wl_pointer*  pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    wl_touch*    touch = nullptr;

    // Cursor
    wl_cursor_theme* cursor_theme = nullptr;
    wl_surface*      cursor_surface = nullptr;
    uint32_t         last_enter_serial = 0;

    // Surfaces
    wl_surface*   surface = nullptr;
    xdg_surface*  xsurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    zxdg_toplevel_decoration_v1* deco = nullptr;

    // EGL
    wl_egl_window* egl_window = nullptr;
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLConfig  egl_config = nullptr;

    // Window state (logical units — compositor coordinates)
    int width = 1000, height = 750;
    int pending_width = 0, pending_height = 0;
    int scale = 1;
    int applied_buffer_w = 0, applied_buffer_h = 0;
    bool configured = false;
    bool running = true;
    bool dirty = true;
    bool frame_pending = false;
    bool keyboard_focus = false;

    // Pointer state (logical coords)
    double px = 0, py = 0;
    bool left_down = false;
    const char* current_cursor = nullptr;

    // Keyboard (xkbcommon)
    xkb_context* xkb_ctx = nullptr;
    xkb_keymap*  xkb_km = nullptr;
    xkb_state*   xkb_st = nullptr;
    xkb_mod_index_t mod_ctrl = XKB_MOD_INVALID;
    xkb_mod_index_t mod_shift = XKB_MOD_INVALID;
    xkb_mod_index_t mod_alt = XKB_MOD_INVALID;
    xkb_mod_index_t mod_super = XKB_MOD_INVALID;
    int mods = 0;
    // Key repeat
    uint32_t repeat_key = 0;
    xkb_keysym_t repeat_sym = 0;
    uint32_t repeat_cp = 0;
    int repeat_rate = 25;       // events per second after delay
    int repeat_delay_ms = 400;  // initial delay
    double repeat_next = 0;     // CLOCK_MONOTONIC seconds

    // CSD
    bool use_csd = false;
    bool floating = true;          // tiled/maximized/fullscreen disables shadow+corners
    uint32_t current_edge = 0;     // hover edge under pointer (0 = none)
    CsdCompositor* csd = nullptr;

    // Active touch point (we track only the first finger for simple
    // single-touch tap/drag — matches how wl_pointer is wired).
    int32_t touch_id = -1;
    double tx = 0, ty = 0;

    // Clipboard
    wl_data_device* data_device = nullptr;
    wl_data_offer* pending_offer = nullptr;       // latest generic offer
    std::vector<std::string> pending_offer_mimes;
    wl_data_offer* selection_offer = nullptr;     // current clipboard owner
    std::vector<std::string> selection_offer_mimes;
    std::string owned_clipboard;                   // string we're advertising
    wl_data_source* active_source = nullptr;
    uint32_t latest_serial = 0;                    // for set_selection

    // Callbacks
    AppWindow::ResizeCb    resizeCb;
    AppWindow::MouseBtnCb  mouseBtnCb;
    AppWindow::MouseMoveCb mouseMoveCb;
    AppWindow::ScrollCb    scrollCb;
    AppWindow::KeyCb       keyCb;
    AppWindow::CharCb      charCb;
};

static double wl_monotonic() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// Logical-pixel shadow margin around the surface content. Zero while the
// window is tiled / maximized / fullscreen (no room for a drop shadow then).
static int wl_effective_shadow(const WlState* st) {
    return (st->use_csd && st->floating) ? CsdCompositor::SHADOW_EXTENT : 0;
}

// CSS-style cursor name → legacy X11 name fallback, for themes that only ship
// one of the two spellings.
static const char* wl_cursor_legacy_alias(const char* name) {
    struct A { const char* css; const char* legacy; };
    static const A ALIAS[] = {
        { "default",   "left_ptr" },
        { "n-resize",  "top_side" },       { "s-resize",  "bottom_side" },
        { "w-resize",  "left_side" },      { "e-resize",  "right_side" },
        { "nw-resize", "top_left_corner" },{ "ne-resize", "top_right_corner" },
        { "sw-resize", "bottom_left_corner" },
        { "se-resize", "bottom_right_corner" },
    };
    for (const auto& a : ALIAS) if (!strcmp(name, a.css)) return a.legacy;
    return nullptr;
}

static void wl_set_cursor(WlState* st, const char* name) {
    if (!st->pointer || !st->cursor_theme || !st->cursor_surface) return;
    if (st->current_cursor && !strcmp(st->current_cursor, name)) return;
    wl_cursor* cur = wl_cursor_theme_get_cursor(st->cursor_theme, name);
    if (!cur) {
        if (const char* alt = wl_cursor_legacy_alias(name))
            cur = wl_cursor_theme_get_cursor(st->cursor_theme, alt);
    }
    if (!cur && strcmp(name, "default")) cur = wl_cursor_theme_get_cursor(st->cursor_theme, "left_ptr");
    if (!cur || !cur->image_count) return;
    wl_cursor_image* img = cur->images[0];
    wl_buffer* buf = wl_cursor_image_get_buffer(img);
    if (!buf) return;
    // The theme was loaded at cursor_size * scale, so img->width/hotspot are
    // in buffer (physical) pixels. Tell the compositor the surface is HiDPI
    // so it treats the buffer at that scale, and convert the hotspot into
    // logical/surface-local pixels.
    int hx = img->hotspot_x / st->scale;
    int hy = img->hotspot_y / st->scale;
    wl_surface_set_buffer_scale(st->cursor_surface, st->scale);
    wl_pointer_set_cursor(st->pointer, st->last_enter_serial,
                          st->cursor_surface, hx, hy);
    wl_surface_attach(st->cursor_surface, buf, 0, 0);
    wl_surface_damage_buffer(st->cursor_surface, 0, 0, img->width, img->height);
    wl_surface_commit(st->cursor_surface);
    st->current_cursor = name;
}

static void wl_sync_geometry(WlState* st) {
    if (!st->xsurface) return;
    const int sh  = wl_effective_shadow(st);
    const int rad = (st->use_csd && st->floating) ? CsdCompositor::CORNER_RADIUS : 0;

    // Tell the compositor the "real" window lives inset by the shadow.
    xdg_surface_set_window_geometry(st->xsurface, sh, sh, st->width, st->height);

    // Input region: only the window content receives clicks; the shadow border
    // is click-through to whatever's below. SSD mode: whole surface accepts input.
    if (st->use_csd) {
        wl_region* input = wl_compositor_create_region(st->compositor);
        wl_region_add(input, sh, sh, st->width, st->height);
        wl_surface_set_input_region(st->surface, input);
        wl_region_destroy(input);
    } else {
        wl_surface_set_input_region(st->surface, nullptr);
    }

    // Opaque region: shrunk by the corner radius so the compositor knows the
    // rounded corners aren't solid. SSD: whole surface is opaque.
    wl_region* opaque = wl_compositor_create_region(st->compositor);
    if (st->use_csd) {
        if (st->width > 2 * rad && st->height > 2 * rad) {
            wl_region_add(opaque, sh + rad, sh + rad,
                          st->width - 2 * rad, st->height - 2 * rad);
        }
    } else {
        wl_region_add(opaque, 0, 0, st->width, st->height);
    }
    wl_surface_set_opaque_region(st->surface, opaque);
    wl_region_destroy(opaque);
}

// ---- xdg-shell listeners --------------------------------------------------

static void xdg_wm_base_handle_ping(void*, xdg_wm_base* b, uint32_t s) {
    xdg_wm_base_pong(b, s);
}
static const xdg_wm_base_listener wm_base_lst = { xdg_wm_base_handle_ping };

// Forward decl: apply a pending toplevel size synchronously — resize the EGL
// window, update our logical size, notify the app. Safe to call before EGL is
// up (it will just update the size fields).
static void wl_apply_pending_resize(WlState* st);

static void xdg_surface_handle_configure(void* data, xdg_surface* s, uint32_t serial) {
    auto* st = static_cast<WlState*>(data);
    xdg_surface_ack_configure(s, serial);
    st->configured = true;
    st->dirty = true;
    // Apply the pending size right here so we re-render AT the new size on the
    // very next frame. If we waited until BeginContentFrame, the app's render
    // loop would never wake up (it gates on App::dirty_, not WlState::dirty)
    // and drag-resize would silently do nothing.
    wl_apply_pending_resize(st);
}
static const xdg_surface_listener xdg_surface_lst = { xdg_surface_handle_configure };

static void toplevel_handle_configure(void* data, xdg_toplevel*,
                                      int32_t w, int32_t h, wl_array* states) {
    auto* st = static_cast<WlState*>(data);
    if (w > 0) st->pending_width = w;
    if (h > 0) st->pending_height = h;

    bool tiled = false, maximized = false, fullscreen = false;
    if (states) {
        uint32_t* s;
        for (s = (uint32_t*)states->data;
             (const char*)s < (const char*)states->data + states->size; ++s) {
            switch (*s) {
            case XDG_TOPLEVEL_STATE_MAXIMIZED:    maximized = true; break;
            case XDG_TOPLEVEL_STATE_FULLSCREEN:   fullscreen = true; break;
            case XDG_TOPLEVEL_STATE_TILED_LEFT:
            case XDG_TOPLEVEL_STATE_TILED_RIGHT:
            case XDG_TOPLEVEL_STATE_TILED_TOP:
            case XDG_TOPLEVEL_STATE_TILED_BOTTOM: tiled = true; break;
            }
        }
    }
    bool was_floating = st->floating;
    st->floating = !(tiled || maximized || fullscreen);
    if (was_floating != st->floating) {
        // Buffer geometry changes with shadow; force re-commit.
        st->applied_buffer_w = 0;
        st->applied_buffer_h = 0;
        st->dirty = true;
    }
}
static void toplevel_handle_close(void* data, xdg_toplevel*) {
    static_cast<WlState*>(data)->running = false;
}
static void toplevel_handle_configure_bounds(void*, xdg_toplevel*, int32_t, int32_t) {}
static void toplevel_handle_wm_capabilities(void*, xdg_toplevel*, wl_array*) {}
static const xdg_toplevel_listener toplevel_lst = {
    toplevel_handle_configure,
    toplevel_handle_close,
    toplevel_handle_configure_bounds,
    toplevel_handle_wm_capabilities,
};

// ---- decoration protocol: request server-side, enable CSD if refused ------

static void deco_handle_configure(void* data, zxdg_toplevel_decoration_v1*,
                                  uint32_t mode) {
    auto* st = static_cast<WlState*>(data);
    bool client_side = (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    if (client_side != st->use_csd) {
        st->use_csd = client_side;
        st->dirty = true;
        if (st->csd && client_side && !st->csd->Init()) {
            fprintf(stderr, "[grit] CSD init failed; drawing without decorations\n");
        }
    }
}
static const zxdg_toplevel_decoration_v1_listener deco_lst = { deco_handle_configure };

// ---- surface: track preferred scale ---------------------------------------

static void surface_handle_enter(void*, wl_surface*, wl_output*) {}
static void surface_handle_leave(void*, wl_surface*, wl_output*) {}
static void surface_handle_preferred_buffer_scale(void* data, wl_surface*, int32_t factor) {
    auto* st = static_cast<WlState*>(data);
    if (factor < 1) factor = 1;
    if (factor == st->scale) return;
    st->scale = factor;
    if (st->surface) wl_surface_set_buffer_scale(st->surface, factor);
    st->applied_buffer_w = 0;  // force resize on next commit
    st->dirty = true;
}
static void surface_handle_preferred_buffer_transform(void*, wl_surface*, uint32_t) {}
static const wl_surface_listener surface_lst = {
    surface_handle_enter,
    surface_handle_leave,
    surface_handle_preferred_buffer_scale,
    surface_handle_preferred_buffer_transform,
};

// ---- pointer --------------------------------------------------------------

static int wl_compute_buffer_mouse(const WlState* st, double lx, double ly,
                                   float* out_x, float* out_y) {
    // lx/ly are already window-local (shadow offset subtracted when they
    // were stored in st->px/py). Convert logical window coords → content-area
    // buffer pixels, with the Y adjusted for the CSD titlebar.
    int title_h = st->use_csd ? CsdCompositor::TITLEBAR_H : 0;
    double by = (ly - title_h) * st->scale;
    double bx = lx * st->scale;
    if (by < 0) { *out_x = (float)bx; *out_y = (float)by; return 0; }
    *out_x = (float)bx;
    *out_y = (float)by;
    return 1;
}

// Pick an appropriate cursor for the current pointer position: resize glyph
// near the border, default arrow otherwise. Kept separate so pointer_enter
// and pointer_motion share the logic.
static void wl_update_hover_cursor(WlState* st) {
    if (!st->use_csd || !st->csd) { wl_set_cursor(st, "left_ptr"); st->current_edge = 0; return; }
    uint32_t edge = st->floating
        ? st->csd->ResizeEdge((int)st->px, (int)st->py, st->width, st->height)
        : 0;
    st->current_edge = edge;
    const char* name = CsdCompositor::CursorNameForEdge(edge);
    if (!name) name = "left_ptr";
    wl_set_cursor(st, name);
}

static void pointer_handle_enter(void* data, wl_pointer*, uint32_t serial,
                                 wl_surface* surf, wl_fixed_t sx, wl_fixed_t sy) {
    auto* st = static_cast<WlState*>(data);
    if (surf != st->surface) return;
    st->last_enter_serial = serial;
    const double off = wl_effective_shadow(st);
    st->px = wl_fixed_to_double(sx) - off;
    st->py = wl_fixed_to_double(sy) - off;
    wl_update_hover_cursor(st);
    if (st->use_csd && st->csd) {
        st->csd->SetCloseHover(st->csd->InCloseButton((int)st->px, (int)st->py,
                                                      st->width));
    }
}
static void pointer_handle_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    auto* st = static_cast<WlState*>(data);
    if (st->use_csd && st->csd) st->csd->SetCloseHover(false);
    st->current_cursor = nullptr;
    st->current_edge = 0;
}
static void pointer_handle_motion(void* data, wl_pointer*, uint32_t,
                                  wl_fixed_t sx, wl_fixed_t sy) {
    auto* st = static_cast<WlState*>(data);
    const double off = wl_effective_shadow(st);
    st->px = wl_fixed_to_double(sx) - off;
    st->py = wl_fixed_to_double(sy) - off;
    wl_update_hover_cursor(st);
    if (st->use_csd && st->csd) {
        st->csd->SetCloseHover(st->csd->InCloseButton((int)st->px, (int)st->py,
                                                      st->width));
        st->dirty = true;
    }
    float bx, by;
    if (wl_compute_buffer_mouse(st, st->px, st->py, &bx, &by) && st->mouseMoveCb)
        st->mouseMoveCb(bx, by, st->left_down);
}
static void pointer_handle_button(void* data, wl_pointer*, uint32_t serial,
                                  uint32_t, uint32_t button, uint32_t state) {
    auto* st = static_cast<WlState*>(data);
    bool pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    if (button == 0x110 /* BTN_LEFT */) {
        st->left_down = pressed;
        if (st->use_csd && pressed && st->csd) {
            // Hit order: close > resize-edge > titlebar move > content click.
            if (st->csd->InCloseButton((int)st->px, (int)st->py, st->width)) {
                st->csd->SetClosePressed(true);
                st->running = false;
                return;
            }
            uint32_t edge = st->floating
                ? st->csd->ResizeEdge((int)st->px, (int)st->py, st->width, st->height)
                : 0;
            if (edge != 0 && st->toplevel) {
                xdg_toplevel_resize(st->toplevel, st->seat, serial, edge);
                st->left_down = false;   // resize grab takes over the press
                return;
            }
            if (st->csd->InTitlebar((int)st->px, (int)st->py)) {
                xdg_toplevel_move(st->toplevel, st->seat, serial);
                st->left_down = false;
                return;
            }
        }
        float bx, by;
        if (wl_compute_buffer_mouse(st, st->px, st->py, &bx, &by) && st->mouseBtnCb) {
            int mods = st->mods;
            st->mouseBtnCb(bx, by, pressed, (mods & MOD_SHIFT) != 0);
        }
    } else if (button == 0x111 /* BTN_RIGHT */) {
        if (pressed && st->use_csd && st->toplevel && st->csd &&
            st->csd->InTitlebar((int)st->px, (int)st->py) &&
            !st->csd->InCloseButton((int)st->px, (int)st->py, st->width)) {
            xdg_toplevel_show_window_menu(st->toplevel, st->seat, serial,
                                          (int)st->px, (int)st->py);
        }
    }
}
static void pointer_handle_axis(void* data, wl_pointer*, uint32_t,
                                uint32_t axis, wl_fixed_t value) {
    auto* st = static_cast<WlState*>(data);
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    // Wayland delivers "pixels" of scroll. Normalize to a notch-like delta
    // by dividing by 10 so we roughly match mouse-wheel clicks.
    double dy = wl_fixed_to_double(value);
    if (st->scrollCb) st->scrollCb(-(float)(dy / 10.0));
}
static void pointer_handle_frame(void*, wl_pointer*) {}
static void pointer_handle_axis_source(void*, wl_pointer*, uint32_t) {}
static void pointer_handle_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
static void pointer_handle_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
static void pointer_handle_axis_value120(void* data, wl_pointer*, uint32_t axis, int32_t val120) {
    auto* st = static_cast<WlState*>(data);
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    if (st->scrollCb) st->scrollCb(-(float)val120 / 120.0f);
}
static void pointer_handle_axis_relative_direction(void*, wl_pointer*, uint32_t, uint32_t) {}
static const wl_pointer_listener pointer_lst = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
    pointer_handle_frame,
    pointer_handle_axis_source,
    pointer_handle_axis_stop,
    pointer_handle_axis_discrete,
    pointer_handle_axis_value120,
    pointer_handle_axis_relative_direction,
};

// ---- touch ----------------------------------------------------------------
// Single-finger tap/drag mapped into the same pointer-button / pointer-move
// pathway. On touch-down we run the same hit-test hierarchy as a left click
// (close > resize edge > titlebar move > content click); on touch-motion we
// emit a move event with left_down=true.

static void touch_handle_down(void* data, wl_touch*, uint32_t serial,
                              uint32_t, wl_surface* surf, int32_t id,
                              wl_fixed_t sx, wl_fixed_t sy) {
    auto* st = static_cast<WlState*>(data);
    if (surf != st->surface) return;
    if (st->touch_id != -1) return;      // already tracking another finger
    st->touch_id = id;
    const double off = wl_effective_shadow(st);
    st->tx = wl_fixed_to_double(sx) - off;
    st->ty = wl_fixed_to_double(sy) - off;
    st->px = st->tx; st->py = st->ty;
    st->last_enter_serial = serial;

    if (st->use_csd && st->csd) {
        if (st->csd->InCloseButton((int)st->px, (int)st->py, st->width)) {
            st->running = false;
            return;
        }
        uint32_t edge = st->floating
            ? st->csd->ResizeEdge((int)st->px, (int)st->py, st->width, st->height)
            : 0;
        if (edge != 0 && st->toplevel) {
            xdg_toplevel_resize(st->toplevel, st->seat, serial, edge);
            st->touch_id = -1;
            return;
        }
        if (st->csd->InTitlebar((int)st->px, (int)st->py)) {
            xdg_toplevel_move(st->toplevel, st->seat, serial);
            st->touch_id = -1;
            return;
        }
    }
    st->left_down = true;
    float bx, by;
    if (wl_compute_buffer_mouse(st, st->px, st->py, &bx, &by) && st->mouseBtnCb)
        st->mouseBtnCb(bx, by, true, (st->mods & MOD_SHIFT) != 0);
}

static void touch_handle_up(void* data, wl_touch*, uint32_t, uint32_t, int32_t id) {
    auto* st = static_cast<WlState*>(data);
    if (id != st->touch_id) return;
    st->touch_id = -1;
    if (!st->left_down) return;
    st->left_down = false;
    float bx, by;
    if (wl_compute_buffer_mouse(st, st->px, st->py, &bx, &by) && st->mouseBtnCb)
        st->mouseBtnCb(bx, by, false, (st->mods & MOD_SHIFT) != 0);
}

static void touch_handle_motion(void* data, wl_touch*, uint32_t, int32_t id,
                                wl_fixed_t sx, wl_fixed_t sy) {
    auto* st = static_cast<WlState*>(data);
    if (id != st->touch_id) return;
    const double off = wl_effective_shadow(st);
    st->tx = wl_fixed_to_double(sx) - off;
    st->ty = wl_fixed_to_double(sy) - off;
    st->px = st->tx; st->py = st->ty;
    float bx, by;
    if (wl_compute_buffer_mouse(st, st->px, st->py, &bx, &by) && st->mouseMoveCb)
        st->mouseMoveCb(bx, by, st->left_down);
}

static void touch_handle_frame(void*, wl_touch*) {}
static void touch_handle_cancel(void* data, wl_touch*) {
    auto* st = static_cast<WlState*>(data);
    st->touch_id = -1;
    if (st->left_down) {
        st->left_down = false;
        float bx, by;
        if (wl_compute_buffer_mouse(st, st->px, st->py, &bx, &by) && st->mouseBtnCb)
            st->mouseBtnCb(bx, by, false, false);
    }
}
static void touch_handle_shape(void*, wl_touch*, int32_t, wl_fixed_t, wl_fixed_t) {}
static void touch_handle_orientation(void*, wl_touch*, int32_t, wl_fixed_t) {}

const wl_touch_listener touch_lst = {
    touch_handle_down,
    touch_handle_up,
    touch_handle_motion,
    touch_handle_frame,
    touch_handle_cancel,
    touch_handle_shape,
    touch_handle_orientation,
};

// ---- keyboard -------------------------------------------------------------

static void keyboard_handle_keymap(void* data, wl_keyboard*, uint32_t fmt, int32_t fd, uint32_t sz) {
    auto* st = static_cast<WlState*>(data);
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char* map = (char*)mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }
    if (!st->xkb_ctx) st->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (st->xkb_km) xkb_keymap_unref(st->xkb_km);
    st->xkb_km = xkb_keymap_new_from_string(st->xkb_ctx, map,
                                            XKB_KEYMAP_FORMAT_TEXT_V1,
                                            XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, sz); close(fd);
    if (st->xkb_st) xkb_state_unref(st->xkb_st);
    st->xkb_st = st->xkb_km ? xkb_state_new(st->xkb_km) : nullptr;
    if (st->xkb_km) {
        st->mod_ctrl  = xkb_keymap_mod_get_index(st->xkb_km, XKB_MOD_NAME_CTRL);
        st->mod_shift = xkb_keymap_mod_get_index(st->xkb_km, XKB_MOD_NAME_SHIFT);
        st->mod_alt   = xkb_keymap_mod_get_index(st->xkb_km, XKB_MOD_NAME_ALT);
        st->mod_super = xkb_keymap_mod_get_index(st->xkb_km, XKB_MOD_NAME_LOGO);
    }
}

// Translate a keysym into the "keycode" gritcode expects.
// We pass xkb keysyms directly because widgets.cpp / app.cpp compare against
// XKB_KEY_* constants already. Letters however are uppercased when Shift is
// held (xkb already does this via key-get-one-sym) — app.cpp checks things
// like `key == Key::A` where Key::A == 'A', so we return the uppercase form.
static int wl_translate_sym(xkb_keysym_t sym) {
    // Printable ASCII: xkb gives us the shifted form already.
    // 'a'..'z' → uppercase them so Key::A = 'A' compares work.
    if (sym >= 'a' && sym <= 'z') return (int)(sym - 32);
    // Most other keysyms pass through (0xff__ for special keys).
    return (int)sym;
}

static void keyboard_handle_enter(void* data, wl_keyboard*, uint32_t,
                                  wl_surface*, wl_array*) {
    static_cast<WlState*>(data)->keyboard_focus = true;
}
static void keyboard_handle_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
    auto* st = static_cast<WlState*>(data);
    st->keyboard_focus = false;
    st->repeat_key = 0;
}
static void keyboard_handle_key(void* data, wl_keyboard*, uint32_t serial,
                                uint32_t, uint32_t key, uint32_t state) {
    auto* st = static_cast<WlState*>(data);
    st->latest_serial = serial;
    if (!st->xkb_st) return;
    uint32_t k = key + 8;  // XKB offset
    xkb_keysym_t sym = xkb_state_key_get_one_sym(st->xkb_st, k);
    bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    int keycode = wl_translate_sym(sym);

    // Unicode codepoint (for text input).
    uint32_t cp = xkb_state_key_get_utf32(st->xkb_st, k);

    if (st->keyCb && keycode) st->keyCb(keycode, st->mods, pressed);

    // Only produce character input on press, and only when Ctrl/Super aren't
    // active (so Ctrl+A isn't interpreted as literal 'A').
    if (pressed && cp >= 0x20 && cp != 0x7f && !(st->mods & (MOD_CTRL | MOD_SUPER))) {
        if (st->charCb) st->charCb(cp);
    }

    // Key repeat tracking
    if (pressed && st->xkb_km &&
        xkb_keymap_key_repeats(st->xkb_km, k)) {
        st->repeat_key = k;
        st->repeat_sym = sym;
        st->repeat_cp = cp;
        st->repeat_next = wl_monotonic() + st->repeat_delay_ms / 1000.0;
    } else if (!pressed && st->repeat_key == k) {
        st->repeat_key = 0;
    }
}
static void keyboard_handle_modifiers(void* data, wl_keyboard*, uint32_t,
                                      uint32_t depressed, uint32_t latched,
                                      uint32_t locked, uint32_t group) {
    auto* st = static_cast<WlState*>(data);
    if (st->xkb_st) xkb_state_update_mask(st->xkb_st, depressed, latched, locked, 0, 0, group);
    st->mods = 0;
    if (!st->xkb_st) return;
    if (st->mod_ctrl != XKB_MOD_INVALID &&
        xkb_state_mod_index_is_active(st->xkb_st, st->mod_ctrl, XKB_STATE_MODS_EFFECTIVE) > 0)
        st->mods |= MOD_CTRL;
    if (st->mod_shift != XKB_MOD_INVALID &&
        xkb_state_mod_index_is_active(st->xkb_st, st->mod_shift, XKB_STATE_MODS_EFFECTIVE) > 0)
        st->mods |= MOD_SHIFT;
    if (st->mod_alt != XKB_MOD_INVALID &&
        xkb_state_mod_index_is_active(st->xkb_st, st->mod_alt, XKB_STATE_MODS_EFFECTIVE) > 0)
        st->mods |= MOD_ALT;
    if (st->mod_super != XKB_MOD_INVALID &&
        xkb_state_mod_index_is_active(st->xkb_st, st->mod_super, XKB_STATE_MODS_EFFECTIVE) > 0)
        st->mods |= MOD_SUPER;
}
static void keyboard_handle_repeat_info(void* data, wl_keyboard*, int32_t rate, int32_t delay) {
    auto* st = static_cast<WlState*>(data);
    if (rate > 0) st->repeat_rate = rate;
    if (delay >= 0) st->repeat_delay_ms = delay;
}
static const wl_keyboard_listener keyboard_lst = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    keyboard_handle_repeat_info,
};

// Forward decl so seat_handle_capabilities can register it.
extern const wl_touch_listener touch_lst;

static void seat_handle_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto* st = static_cast<WlState*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !st->pointer) {
        st->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(st->pointer, &pointer_lst, st);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && st->pointer) {
        wl_pointer_release(st->pointer); st->pointer = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !st->keyboard) {
        st->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(st->keyboard, &keyboard_lst, st);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && st->keyboard) {
        wl_keyboard_release(st->keyboard); st->keyboard = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !st->touch) {
        st->touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(st->touch, &touch_lst, st);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && st->touch) {
        wl_touch_release(st->touch); st->touch = nullptr;
    }
}
static void seat_handle_name(void*, wl_seat*, const char*) {}
static const wl_seat_listener seat_lst = { seat_handle_capabilities, seat_handle_name };

// ---- data device (clipboard) ---------------------------------------------

static void data_offer_handle_offer(void* data, wl_data_offer*, const char* mime) {
    auto* st = static_cast<WlState*>(data);
    st->pending_offer_mimes.push_back(mime);
}
static void data_offer_handle_source_actions(void*, wl_data_offer*, uint32_t) {}
static void data_offer_handle_action(void*, wl_data_offer*, uint32_t) {}
static const wl_data_offer_listener data_offer_lst = {
    data_offer_handle_offer,
    data_offer_handle_source_actions,
    data_offer_handle_action,
};

static void data_device_handle_data_offer(void* data, wl_data_device*, wl_data_offer* off) {
    auto* st = static_cast<WlState*>(data);
    if (st->pending_offer) {
        wl_data_offer_destroy(st->pending_offer);
    }
    st->pending_offer = off;
    st->pending_offer_mimes.clear();
    wl_data_offer_add_listener(off, &data_offer_lst, st);
}
static void data_device_handle_enter(void*, wl_data_device*, uint32_t, wl_surface*,
                                     wl_fixed_t, wl_fixed_t, wl_data_offer*) {}
static void data_device_handle_leave(void*, wl_data_device*) {}
static void data_device_handle_motion(void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
static void data_device_handle_drop(void*, wl_data_device*) {}
static void data_device_handle_selection(void* data, wl_data_device*, wl_data_offer* off) {
    auto* st = static_cast<WlState*>(data);
    if (st->selection_offer) {
        wl_data_offer_destroy(st->selection_offer);
        st->selection_offer = nullptr;
        st->selection_offer_mimes.clear();
    }
    if (off == st->pending_offer) {
        st->selection_offer = off;
        st->selection_offer_mimes = std::move(st->pending_offer_mimes);
        st->pending_offer = nullptr;
        st->pending_offer_mimes.clear();
    }
}
static const wl_data_device_listener data_device_lst = {
    data_device_handle_data_offer,
    data_device_handle_enter,
    data_device_handle_leave,
    data_device_handle_motion,
    data_device_handle_drop,
    data_device_handle_selection,
};

// Outgoing clipboard source.
static void data_source_handle_target(void*, wl_data_source*, const char*) {}
static void data_source_handle_send(void* data, wl_data_source* src, const char*, int32_t fd) {
    auto* st = static_cast<WlState*>(data);
    const std::string& s = st->owned_clipboard;
    // Blocking write is fine — clipboards are small and the compositor reads quickly.
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = write(fd, s.data() + off, s.size() - off);
        if (n < 0) { if (errno == EINTR) continue; break; }
        off += (size_t)n;
    }
    close(fd);
    (void)src;
}
static void data_source_handle_cancelled(void* data, wl_data_source* src) {
    auto* st = static_cast<WlState*>(data);
    if (st->active_source == src) st->active_source = nullptr;
    wl_data_source_destroy(src);
}
static void data_source_handle_dnd_drop_performed(void*, wl_data_source*) {}
static void data_source_handle_dnd_finished(void*, wl_data_source*) {}
static void data_source_handle_action(void*, wl_data_source*, uint32_t) {}
static const wl_data_source_listener data_source_lst = {
    data_source_handle_target,
    data_source_handle_send,
    data_source_handle_cancelled,
    data_source_handle_dnd_drop_performed,
    data_source_handle_dnd_finished,
    data_source_handle_action,
};

// ---- registry -------------------------------------------------------------

static void registry_handle_global(void* data, wl_registry* reg, uint32_t id,
                                   const char* iface, uint32_t ver) {
    auto* st = static_cast<WlState*>(data);
    if (!strcmp(iface, wl_compositor_interface.name))
        st->compositor = (wl_compositor*)wl_registry_bind(reg, id, &wl_compositor_interface,
                                                          ver < 6 ? ver : 6);
    else if (!strcmp(iface, xdg_wm_base_interface.name))
        st->wm_base = (xdg_wm_base*)wl_registry_bind(reg, id, &xdg_wm_base_interface,
                                                     ver < 5 ? ver : 5);
    else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
        st->deco_mgr = (zxdg_decoration_manager_v1*)wl_registry_bind(reg, id,
                                                                     &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name))
        st->seat = (wl_seat*)wl_registry_bind(reg, id, &wl_seat_interface,
                                              ver < 7 ? ver : 7);
    else if (!strcmp(iface, wl_shm_interface.name))
        st->shm = (wl_shm*)wl_registry_bind(reg, id, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_data_device_manager_interface.name))
        st->data_device_mgr = (wl_data_device_manager*)wl_registry_bind(reg, id,
                                                                        &wl_data_device_manager_interface, 3);
}
static void registry_handle_global_remove(void*, wl_registry*, uint32_t) {}
static const wl_registry_listener registry_lst = {
    registry_handle_global,
    registry_handle_global_remove,
};

// ---- EGL & frame commit ---------------------------------------------------

static bool wl_init_egl(WlState* st) {
    st->egl_display = eglGetDisplay((EGLNativeDisplayType)st->display);
    if (st->egl_display == EGL_NO_DISPLAY) return false;
    EGLint maj, min;
    if (!eglInitialize(st->egl_display, &maj, &min)) return false;

    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint n = 0;
    if (!eglChooseConfig(st->egl_display, cfg_attrs, &st->egl_config, 1, &n) || n < 1)
        return false;

    eglBindAPI(EGL_OPENGL_API);
    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    st->egl_context = eglCreateContext(st->egl_display, st->egl_config,
                                       EGL_NO_CONTEXT, ctx_attrs);
    if (st->egl_context == EGL_NO_CONTEXT) return false;

    const int sh = wl_effective_shadow(st);
    const int buf_w = (st->width  + 2 * sh) * st->scale;
    const int buf_h = (st->height + 2 * sh) * st->scale;
    st->egl_window = wl_egl_window_create(st->surface, buf_w, buf_h);
    st->egl_surface = eglCreateWindowSurface(st->egl_display, st->egl_config,
                                             (EGLNativeWindowType)st->egl_window, nullptr);
    if (st->egl_surface == EGL_NO_SURFACE) return false;

    eglMakeCurrent(st->egl_display, st->egl_surface, st->egl_surface, st->egl_context);
    eglSwapInterval(st->egl_display, 0);  // compositor-paced via frame callbacks
    st->applied_buffer_w = buf_w;
    st->applied_buffer_h = buf_h;
    return true;
}

static void frame_callback_done(void* data, wl_callback* cb, uint32_t) {
    wl_callback_destroy(cb);
    static_cast<WlState*>(data)->frame_pending = false;
}
static const wl_callback_listener frame_cb_lst = { frame_callback_done };

// ---- BeginFrame / EndFrame --------------------------------------------------

// Apply any pending compositor-driven resize. Called from xdg_surface.configure
// (so drag-resize wakes the render loop) and as a fallback from BeginFrame
// (covers the initial configure that arrives before EGL is set up).
static void wl_apply_pending_resize(WlState* st) {
    if (st->pending_width > 0 && st->pending_height > 0 &&
        (st->pending_width != st->width || st->pending_height != st->height)) {
        st->width = st->pending_width;
        st->height = st->pending_height;
    }
    st->pending_width = 0;
    st->pending_height = 0;

    if (!st->egl_window) return;  // pre-EGL; first real frame will catch up

    const int sh = wl_effective_shadow(st);
    const int want_w = (st->width  + 2 * sh) * st->scale;
    const int want_h = (st->height + 2 * sh) * st->scale;
    if (want_w == st->applied_buffer_w && want_h == st->applied_buffer_h) return;

    wl_egl_window_resize(st->egl_window, want_w, want_h, 0, 0);
    st->applied_buffer_w = want_w;
    st->applied_buffer_h = want_h;
    wl_sync_geometry(st);
    int title_h = st->use_csd ? CsdCompositor::TITLEBAR_H : 0;
    if (st->resizeCb)
        st->resizeCb(st->width * st->scale,
                     (st->height - title_h) * st->scale,
                     (float)st->scale);
}

// BeginContentFrame binds the CSD content FBO (if CSD) or the swapchain
// framebuffer, and returns its dimensions.
static int wl_begin_content(WlState* st, int* outW, int* outH,
                            int* outLogicalW, int* outLogicalH, int* outOffsetY) {
    wl_apply_pending_resize(st);

    int title_h = st->use_csd ? CsdCompositor::TITLEBAR_H : 0;
    int contentPixelH = (st->height - title_h) * st->scale;
    int contentPixelW = st->width * st->scale;
    if (st->use_csd && st->csd) {
        auto f = st->csd->BeginFrame(st->width, st->height, st->scale);
        *outW = f.width;
        *outH = f.height;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, contentPixelW, contentPixelH);
        *outW = contentPixelW;
        *outH = contentPixelH;
    }
    *outLogicalW = st->width;
    *outLogicalH = st->height - title_h;
    *outOffsetY = title_h;
    return 1;
}

static void wl_end_content(WlState* st) {
    if (st->use_csd && st->csd) {
        st->csd->EndFrame(st->width, st->height, st->scale, st->floating);
    }
    wl_callback* cb = wl_surface_frame(st->surface);
    wl_callback_add_listener(cb, &frame_cb_lst, st);
    st->frame_pending = true;
    eglSwapBuffers(st->egl_display, st->egl_surface);
    st->dirty = false;
}

#endif  // HAVE_WAYLAND

// ============================================================================
// X11 backend
// ============================================================================
#ifdef HAVE_X11

struct X11State {
    Display* display = nullptr;
    ::Window window = 0;
    int screen = 0;
    Atom wmProtocols = 0, wmDelete = 0, wmNetSyncReq = 0, wmNetSyncCounter = 0;
    Atom atomClipboard = 0, atomUtf8 = 0, atomTargets = 0, atomPrimary = 0;

    // XSync (smooth resize)
    bool haveSync = false;
    XSyncCounter basicCounter = 0, extendedCounter = 0;
    XSyncValue currentSyncValue = {};

    // EGL
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLConfig  egl_config = nullptr;

    // Input method (UTF-8 input)
    XIM xim = nullptr;
    XIC xic = nullptr;

    int width = 1000, height = 750;
    float scale = 1.0f;
    bool running = true;
    bool shouldClose = false;
    bool keyboard_focus = true;
    bool dirty = true;

    double px = 0, py = 0;
    bool left_down = false;
    int mods = 0;

    std::string owned_clipboard;  // we own selection until another client claims

    AppWindow::ResizeCb    resizeCb;
    AppWindow::MouseBtnCb  mouseBtnCb;
    AppWindow::MouseMoveCb mouseMoveCb;
    AppWindow::ScrollCb    scrollCb;
    AppWindow::KeyCb       keyCb;
    AppWindow::CharCb      charCb;
};

static int x11_translate_keysym(KeySym sym) {
    if (sym >= 'a' && sym <= 'z') return (int)(sym - 32);
    return (int)sym;
}

static int x11_mods_from_state(unsigned int state) {
    int m = 0;
    if (state & ControlMask) m |= MOD_CTRL;
    if (state & ShiftMask)   m |= MOD_SHIFT;
    if (state & Mod1Mask)    m |= MOD_ALT;
    if (state & Mod4Mask)    m |= MOD_SUPER;
    return m;
}

static bool x11_init_egl(X11State* st) {
    st->egl_display = eglGetDisplay((EGLNativeDisplayType)st->display);
    if (st->egl_display == EGL_NO_DISPLAY) return false;
    EGLint maj, min;
    if (!eglInitialize(st->egl_display, &maj, &min)) return false;

    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint n = 0;
    if (!eglChooseConfig(st->egl_display, cfg_attrs, &st->egl_config, 1, &n) || n < 1)
        return false;
    eglBindAPI(EGL_OPENGL_API);
    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    st->egl_context = eglCreateContext(st->egl_display, st->egl_config,
                                       EGL_NO_CONTEXT, ctx_attrs);
    if (st->egl_context == EGL_NO_CONTEXT) return false;
    return true;
}

static void x11_respond_selection_request(X11State* st, XSelectionRequestEvent& req) {
    XSelectionEvent ev = {};
    ev.type = SelectionNotify;
    ev.display = req.display;
    ev.requestor = req.requestor;
    ev.selection = req.selection;
    ev.target = req.target;
    ev.time = req.time;
    ev.property = None;

    if (req.target == st->atomTargets) {
        Atom list[] = { st->atomTargets, st->atomUtf8, XA_STRING };
        XChangeProperty(st->display, req.requestor, req.property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char*)list, 3);
        ev.property = req.property;
    } else if (req.target == st->atomUtf8 || req.target == XA_STRING) {
        const std::string& s = st->owned_clipboard;
        XChangeProperty(st->display, req.requestor, req.property, req.target, 8,
                        PropModeReplace, (unsigned char*)s.data(), (int)s.size());
        ev.property = req.property;
    }
    XSendEvent(st->display, req.requestor, 0, 0, (XEvent*)&ev);
    XFlush(st->display);
}

static std::string x11_read_clipboard(X11State* st) {
    ::Window owner = XGetSelectionOwner(st->display, st->atomClipboard);
    if (owner == None) return "";
    if (owner == st->window) return st->owned_clipboard;

    Atom prop = XInternAtom(st->display, "GRIT_CLIPBOARD_IN", False);
    XConvertSelection(st->display, st->atomClipboard, st->atomUtf8,
                      prop, st->window, CurrentTime);
    XFlush(st->display);

    // Pump events until SelectionNotify (up to ~1s)
    double deadline = 0;
    {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline = ts.tv_sec + ts.tv_nsec / 1e9 + 1.0;
    }
    for (;;) {
        while (XPending(st->display)) {
            XEvent e;
            XNextEvent(st->display, &e);
            if (e.type == SelectionNotify && e.xselection.property != None) {
                Atom type; int fmt; unsigned long nitems, bytesAfter;
                unsigned char* data = nullptr;
                XGetWindowProperty(st->display, st->window, prop, 0, ~0L, True,
                                   AnyPropertyType, &type, &fmt, &nitems, &bytesAfter, &data);
                std::string out;
                if (data && nitems > 0) {
                    out.assign((char*)data, nitems);
                    XFree(data);
                }
                return out;
            } else if (e.type == SelectionRequest) {
                x11_respond_selection_request(st, e.xselectionrequest);
            }
        }
        struct pollfd pfd{ ConnectionNumber(st->display), POLLIN, 0 };
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec / 1e9;
        if (now > deadline) return "";
        int ms = (int)((deadline - now) * 1000);
        if (ms <= 0) return "";
        poll(&pfd, 1, ms);
    }
}

static void x11_pump_event(X11State* st, XEvent& e) {
    if (XFilterEvent(&e, None)) return;
    switch (e.type) {
    case ConfigureNotify: {
        if (e.xconfigure.width != st->width || e.xconfigure.height != st->height) {
            st->width = e.xconfigure.width;
            st->height = e.xconfigure.height;
            if (st->resizeCb) st->resizeCb(st->width, st->height, st->scale);
            st->dirty = true;
        }
        break;
    }
    case Expose: st->dirty = true; break;
    case FocusIn: st->keyboard_focus = true; if (st->xic) XSetICFocus(st->xic); break;
    case FocusOut: st->keyboard_focus = false; if (st->xic) XUnsetICFocus(st->xic); break;
    case ClientMessage: {
        if ((Atom)e.xclient.data.l[0] == st->wmDelete) {
            st->shouldClose = true;
        } else if ((Atom)e.xclient.data.l[0] == st->wmNetSyncReq && st->haveSync) {
            XSyncValue v;
            XSyncIntsToValue(&v, e.xclient.data.l[2], e.xclient.data.l[3]);
            st->currentSyncValue = v;
        }
        break;
    }
    case ButtonPress: {
        st->mods = x11_mods_from_state(e.xbutton.state);
        if (e.xbutton.button == Button1) {
            st->left_down = true;
            st->px = e.xbutton.x; st->py = e.xbutton.y;
            if (st->mouseBtnCb) st->mouseBtnCb((float)st->px, (float)st->py, true,
                                               (st->mods & MOD_SHIFT) != 0);
        } else if (e.xbutton.button == Button4 && st->scrollCb) {
            st->scrollCb(1.0f);
        } else if (e.xbutton.button == Button5 && st->scrollCb) {
            st->scrollCb(-1.0f);
        }
        break;
    }
    case ButtonRelease:
        if (e.xbutton.button == Button1) {
            st->left_down = false;
            st->px = e.xbutton.x; st->py = e.xbutton.y;
            if (st->mouseBtnCb) st->mouseBtnCb((float)st->px, (float)st->py, false, false);
        }
        break;
    case MotionNotify:
        st->px = e.xmotion.x; st->py = e.xmotion.y;
        if (st->mouseMoveCb) st->mouseMoveCb((float)st->px, (float)st->py, st->left_down);
        break;
    case KeyPress: {
        st->mods = x11_mods_from_state(e.xkey.state);
        KeySym sym = XLookupKeysym(&e.xkey, 0);
        // Shifted keysym for proper 'A' vs 'a' comparisons
        if (e.xkey.state & ShiftMask) sym = XLookupKeysym(&e.xkey, 1);
        int keycode = x11_translate_keysym(sym);
        if (keycode && st->keyCb) st->keyCb(keycode, st->mods, true);

        // Character input via Xutf8LookupString (handles compose, IME).
        if (st->charCb && !(st->mods & (MOD_CTRL | MOD_SUPER))) {
            char buf[64];
            Status status = 0;
            KeySym ks = 0;
            int len = st->xic
                ? Xutf8LookupString(st->xic, &e.xkey, buf, sizeof(buf) - 1, &ks, &status)
                : XLookupString(&e.xkey, buf, sizeof(buf) - 1, &ks, nullptr);
            if (len > 0) {
                buf[len] = 0;
                // Decode UTF-8 → codepoints
                for (int i = 0; i < len; ) {
                    unsigned char c0 = (unsigned char)buf[i];
                    uint32_t cp = 0;
                    int n = 1;
                    if ((c0 & 0x80) == 0)       { cp = c0; n = 1; }
                    else if ((c0 & 0xE0) == 0xC0 && i+1 < len) { cp = (c0 & 0x1F) << 6 | (buf[i+1] & 0x3F); n = 2; }
                    else if ((c0 & 0xF0) == 0xE0 && i+2 < len) { cp = (c0 & 0x0F) << 12 | (buf[i+1] & 0x3F) << 6 | (buf[i+2] & 0x3F); n = 3; }
                    else if ((c0 & 0xF8) == 0xF0 && i+3 < len) { cp = (c0 & 0x07) << 18 | (buf[i+1] & 0x3F) << 12 | (buf[i+2] & 0x3F) << 6 | (buf[i+3] & 0x3F); n = 4; }
                    else break;
                    if (cp >= 0x20 && cp != 0x7F) st->charCb(cp);
                    i += n;
                }
            }
        }
        break;
    }
    case KeyRelease: {
        st->mods = x11_mods_from_state(e.xkey.state);
        KeySym sym = XLookupKeysym(&e.xkey, 0);
        if (e.xkey.state & ShiftMask) sym = XLookupKeysym(&e.xkey, 1);
        int keycode = x11_translate_keysym(sym);
        if (keycode && st->keyCb) st->keyCb(keycode, st->mods, false);
        break;
    }
    case SelectionRequest:
        x11_respond_selection_request(st, e.xselectionrequest);
        break;
    case SelectionClear:
        if (e.xselectionclear.selection == st->atomClipboard) st->owned_clipboard.clear();
        break;
    }
}

static void x11_drain(X11State* st) {
    while (XPending(st->display)) {
        XEvent e;
        XNextEvent(st->display, &e);
        x11_pump_event(st, e);
    }
}

#endif  // HAVE_X11

// ============================================================================
// AppWindow (Pimpl)
// ============================================================================

struct AppWindow::Impl {
#ifdef HAVE_WAYLAND
    WlState* wl = nullptr;
#endif
#ifdef HAVE_X11
    X11State* x11 = nullptr;
#endif
};

AppWindow::AppWindow() : impl_(new Impl()) {}

AppWindow::~AppWindow() {
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        auto* st = impl_->wl;
        if (st->csd) { st->csd->Shutdown(); delete st->csd; }
        if (st->active_source) wl_data_source_destroy(st->active_source);
        if (st->selection_offer) wl_data_offer_destroy(st->selection_offer);
        if (st->pending_offer) wl_data_offer_destroy(st->pending_offer);
        if (st->data_device) wl_data_device_release(st->data_device);
        if (st->touch) wl_touch_release(st->touch);
        if (st->cursor_surface) wl_surface_destroy(st->cursor_surface);
        if (st->cursor_theme) wl_cursor_theme_destroy(st->cursor_theme);
        if (st->egl_surface != EGL_NO_SURFACE) {
            eglMakeCurrent(st->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(st->egl_display, st->egl_surface);
        }
        if (st->egl_context != EGL_NO_CONTEXT) eglDestroyContext(st->egl_display, st->egl_context);
        if (st->egl_window) wl_egl_window_destroy(st->egl_window);
        if (st->egl_display != EGL_NO_DISPLAY) eglTerminate(st->egl_display);
        if (st->deco) zxdg_toplevel_decoration_v1_destroy(st->deco);
        if (st->toplevel) xdg_toplevel_destroy(st->toplevel);
        if (st->xsurface) xdg_surface_destroy(st->xsurface);
        if (st->surface) wl_surface_destroy(st->surface);
        if (st->xkb_st) xkb_state_unref(st->xkb_st);
        if (st->xkb_km) xkb_keymap_unref(st->xkb_km);
        if (st->xkb_ctx) xkb_context_unref(st->xkb_ctx);
        if (st->display) wl_display_disconnect(st->display);
        delete st;
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) {
        auto* st = impl_->x11;
        if (st->xic) XDestroyIC(st->xic);
        if (st->xim) XCloseIM(st->xim);
        if (st->egl_surface != EGL_NO_SURFACE) {
            eglMakeCurrent(st->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(st->egl_display, st->egl_surface);
        }
        if (st->egl_context != EGL_NO_CONTEXT) eglDestroyContext(st->egl_display, st->egl_context);
        if (st->egl_display != EGL_NO_DISPLAY) eglTerminate(st->egl_display);
        if (st->window) XDestroyWindow(st->display, st->window);
        if (st->display) XCloseDisplay(st->display);
        delete st;
    }
#endif
    delete impl_;
}

bool AppWindow::Init(int width, int height, const char* title) {
#ifdef HAVE_WAYLAND
    if (const char* env = getenv("WAYLAND_DISPLAY"); env && *env) {
        auto* st = new WlState();
        st->width = width; st->height = height;
        st->display = wl_display_connect(nullptr);
        if (!st->display) { delete st; goto try_x11; }

        wl_registry* reg = wl_display_get_registry(st->display);
        wl_registry_add_listener(reg, &registry_lst, st);
        wl_display_roundtrip(st->display);

        if (!st->compositor || !st->wm_base || !st->seat) {
            delete st; goto try_x11;
        }

        xdg_wm_base_add_listener(st->wm_base, &wm_base_lst, st);
        if (st->seat) wl_seat_add_listener(st->seat, &seat_lst, st);

        st->surface = wl_compositor_create_surface(st->compositor);
        wl_surface_add_listener(st->surface, &surface_lst, st);
        wl_surface_set_buffer_scale(st->surface, st->scale);

        st->xsurface = xdg_wm_base_get_xdg_surface(st->wm_base, st->surface);
        xdg_surface_add_listener(st->xsurface, &xdg_surface_lst, st);
        st->toplevel = xdg_surface_get_toplevel(st->xsurface);
        xdg_toplevel_add_listener(st->toplevel, &toplevel_lst, st);
        xdg_toplevel_set_title(st->toplevel, title ? title : "Gritcode");
        xdg_toplevel_set_app_id(st->toplevel, "grit");
        xdg_toplevel_set_min_size(st->toplevel, 480, 360);

        // Request server-side decorations. If the compositor doesn't implement
        // the protocol at all, we assume CSD required (GNOME's stance).
        if (st->deco_mgr) {
            st->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(
                st->deco_mgr, st->toplevel);
            zxdg_toplevel_decoration_v1_add_listener(st->deco, &deco_lst, st);
            zxdg_toplevel_decoration_v1_set_mode(st->deco,
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        } else {
            st->use_csd = true;  // No protocol → assume we draw our own chrome
        }

        wl_surface_commit(st->surface);
        wl_display_roundtrip(st->display);  // get initial configure + deco reply

        if (!wl_init_egl(st)) { delete st; goto try_x11; }

        if (st->use_csd) {
            st->csd = new CsdCompositor();
            if (!st->csd->Init()) {
                fprintf(stderr, "[grit] CSD shader compile failed; running without decorations\n");
                delete st->csd; st->csd = nullptr; st->use_csd = false;
            }
        }

        // Cursor theme
        const char* size_s = getenv("XCURSOR_SIZE");
        int cursor_size = size_s ? atoi(size_s) : 24;
        if (cursor_size <= 0) cursor_size = 24;
        const char* theme = getenv("XCURSOR_THEME");
        if (st->shm) {
            st->cursor_theme = wl_cursor_theme_load(theme, cursor_size * st->scale, st->shm);
            if (st->compositor) st->cursor_surface = wl_compositor_create_surface(st->compositor);
        }

        // Clipboard data device
        if (st->data_device_mgr && st->seat) {
            st->data_device = wl_data_device_manager_get_data_device(st->data_device_mgr, st->seat);
            wl_data_device_add_listener(st->data_device, &data_device_lst, st);
        }

        wl_sync_geometry(st);
        impl_->wl = st;
        return true;
    }
try_x11:
#endif

#ifdef HAVE_X11
    {
        auto* st = new X11State();
        st->width = width; st->height = height;

        st->display = XOpenDisplay(nullptr);
        if (!st->display) { delete st; return false; }
        st->screen = DefaultScreen(st->display);

        if (!x11_init_egl(st)) { XCloseDisplay(st->display); delete st; return false; }

        EGLint vid = 0;
        eglGetConfigAttrib(st->egl_display, st->egl_config, EGL_NATIVE_VISUAL_ID, &vid);
        XVisualInfo viTmpl = {};
        viTmpl.visualid = (VisualID)vid;
        int nv = 0;
        XVisualInfo* vi = XGetVisualInfo(st->display, VisualIDMask, &viTmpl, &nv);
        Visual* visual = vi ? vi->visual : DefaultVisual(st->display, st->screen);
        int depth = vi ? vi->depth : DefaultDepth(st->display, st->screen);
        Colormap cmap = XCreateColormap(st->display, RootWindow(st->display, st->screen), visual, AllocNone);

        XSetWindowAttributes swa = {};
        swa.colormap = cmap;
        swa.background_pixel = 0;
        swa.border_pixel = 0;
        swa.event_mask = StructureNotifyMask | ExposureMask |
                         ButtonPressMask | ButtonReleaseMask |
                         PointerMotionMask |
                         KeyPressMask | KeyReleaseMask |
                         FocusChangeMask;
        st->window = XCreateWindow(st->display, RootWindow(st->display, st->screen),
                                   0, 0, width, height, 0, depth, InputOutput, visual,
                                   CWColormap | CWEventMask | CWBackPixel | CWBorderPixel,
                                   &swa);
        if (vi) XFree(vi);

        XStoreName(st->display, st->window, title ? title : "Gritcode");
        XClassHint classHint; classHint.res_name = (char*)"grit"; classHint.res_class = (char*)"grit";
        XSetClassHint(st->display, st->window, &classHint);
        XSizeHints hints = {};
        hints.flags = PMinSize;
        hints.min_width = 480; hints.min_height = 360;
        XSetWMNormalHints(st->display, st->window, &hints);

        st->wmProtocols = XInternAtom(st->display, "WM_PROTOCOLS", False);
        st->wmDelete = XInternAtom(st->display, "WM_DELETE_WINDOW", False);
        st->wmNetSyncReq = XInternAtom(st->display, "_NET_WM_SYNC_REQUEST", False);
        st->wmNetSyncCounter = XInternAtom(st->display, "_NET_WM_SYNC_REQUEST_COUNTER", False);
        st->atomClipboard = XInternAtom(st->display, "CLIPBOARD", False);
        st->atomUtf8 = XInternAtom(st->display, "UTF8_STRING", False);
        st->atomTargets = XInternAtom(st->display, "TARGETS", False);
        st->atomPrimary = XA_PRIMARY;

        int syncMajor = 0, syncMinor = 0;
        if (XSyncQueryExtension(st->display, &syncMajor, &syncMinor) &&
            XSyncInitialize(st->display, &syncMajor, &syncMinor)) {
            XSyncValue zero; XSyncIntToValue(&zero, 0);
            st->basicCounter    = XSyncCreateCounter(st->display, zero);
            st->extendedCounter = XSyncCreateCounter(st->display, zero);
            XID counters[2] = { st->basicCounter, st->extendedCounter };
            XChangeProperty(st->display, st->window, st->wmNetSyncCounter,
                            XA_CARDINAL, 32, PropModeReplace,
                            (unsigned char*)counters, 2);
            Atom protocols[] = { st->wmDelete, st->wmNetSyncReq };
            XSetWMProtocols(st->display, st->window, protocols, 2);
            st->haveSync = true;
        } else {
            XSetWMProtocols(st->display, st->window, &st->wmDelete, 1);
        }

        // Input method (text entry)
        XSetLocaleModifiers("");
        st->xim = XOpenIM(st->display, nullptr, nullptr, nullptr);
        if (st->xim) {
            st->xic = XCreateIC(st->xim,
                                XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                                XNClientWindow, st->window,
                                XNFocusWindow, st->window,
                                NULL);
        }

        st->egl_surface = eglCreateWindowSurface(st->egl_display, st->egl_config,
                                                 (EGLNativeWindowType)st->window, nullptr);
        if (st->egl_surface == EGL_NO_SURFACE) {
            fprintf(stderr, "[grit] eglCreateWindowSurface failed on X11\n");
            XDestroyWindow(st->display, st->window); XCloseDisplay(st->display); delete st; return false;
        }
        eglMakeCurrent(st->egl_display, st->egl_surface, st->egl_surface, st->egl_context);
        eglSwapInterval(st->egl_display, 1);

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
        wl_display_flush(impl_->wl->display);
        // Pump a few roundtrips so the first frame dispatch configure+scale.
        wl_display_roundtrip(impl_->wl->display);
        return;
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) {
        XMapWindow(impl_->x11->display, impl_->x11->window);
        XFlush(impl_->x11->display);
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
    if (impl_->x11) return impl_->x11->shouldClose;
#endif
    return true;
}

void AppWindow::SetShouldClose(bool close) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->running = !close;
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->shouldClose = close;
#endif
}

void AppWindow::PollEvents() {
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        wl_display_dispatch_pending(impl_->wl->display);
        // Non-blocking read from the wire
        while (wl_display_prepare_read(impl_->wl->display) != 0) {
            wl_display_dispatch_pending(impl_->wl->display);
        }
        wl_display_flush(impl_->wl->display);
        struct pollfd pfd{ wl_display_get_fd(impl_->wl->display), POLLIN, 0 };
        int ret = poll(&pfd, 1, 0);
        if (ret > 0 && (pfd.revents & POLLIN)) wl_display_read_events(impl_->wl->display);
        else wl_display_cancel_read(impl_->wl->display);
        wl_display_dispatch_pending(impl_->wl->display);
        return;
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) x11_drain(impl_->x11);
#endif
}

void AppWindow::WaitEvents() {
    // Cap at 500ms so background-thread pushes to the event queue (MCP, HTTP
    // callbacks) get picked up in reasonable time even when no OS events are
    // arriving. Matches what our previous GLFW layer did.
    WaitEventsTimeout(0.5);
}

void AppWindow::WaitEventsTimeout(double timeout) {
    int ms = (int)(timeout * 1000);
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        auto* st = impl_->wl;
        wl_display_dispatch_pending(st->display);
        while (wl_display_prepare_read(st->display) != 0) {
            wl_display_dispatch_pending(st->display);
        }
        wl_display_flush(st->display);

        // Account for pending key repeats — wake sooner if one's due.
        if (st->repeat_key) {
            double now = wl_monotonic();
            double wait = (st->repeat_next - now) * 1000.0;
            if (wait < 0) wait = 0;
            if (wait < ms) ms = (int)wait;
        }

        struct pollfd pfd{ wl_display_get_fd(st->display), POLLIN, 0 };
        int ret = poll(&pfd, 1, ms);
        if (ret > 0 && (pfd.revents & POLLIN)) wl_display_read_events(st->display);
        else wl_display_cancel_read(st->display);
        wl_display_dispatch_pending(st->display);

        // Fire any overdue key repeats.
        if (st->repeat_key && st->keyboard_focus) {
            double now = wl_monotonic();
            while (st->repeat_key && st->repeat_next <= now) {
                int kc = wl_translate_sym(st->repeat_sym);
                if (kc && st->keyCb) st->keyCb(kc, st->mods, true);
                if (st->repeat_cp >= 0x20 && st->repeat_cp != 0x7f &&
                    !(st->mods & (MOD_CTRL | MOD_SUPER)) && st->charCb)
                    st->charCb(st->repeat_cp);
                st->repeat_next += 1.0 / st->repeat_rate;
            }
        }
        return;
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) {
        x11_drain(impl_->x11);
        struct pollfd pfd{ ConnectionNumber(impl_->x11->display), POLLIN, 0 };
        poll(&pfd, 1, ms);
        x11_drain(impl_->x11);
    }
#endif
}

AppWindow::ContentFrame AppWindow::BeginContentFrame() {
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        int w, h, lw, lh, oy;
        wl_begin_content(impl_->wl, &w, &h, &lw, &lh, &oy);
        return {w, h, lw, lh, oy};
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, impl_->x11->width, impl_->x11->height);
        return {impl_->x11->width, impl_->x11->height,
                impl_->x11->width, impl_->x11->height, 0};
    }
#endif
    return {0, 0, 0, 0, 0};
}

void AppWindow::EndContentFrame() {
#ifdef HAVE_WAYLAND
    if (impl_->wl) { wl_end_content(impl_->wl); return; }
#endif
#ifdef HAVE_X11
    if (impl_->x11) {
        eglSwapBuffers(impl_->x11->egl_display, impl_->x11->egl_surface);
        impl_->x11->dirty = false;
    }
#endif
}

void AppWindow::SwapBuffers() { EndContentFrame(); }

int AppWindow::Width() const {
#ifdef HAVE_WAYLAND
    if (impl_->wl) return impl_->wl->width * impl_->wl->scale;
#endif
#ifdef HAVE_X11
    if (impl_->x11) return impl_->x11->width;
#endif
    return 0;
}
int AppWindow::Height() const {
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        int t = impl_->wl->use_csd ? CsdCompositor::TITLEBAR_H : 0;
        return (impl_->wl->height - t) * impl_->wl->scale;
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) return impl_->x11->height;
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
    if (impl_->wl) {
        int t = impl_->wl->use_csd ? CsdCompositor::TITLEBAR_H : 0;
        return impl_->wl->height - t;
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) return impl_->x11->height;
#endif
    return 0;
}
float AppWindow::Scale() const {
#ifdef HAVE_WAYLAND
    if (impl_->wl) return (float)impl_->wl->scale;
#endif
#ifdef HAVE_X11
    if (impl_->x11) return impl_->x11->scale;
#endif
    return 1.0f;
}

void AppWindow::OnResize(ResizeCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->resizeCb = std::move(cb);
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->resizeCb = std::move(cb);
#endif
}
void AppWindow::OnMouseButton(MouseBtnCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->mouseBtnCb = std::move(cb);
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->mouseBtnCb = std::move(cb);
#endif
}
void AppWindow::OnMouseMove(MouseMoveCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->mouseMoveCb = std::move(cb);
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->mouseMoveCb = std::move(cb);
#endif
}
void AppWindow::OnScrollEvent(ScrollCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->scrollCb = std::move(cb);
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->scrollCb = std::move(cb);
#endif
}
void AppWindow::OnKeyEvent(KeyCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->keyCb = std::move(cb);
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->keyCb = std::move(cb);
#endif
}
void AppWindow::OnCharEvent(CharCb cb) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) impl_->wl->charCb = std::move(cb);
#endif
#ifdef HAVE_X11
    if (impl_->x11) impl_->x11->charCb = std::move(cb);
#endif
}

void AppWindow::SetFontManager(const FontManager* fm) {
#ifdef HAVE_WAYLAND
    if (impl_->wl && impl_->wl->csd) {
        impl_->wl->csd->SetFontManager(fm);
        impl_->wl->dirty = true;
    }
#else
    (void)fm;
#endif
}

void AppWindow::SetClipboard(const std::string& text) {
#ifdef HAVE_WAYLAND
    if (impl_->wl) {
        auto* st = impl_->wl;
        st->owned_clipboard = text;
        if (!st->data_device_mgr || !st->data_device) return;
        if (st->active_source) {
            wl_data_source_destroy(st->active_source);
            st->active_source = nullptr;
        }
        st->active_source = wl_data_device_manager_create_data_source(st->data_device_mgr);
        wl_data_source_add_listener(st->active_source, &data_source_lst, st);
        wl_data_source_offer(st->active_source, "text/plain;charset=utf-8");
        wl_data_source_offer(st->active_source, "text/plain");
        wl_data_source_offer(st->active_source, "UTF8_STRING");
        wl_data_device_set_selection(st->data_device, st->active_source, st->latest_serial);
        wl_display_flush(st->display);
        return;
    }
#endif
#ifdef HAVE_X11
    if (impl_->x11) {
        auto* st = impl_->x11;
        st->owned_clipboard = text;
        XSetSelectionOwner(st->display, st->atomClipboard, st->window, CurrentTime);
        XFlush(st->display);
    }
#endif
}

#ifdef HAVE_WAYLAND
static std::string wl_read_from_offer(WlState* st) {
    if (!st->selection_offer) return "";
    const char* mime = nullptr;
    static const char* const prefs[] = {
        "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING", nullptr
    };
    for (int i = 0; prefs[i]; ++i) {
        for (auto& m : st->selection_offer_mimes) {
            if (m == prefs[i]) { mime = prefs[i]; break; }
        }
        if (mime) break;
    }
    if (!mime && !st->selection_offer_mimes.empty()) mime = st->selection_offer_mimes.front().c_str();
    if (!mime) return "";

    int fds[2];
    if (pipe(fds) < 0) return "";
    wl_data_offer_receive(st->selection_offer, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(st->display);

    std::string out;
    char buf[4096];
    for (;;) {
        // Pump wayland so the sender's 'send' event dispatches.
        wl_display_flush(st->display);
        struct pollfd pfds[2] = {
            { fds[0], POLLIN, 0 },
            { wl_display_get_fd(st->display), POLLIN, 0 }
        };
        int ret = poll(pfds, 2, 1000);
        if (ret <= 0) break;
        if (pfds[1].revents & POLLIN) {
            wl_display_dispatch(st->display);
        }
        if (pfds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(fds[0], buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, buf + n);
        }
    }
    close(fds[0]);
    return out;
}
#endif

std::string AppWindow::GetClipboard() {
#ifdef HAVE_WAYLAND
    if (impl_->wl) return wl_read_from_offer(impl_->wl);
#endif
#ifdef HAVE_X11
    if (impl_->x11) return x11_read_clipboard(impl_->x11);
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
