// Native window backend for Gritcode — direct Wayland (primary) and X11 (fallback).
// Smooth resize via frame callbacks (Wayland) and _NET_WM_SYNC_REQUEST (X11).
// Adapted from https://github.com/lszl84/opengl-smooth (src/main.cpp).
//
// CSD rendering and input follow that reference 1:1: an FBO sized to the
// shadow-padded buffer, a compose shader that applies rounded corners and a
// gaussian drop shadow, and a close-button shader laid over the titlebar.
//
// Copyright (C) 2026 luke@devmindscape.com
// SPDX-License-Identifier: GPL-3.0-or-later

#include "glfw_window.h"
#include "types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/mman.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#ifdef HAVE_WAYLAND
#include <wayland-client.h>
#include <wayland-egl.h>
#include <wayland-cursor.h>
#include <EGL/egl.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#endif

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/sync.h>
#include <EGL/egl.h>
#endif

#include <poll.h>

static int s_wake_fd = -1;

static void wake_event_loop() {
    if (s_wake_fd >= 0) {
        uint64_t val = 1;
        (void)write(s_wake_fd, &val, sizeof(val));
    }
}

static void consume_wake() {
    if (s_wake_fd >= 0) {
        uint64_t val;
        (void)read(s_wake_fd, &val, sizeof(val));
    }
}

#ifdef HAVE_WAYLAND

namespace {

// opengl-smooth src/main.cpp:64-74 — the reference's CSD metrics.
constexpr int   TITLEBAR_H    = 40;
constexpr int   BORDER_GRAB   = 6;
constexpr int   CLOSE_SIZE    = 24;
constexpr int   CLOSE_MARGIN  = 8;
constexpr int   SHADOW_EXTENT = 32;
constexpr int   CORNER_RADIUS = 12;
constexpr float SHADOW_SIGMA  = 14.0f;
constexpr float TITLEBAR_R    = 0.13f;
constexpr float TITLEBAR_G    = 0.14f;
constexpr float TITLEBAR_B    = 0.18f;

// ---------------------------------------------------------------------------
// Close-button shader — verbatim from opengl-smooth src/main.cpp:172-216.
// ---------------------------------------------------------------------------
const char* kCloseVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec4 uRect;
uniform vec2 uScreen;
out vec2 vUV;
void main() {
    vec2 px  = uRect.xy + aPos * uRect.zw;
    vec2 ndc = vec2(px.x / uScreen.x,
                    1.0 - px.y / uScreen.y) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aPos;
}
)";

const char* kCloseFS = R"(#version 330 core
in  vec2 vUV;
out vec4 fragColor;
uniform float uHover;
uniform float uPressed;
uniform vec3  uBarColor;
void main() {
    vec2  p  = vUV * 2.0 - 1.0;
    float r  = length(p);
    float aa = fwidth(r);
    float disc = 1.0 - smoothstep(1.0 - aa, 1.0, r);
    mat2 R = mat2( 0.70710678, -0.70710678,
                   0.70710678,  0.70710678);
    vec2 q = R * p;
    const float cap_r = 0.055;
    const float cap_L = 0.34;
    float d1 = length(vec2(max(abs(q.x) - cap_L, 0.0), q.y)) - cap_r;
    float d2 = length(vec2(max(abs(q.y) - cap_L, 0.0), q.x)) - cap_r;
    float dx  = min(d1, d2);
    float aax = fwidth(dx);
    float xmask = 1.0 - smoothstep(0.0, aax, dx);
    vec3 normal_c  = vec3(0.245, 0.255, 0.295);
    vec3 hover_c   = vec3(0.320, 0.330, 0.370);
    vec3 pressed_c = vec3(0.420, 0.430, 0.470);
    vec3 bg = mix(normal_c, hover_c, uHover);
    bg      = mix(bg, pressed_c, uPressed);
    vec3 disc_col = mix(bg, vec3(1.0), xmask);
    vec3 col      = mix(uBarColor, disc_col, disc);
    fragColor     = vec4(col, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Compose shader — verbatim from opengl-smooth src/main.cpp:220-264. Samples
// the content FBO and applies rounded-rect SDF clip + gaussian drop shadow
// in a single full-screen pass.
// ---------------------------------------------------------------------------
const char* kFullQuadVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vPx;
out vec2 vUV;
uniform vec2 uScreen;
void main() {
    vec2 px  = aPos * uScreen;
    vec2 ndc = vec2(aPos.x, 1.0 - aPos.y) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vPx = px;
    vUV = vec2(aPos.x, 1.0 - aPos.y);
}
)";

const char* kComposeFS = R"(#version 330 core
in  vec2 vPx;
in  vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2  uScreen;
uniform vec4  uWindow;
uniform float uRadius;
uniform float uSigma;
uniform float uShadow;
uniform float uOutline;
void main() {
    vec2 center = uWindow.xy + uWindow.zw * 0.5;
    vec2 halfs  = uWindow.zw * 0.5;
    vec2 q = abs(vPx - center) - halfs + vec2(uRadius);
    float sdf = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius;
    float fw = fwidth(sdf);
    float inside = 1.0 - smoothstep(-fw * 0.5, fw * 0.5, sdf);
    float outline_band = smoothstep(-fw * 1.5, -fw * 0.5, sdf) *
                         (1.0 - smoothstep(-fw * 0.5, fw * 0.5, sdf));
    vec4 content = texture(uTex, vUV);
    vec3 content_rgb = mix(content.rgb,
                           content.rgb * (1.0 - uOutline),
                           outline_band);
    float d = max(sdf, 0.0);
    float shadow_alpha = uShadow * exp(-(d * d) / (2.0 * uSigma * uSigma));
    float out_a = inside + shadow_alpha * (1.0 - inside);
    vec3  out_rgb = content_rgb * inside / max(out_a, 1e-4);
    fragColor = vec4(out_rgb, out_a);
}
)";

bool compile_shader(GLuint shader, const char* src) {
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "grit: shader compile error: %s\n", log);
        return false;
    }
    return true;
}

GLuint link_program(const char* vs_src, const char* fs_src) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    if (!compile_shader(vs, vs_src) || !compile_shader(fs, fs_src)) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "grit: program link error: %s\n", log);
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

} // namespace

struct GlfwWindow::Impl {
    wl_display*          display      = nullptr;
    wl_compositor*       compositor   = nullptr;
    xdg_wm_base*         wm_base      = nullptr;
    zxdg_decoration_manager_v1* deco_mgr = nullptr;
    wl_seat*             seat         = nullptr;
    wl_pointer*          pointer      = nullptr;
    wl_keyboard*         keyboard     = nullptr;
    wl_shm*              shm          = nullptr;

    // Start in CSD: compositors without xdg-decoration-manager never send
    // the decoration_configure event, so this stays true. Mirrors
    // opengl-smooth src/main.cpp:276.
    bool                 use_csd      = true;

    wl_surface*          surface      = nullptr;
    xdg_surface*         xsurface     = nullptr;
    xdg_toplevel*        toplevel     = nullptr;
    zxdg_toplevel_decoration_v1* deco = nullptr;

    wl_egl_window*       egl_window   = nullptr;
    EGLDisplay           egl_display  = EGL_NO_DISPLAY;
    EGLContext           egl_context  = EGL_NO_CONTEXT;
    EGLSurface           egl_surface  = EGL_NO_SURFACE;
    EGLConfig            egl_config   = nullptr;

    wl_cursor_theme*     cursor_theme   = nullptr;
    wl_surface*          cursor_surface = nullptr;
    uint32_t             last_enter_serial = 0;
    const char*          current_cursor = nullptr;

    int min_width  = 80;   // opengl-smooth src/main.cpp:321-322
    int min_height = 80;

    int width  = 900;   // logical window size (excluding shadow)
    int height = 700;
    int pending_width  = 0;
    int pending_height = 0;
    int scale  = 1;

    int applied_buffer_w = 0;  // includes shadow extent when CSD floats
    int applied_buffer_h = 0;

    bool egl_ready  = false;
    bool running    = true;
    bool configured = false;

    // opengl-smooth src/main.cpp:338-340. When any of these is set the
    // window no longer draws its own shadow/rounded corners.
    bool tiled      = false;
    bool maximized  = false;
    bool fullscreen = false;

    double px = 0, py = 0;   // pointer in content-logical coords (shadow-subtracted)
    bool   left_down  = false;
    bool   shift_held = false;

    // Close-button CSD state (opengl-smooth src/main.cpp:281-290, 343).
    bool   hover_close     = false;
    bool   close_pressed   = false;
    float  close_hover_amt = 0.0f;

    GLuint close_prog    = 0;
    GLuint close_vao     = 0;
    GLuint close_vbo     = 0;
    GLint  uRect_loc     = -1;
    GLint  uScreen_loc   = -1;
    GLint  uHover_loc    = -1;
    GLint  uPressed_loc  = -1;
    GLint  uBarColor_loc = -1;

    // Content FBO + compose pass (opengl-smooth src/main.cpp:292-307).
    GLuint fbo          = 0;
    GLuint fbo_tex      = 0;
    int    fbo_w        = 0;
    int    fbo_h        = 0;

    GLuint quad_vao     = 0;
    GLuint quad_vbo     = 0;

    GLuint compose_prog           = 0;
    GLint  compose_uTex_loc       = -1;
    GLint  compose_uScreen_loc    = -1;
    GLint  compose_uWindow_loc    = -1;
    GLint  compose_uRadius_loc    = -1;
    GLint  compose_uSigma_loc     = -1;
    GLint  compose_uShadow_loc    = -1;
    GLint  compose_uOutline_loc   = -1;

    struct xkb_context*  xkb_ctx = nullptr;
    struct xkb_keymap*   xkb_km  = nullptr;
    struct xkb_state*    xkb_st  = nullptr;

    GlfwWindow* owner = nullptr;
    const char* title = nullptr;

    int titlebar_h = TITLEBAR_H;
    RectI close_btn = {};
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void registry_global(void*, wl_registry*, uint32_t, const char*, uint32_t);
static void wm_base_ping(void*, xdg_wm_base*, uint32_t);
static void xdg_surface_configure(void*, xdg_surface*, uint32_t);
static void toplevel_configure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*);
static void toplevel_close(void*, xdg_toplevel*);
static void deco_configure(void*, zxdg_toplevel_decoration_v1*, uint32_t);
static void surface_preferred_scale(void*, wl_surface*, int32_t);
static void pointer_enter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t);
static void pointer_leave(void*, wl_pointer*, uint32_t, wl_surface*);
static void pointer_motion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t);
static void pointer_button(void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t);
static void pointer_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t);
static void seat_capabilities(void*, wl_seat*, uint32_t);
static void kb_keymap(void*, wl_keyboard*, uint32_t, int32_t, uint32_t);
static void kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*);
static void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*);
static void kb_key(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t);
static void kb_modifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static void kb_repeat_info(void*, wl_keyboard*, int32_t, int32_t);

static void wl_sync_surface_geometry(GlfwWindow::Impl& d);

static const wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = [](void*, wl_registry*, uint32_t) {},
};

static const xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static const xdg_surface_listener xdg_surface_listener_impl = {
    .configure = xdg_surface_configure,
};

static const xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {},
    .wm_capabilities = [](void*, xdg_toplevel*, wl_array*) {},
};

static const zxdg_toplevel_decoration_v1_listener deco_listener = {
    .configure = deco_configure,
};

static const wl_surface_listener surface_listener = {
    .enter = [](void*, wl_surface*, wl_output*) {},
    .leave = [](void*, wl_surface*, wl_output*) {},
    .preferred_buffer_scale = surface_preferred_scale,
    .preferred_buffer_transform = [](void*, wl_surface*, uint32_t) {},
};

static void pointer_frame(void*, wl_pointer*) {}
static void pointer_axis_source(void*, wl_pointer*, uint32_t) {}
static void pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
static void pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
static void pointer_axis_value120(void*, wl_pointer*, uint32_t, int32_t) {}
static void pointer_axis_relative(void*, wl_pointer*, uint32_t, uint32_t) {}

static const wl_pointer_listener pointer_listener_impl = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative,
};

static const wl_seat_listener seat_listener_impl = {
    .capabilities = seat_capabilities,
    .name = [](void*, wl_seat*, const char*) {},
};

static const wl_keyboard_listener kb_listener_impl = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat_info,
};

// ---------------------------------------------------------------------------
// CSD math helpers (opengl-smooth src/main.cpp:348-352)
// ---------------------------------------------------------------------------

static bool wl_floating(const GlfwWindow::Impl& d) {
    return !d.tiled && !d.maximized && !d.fullscreen;
}
static int wl_eff_shadow(const GlfwWindow::Impl& d) {
    return (d.use_csd && wl_floating(d)) ? SHADOW_EXTENT : 0;
}
static int wl_eff_radius(const GlfwWindow::Impl& d) {
    return (d.use_csd && wl_floating(d)) ? CORNER_RADIUS : 0;
}

// ---------------------------------------------------------------------------
// Cursor helpers (opengl-smooth src/main.cpp:707-752)
// ---------------------------------------------------------------------------

static wl_cursor* lookup_cursor(wl_cursor_theme* theme, const char* name) {
    if (wl_cursor* c = wl_cursor_theme_get_cursor(theme, name)) return c;
    struct { const char* css; const char* legacy; } aliases[] = {
        { "default",   "left_ptr" },
        { "n-resize",  "top_side" },
        { "s-resize",  "bottom_side" },
        { "w-resize",  "left_side" },
        { "e-resize",  "right_side" },
        { "nw-resize", "top_left_corner" },
        { "ne-resize", "top_right_corner" },
        { "sw-resize", "bottom_left_corner" },
        { "se-resize", "bottom_right_corner" },
        { "text",      "xterm" },
    };
    for (const auto& a : aliases) {
        if (std::strcmp(name, a.css) == 0)
            if (wl_cursor* c = wl_cursor_theme_get_cursor(theme, a.legacy)) return c;
    }
    return nullptr;
}

static void set_cursor(GlfwWindow::Impl& d, const char* name) {
    if (!d.pointer || !d.cursor_theme || !d.cursor_surface) return;
    if (d.current_cursor == name) return;
    d.current_cursor = name;

    wl_cursor* cursor = lookup_cursor(d.cursor_theme, name);
    if (!cursor || cursor->image_count == 0) return;
    wl_cursor_image* img = cursor->images[0];
    wl_buffer* buf = wl_cursor_image_get_buffer(img);
    if (!buf) return;

    wl_pointer_set_cursor(d.pointer, d.last_enter_serial,
                          d.cursor_surface, img->hotspot_x, img->hotspot_y);
    wl_surface_attach(d.cursor_surface, buf, 0, 0);
    wl_surface_damage_buffer(d.cursor_surface, 0, 0, img->width, img->height);
    wl_surface_commit(d.cursor_surface);
}

// ---------------------------------------------------------------------------
// Edge / close-button hit-tests (opengl-smooth src/main.cpp:677-732)
// ---------------------------------------------------------------------------

static uint32_t edge_for(const GlfwWindow::Impl& d, double x, double y) {
    const bool L = x < BORDER_GRAB;
    const bool R = x > d.width  - BORDER_GRAB;
    const bool T = y < BORDER_GRAB;
    const bool B = y > d.height - BORDER_GRAB;
    if (T && L) return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    if (T && R) return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    if (B && L) return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    if (B && R) return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    if (L)      return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    if (R)      return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    if (T)      return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    if (B)      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
}

static const char* cursor_name_for_edge(uint32_t edge) {
    switch (edge) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP:          return "n-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:       return "s-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:         return "w-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:        return "e-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:     return "nw-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:    return "ne-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:  return "sw-resize";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT: return "se-resize";
    default:                                    return "default";
    }
}

static bool in_close(const GlfwWindow::Impl& d, double x, double y) {
    const int cx = d.width - CLOSE_MARGIN - CLOSE_SIZE;
    const int cy = (TITLEBAR_H - CLOSE_SIZE) / 2;
    return x >= cx && x < cx + CLOSE_SIZE && y >= cy && y < cy + CLOSE_SIZE;
}

struct CloseRectPx { int x, y, w, h; };
static CloseRectPx close_rect_buf(const GlfwWindow::Impl& d) {
    const int S  = d.scale;
    const int sh = wl_eff_shadow(d);
    const int x = (sh + d.width - CLOSE_MARGIN - CLOSE_SIZE) * S;
    const int y = (sh + (TITLEBAR_H - CLOSE_SIZE) / 2) * S;
    return { x, y, CLOSE_SIZE * S, CLOSE_SIZE * S };
}

static void update_hover(GlfwWindow::Impl& d) {
    if (!d.use_csd) {
        d.hover_close = false;
        set_cursor(d, "default");
        return;
    }
    const bool hov = in_close(d, d.px, d.py);
    if (hov != d.hover_close) d.hover_close = hov;
    const uint32_t edge = edge_for(d, d.px, d.py);
    const char* name = hov ? "default" : cursor_name_for_edge(edge);
    set_cursor(d, name);
}

// ---------------------------------------------------------------------------
// FBO + compose pipeline (opengl-smooth src/main.cpp:405-510)
// ---------------------------------------------------------------------------

static bool init_close_button(GlfwWindow::Impl& d) {
    d.close_prog = link_program(kCloseVS, kCloseFS);
    if (!d.close_prog) return false;

    d.uRect_loc     = glGetUniformLocation(d.close_prog, "uRect");
    d.uScreen_loc   = glGetUniformLocation(d.close_prog, "uScreen");
    d.uHover_loc    = glGetUniformLocation(d.close_prog, "uHover");
    d.uPressed_loc  = glGetUniformLocation(d.close_prog, "uPressed");
    d.uBarColor_loc = glGetUniformLocation(d.close_prog, "uBarColor");

    const float quad[] = {
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
    };
    glGenVertexArrays(1, &d.close_vao);
    glBindVertexArray(d.close_vao);
    glGenBuffers(1, &d.close_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, d.close_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    return true;
}

static bool init_compose(GlfwWindow::Impl& d) {
    d.compose_prog = link_program(kFullQuadVS, kComposeFS);
    if (!d.compose_prog) return false;
    d.compose_uTex_loc     = glGetUniformLocation(d.compose_prog, "uTex");
    d.compose_uScreen_loc  = glGetUniformLocation(d.compose_prog, "uScreen");
    d.compose_uWindow_loc  = glGetUniformLocation(d.compose_prog, "uWindow");
    d.compose_uRadius_loc  = glGetUniformLocation(d.compose_prog, "uRadius");
    d.compose_uSigma_loc   = glGetUniformLocation(d.compose_prog, "uSigma");
    d.compose_uShadow_loc  = glGetUniformLocation(d.compose_prog, "uShadow");
    d.compose_uOutline_loc = glGetUniformLocation(d.compose_prog, "uOutline");

    const float quad[] = {
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
    };
    glGenVertexArrays(1, &d.quad_vao);
    glBindVertexArray(d.quad_vao);
    glGenBuffers(1, &d.quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, d.quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    return true;
}

static void ensure_fbo(GlfwWindow::Impl& d, int w, int h) {
    if (w == d.fbo_w && h == d.fbo_h && d.fbo) return;
    if (!d.fbo)     glGenFramebuffers(1, &d.fbo);
    if (!d.fbo_tex) glGenTextures(1, &d.fbo_tex);

    glBindTexture(GL_TEXTURE_2D, d.fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, d.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, d.fbo_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    d.fbo_w = w; d.fbo_h = h;
}

// opengl-smooth src/main.cpp:435-453
static void draw_close_button(const GlfwWindow::Impl& d) {
    const int S = d.scale;
    const int sh = wl_eff_shadow(d);
    const int W  = (d.width  + 2 * sh) * S;
    const int H  = (d.height + 2 * sh) * S;
    const auto r = close_rect_buf(d);

    glUseProgram(d.close_prog);
    glUniform4f(d.uRect_loc, float(r.x), float(r.y), float(r.w), float(r.h));
    glUniform2f(d.uScreen_loc, float(W), float(H));
    glUniform1f(d.uHover_loc, d.close_hover_amt);
    glUniform1f(d.uPressed_loc, d.close_pressed ? 1.0f : 0.0f);
    glUniform3f(d.uBarColor_loc, TITLEBAR_R, TITLEBAR_G, TITLEBAR_B);

    glBindVertexArray(d.close_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

// opengl-smooth src/main.cpp:455-510. For non-CSD, the app has already
// rendered straight into the default framebuffer (see GlfwWindow::BeginFrame)
// and there is no compose step. For CSD, we unbind the FBO, clear the
// default FB transparent, and blit through the compose shader.
static void compose_frame(GlfwWindow::Impl& d) {
    const int S  = d.scale;
    const int sh = wl_eff_shadow(d);
    const int W  = (d.width  + 2 * sh) * S;
    const int H  = (d.height + 2 * sh) * S;
    const int wx = sh * S;
    const int wy = sh * S;
    const int winW = d.width  * S;
    const int winH = d.height * S;
    const int rad = wl_eff_radius(d);

    // Hover easing (opengl-smooth src/main.cpp:573-580).
    const float target = d.hover_close ? 1.0f : 0.0f;
    const float step = 0.18f;
    if (target > d.close_hover_amt)
        d.close_hover_amt = std::min(target, d.close_hover_amt + step);
    else
        d.close_hover_amt = std::max(target, d.close_hover_amt - step);

    // Close button lives in the FBO so compose applies the rounded clip to
    // it. Its shader converts buffer-pixel uRect + uScreen into NDC; that
    // math assumes the viewport covers the whole FBO, so reset the viewport
    // from the app's content rect back to (0, 0, W, H) before drawing.
    glViewport(0, 0, W, H);
    draw_close_button(d);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // GLRenderer::Init enables GL_BLEND with (GL_SRC_ALPHA,
    // GL_ONE_MINUS_SRC_ALPHA) and every subsequent frame's glyph/rounded-rect
    // draws depend on that exact state. The compose pass needs separate
    // alpha blending so the drop-shadow alpha accumulates correctly — switch
    // it on for the compose draw, then restore the renderer's defaults.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE,       GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(d.compose_prog);
    glUniform2f(d.compose_uScreen_loc, float(W), float(H));
    glUniform4f(d.compose_uWindow_loc,
                float(wx), float(wy), float(winW), float(winH));
    glUniform1f(d.compose_uRadius_loc, float(rad * S));
    glUniform1f(d.compose_uSigma_loc,  SHADOW_SIGMA * S);
    glUniform1f(d.compose_uShadow_loc,  wl_floating(d) ? 0.275f : 0.0f);
    glUniform1f(d.compose_uOutline_loc, wl_floating(d) ? 0.18f  : 0.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d.fbo_tex);
    glUniform1i(d.compose_uTex_loc, 0);

    glBindVertexArray(d.quad_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    // Restore renderer blend state for the next frame.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// ---------------------------------------------------------------------------
// Surface geometry / input / opaque region (opengl-smooth src/main.cpp:523-554)
// ---------------------------------------------------------------------------

static void wl_sync_surface_geometry(GlfwWindow::Impl& d) {
    if (!d.use_csd) {
        xdg_surface_set_window_geometry(d.xsurface,
                                        0, 0, d.width, d.height);
        wl_surface_set_input_region(d.surface, nullptr);
        if (wl_region* r = wl_compositor_create_region(d.compositor)) {
            wl_region_add(r, 0, 0, d.width, d.height);
            wl_surface_set_opaque_region(d.surface, r);
            wl_region_destroy(r);
        }
        return;
    }

    const int sh = wl_eff_shadow(d);
    xdg_surface_set_window_geometry(d.xsurface,
                                    sh, sh, d.width, d.height);

    if (wl_region* r = wl_compositor_create_region(d.compositor)) {
        wl_region_add(r, sh, sh, d.width, d.height);
        wl_surface_set_input_region(d.surface, r);
        wl_region_destroy(r);
    }
    if (wl_region* r = wl_compositor_create_region(d.compositor)) {
        const int rad = wl_eff_radius(d);
        if (d.width > 2 * rad && d.height > 2 * rad) {
            wl_region_add(r, sh + rad, sh + rad,
                          d.width - 2 * rad, d.height - 2 * rad);
        }
        wl_surface_set_opaque_region(d.surface, r);
        wl_region_destroy(r);
    }
}

// ---------------------------------------------------------------------------
// Wayland listener implementations
// ---------------------------------------------------------------------------

static void registry_global(void* data, wl_registry* reg, uint32_t name,
                            const char* iface, uint32_t) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        d.compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 6));
    } else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0) {
        d.wm_base = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(d.wm_base, &wm_base_listener, &d);
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        d.seat = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, 5));
        wl_seat_add_listener(d.seat, &seat_listener_impl, &d);
    } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        d.shm = static_cast<wl_shm*>(
            wl_registry_bind(reg, name, &wl_shm_interface, 1));
    } else if (std::strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
        d.deco_mgr = static_cast<zxdg_decoration_manager_v1*>(
            wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1));
    }
}

static void wm_base_ping(void*, xdg_wm_base* wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}

// Report content area (buf_w, content_h) to app, where content_h is the
// buffer height minus the titlebar row (in CSD) — the shadow extent is not
// part of the reported height because it's decorative padding the app never
// draws into.
static void report_resize(GlfwWindow::Impl& d) {
    if (!d.owner || !d.owner->resizeCb_) return;
    int buf_w = d.width * d.scale;
    int content_h = d.height * d.scale;
    if (d.use_csd) content_h -= TITLEBAR_H * d.scale;
    if (content_h < 1) content_h = 1;
    d.owner->resizeCb_(buf_w, content_h, (float)d.scale);
}

static void xdg_surface_configure(void* data, xdg_surface* s, uint32_t serial) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    xdg_surface_ack_configure(s, serial);
    d.configured = true;

    if (d.pending_width > 0 && d.pending_height > 0) {
        bool changed = (d.pending_width != d.width || d.pending_height != d.height);
        d.width  = d.pending_width;
        d.height = d.pending_height;

        const int sh = wl_eff_shadow(d);
        int buf_w = (d.width  + 2 * sh) * d.scale;
        int buf_h = (d.height + 2 * sh) * d.scale;
        if (buf_w != d.applied_buffer_w || buf_h != d.applied_buffer_h) {
            wl_egl_window_resize(d.egl_window, buf_w, buf_h, 0, 0);
            d.applied_buffer_w = buf_w;
            d.applied_buffer_h = buf_h;
            wl_sync_surface_geometry(d);
        }

        if (changed) report_resize(d);
    }
}

static void toplevel_configure(void* data, xdg_toplevel*,
                               int32_t w, int32_t h, wl_array* states) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    if (w > 0) d.pending_width  = std::max(w, d.min_width);
    if (h > 0) d.pending_height = std::max(h, d.min_height);

    bool tiled = false, max = false, full = false;
    uint32_t* s;
    for (s = (uint32_t*)states->data;
         (const char*)s < (const char*)states->data + states->size; ++s) {
        switch (*s) {
        case XDG_TOPLEVEL_STATE_MAXIMIZED:    max  = true; break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:   full = true; break;
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM: tiled = true; break;
        default: break;
        }
    }
    const bool was_floating = wl_floating(d);
    d.tiled = tiled; d.maximized = max; d.fullscreen = full;
    if (was_floating != wl_floating(d)) {
        // Shadow extent changes — force buffer/geometry re-layout on the
        // next xdg_surface_configure.
        d.applied_buffer_w = 0;
        d.applied_buffer_h = 0;
    }
}

static void toplevel_close(void* data, xdg_toplevel*) {
    static_cast<GlfwWindow::Impl*>(data)->running = false;
}

static void deco_configure(void* data, zxdg_toplevel_decoration_v1*,
                           uint32_t mode) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    bool was_csd = d.use_csd;
    d.use_csd = (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    std::fprintf(stderr, "grit: decoration mode: %s\n",
                 d.use_csd ? "CSD" : "SSD");
    if (was_csd != d.use_csd) {
        d.applied_buffer_w = 0;
        d.applied_buffer_h = 0;
        if (d.egl_ready) report_resize(d);
    }
}

static void surface_preferred_scale(void* data, wl_surface*, int32_t factor) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    if (factor < 1) factor = 1;
    if (factor == d.scale) return;
    d.scale = factor;
    wl_surface_set_buffer_scale(d.surface, factor);

    const int sh = wl_eff_shadow(d);
    int buf_w = (d.width  + 2 * sh) * d.scale;
    int buf_h = (d.height + 2 * sh) * d.scale;
    if (d.egl_window && (buf_w != d.applied_buffer_w || buf_h != d.applied_buffer_h)) {
        wl_egl_window_resize(d.egl_window, buf_w, buf_h, 0, 0);
        d.applied_buffer_w = buf_w;
        d.applied_buffer_h = buf_h;
    }
    report_resize(d);
}

// ---------------------------------------------------------------------------
// Pointer input — opengl-smooth src/main.cpp:774-828. d.px/d.py carry the
// shadow-subtracted content-local coords so edge/close/titlebar hit-tests
// can use them directly.
// ---------------------------------------------------------------------------

static void deliver_mouse_move(GlfwWindow::Impl& d) {
    if (!d.owner || !d.owner->mouseMoveCb_) return;
    double cy = d.py;
    if (d.use_csd) cy -= TITLEBAR_H;
    d.owner->mouseMoveCb_((float)(d.px * d.scale),
                          (float)(cy * d.scale),
                          d.left_down);
}

static void pointer_enter(void* data, wl_pointer*, uint32_t serial,
                          wl_surface*, wl_fixed_t sx, wl_fixed_t sy) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    d.last_enter_serial = serial;
    d.current_cursor = nullptr;
    const double o = double(wl_eff_shadow(d));
    d.px = wl_fixed_to_double(sx) - o;
    d.py = wl_fixed_to_double(sy) - o;
    update_hover(d);
}

static void pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    d.hover_close = false;
}

static void pointer_motion(void* data, wl_pointer*, uint32_t,
                           wl_fixed_t sx, wl_fixed_t sy) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    const double o = double(wl_eff_shadow(d));
    d.px = wl_fixed_to_double(sx) - o;
    d.py = wl_fixed_to_double(sy) - o;
    update_hover(d);
    deliver_mouse_move(d);
}

static void pointer_button(void* data, wl_pointer*, uint32_t serial,
                           uint32_t, uint32_t button, uint32_t state) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    constexpr uint32_t BTN_LEFT = 0x110;
    if (button != BTN_LEFT) return;

    bool pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    d.left_down = pressed;

    auto dispatch_to_app = [&] {
        if (!d.owner || !d.owner->mouseBtnCb_) return;
        double cy = d.py;
        if (d.use_csd) cy -= TITLEBAR_H;
        d.owner->mouseBtnCb_((float)(d.px * d.scale),
                             (float)(cy * d.scale),
                             pressed, d.shift_held);
    };

    if (!d.use_csd) {
        dispatch_to_app();
        return;
    }

    if (pressed) {
        if (in_close(d, d.px, d.py)) {
            d.close_pressed = true;
            return;
        }
        const uint32_t edge = edge_for(d, d.px, d.py);
        if (edge != XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
            xdg_toplevel_resize(d.toplevel, d.seat, serial, edge);
            return;
        }
        if (d.py < TITLEBAR_H) {
            xdg_toplevel_move(d.toplevel, d.seat, serial);
            return;
        }
        dispatch_to_app();
    } else {
        if (d.close_pressed) {
            const bool still = in_close(d, d.px, d.py);
            d.close_pressed = false;
            if (still) d.running = false;
            return;
        }
        dispatch_to_app();
    }
}

static void pointer_axis(void* data, wl_pointer*, uint32_t,
                         uint32_t axis, wl_fixed_t value) {
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    float delta = -(float)wl_fixed_to_double(value) / 10.0f;
    if (d.owner && d.owner->scrollCb_) d.owner->scrollCb_(delta);
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

static void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d.pointer) {
        d.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(d.pointer, &pointer_listener_impl, &d);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d.pointer) {
        wl_pointer_destroy(d.pointer);
        d.pointer = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d.keyboard) {
        d.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(d.keyboard, &kb_listener_impl, &d);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d.keyboard) {
        wl_keyboard_destroy(d.keyboard);
        d.keyboard = nullptr;
    }
}

static void kb_keymap(void* data, wl_keyboard*, uint32_t format,
                      int32_t fd, uint32_t size) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }

    char* map = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (map == MAP_FAILED) return;

    if (d.xkb_km) xkb_keymap_unref(d.xkb_km);
    if (d.xkb_st) xkb_state_unref(d.xkb_st);

    d.xkb_km = xkb_keymap_new_from_string(d.xkb_ctx, map,
                                           XKB_KEYMAP_FORMAT_TEXT_V1,
                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (d.xkb_km) d.xkb_st = xkb_state_new(d.xkb_km);
}

static void kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
static void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}

static void kb_key(void* data, wl_keyboard*, uint32_t, uint32_t,
                   uint32_t key, uint32_t state) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    if (!d.xkb_st) return;

    uint32_t keycode = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(d.xkb_st, keycode);
    bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);

    int mods = 0;
    if (xkb_state_mod_name_is_active(d.xkb_st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE))
        mods |= Mod::Ctrl;
    if (xkb_state_mod_name_is_active(d.xkb_st, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE))
        mods |= Mod::Shift;
    d.shift_held = (mods & Mod::Shift) != 0;

    if (sym && pressed && d.owner && d.owner->keyCb_)
        d.owner->keyCb_((int)sym, mods, true);

    if (pressed && !(mods & Mod::Ctrl)) {
        uint32_t cp = xkb_state_key_get_utf32(d.xkb_st, keycode);
        if (cp >= 32 && d.owner && d.owner->charCb_)
            d.owner->charCb_(cp);
    }
}

static void kb_modifiers(void* data, wl_keyboard*, uint32_t,
                         uint32_t depressed, uint32_t latched,
                         uint32_t locked, uint32_t group) {
    auto& d = *static_cast<GlfwWindow::Impl*>(data);
    if (d.xkb_st)
        xkb_state_update_mask(d.xkb_st, depressed, latched, locked, 0, 0, group);
}

static void kb_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}

// ---------------------------------------------------------------------------
// EGL init (opengl-smooth src/main.cpp:888-929)
// ---------------------------------------------------------------------------

static bool init_egl(GlfwWindow::Impl& d) {
    d.egl_display = eglGetDisplay(
        reinterpret_cast<EGLNativeDisplayType>(d.display));
    if (d.egl_display == EGL_NO_DISPLAY) return false;
    if (!eglInitialize(d.egl_display, nullptr, nullptr)) return false;
    if (!eglBindAPI(EGL_OPENGL_API)) return false;

    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_STENCIL_SIZE, 8,
        EGL_SAMPLE_BUFFERS, 1,
        EGL_SAMPLES, 4,
        EGL_NONE
    };
    EGLint n = 0;
    if (!eglChooseConfig(d.egl_display, attrs, &d.egl_config, 1, &n) || n < 1)
        return false;

    const EGLint ctx_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    d.egl_context = eglCreateContext(
        d.egl_display, d.egl_config, EGL_NO_CONTEXT, ctx_attrs);
    if (d.egl_context == EGL_NO_CONTEXT) return false;

    const int sh = wl_eff_shadow(d);
    int buf_w = (d.width  + 2 * sh) * d.scale;
    int buf_h = (d.height + 2 * sh) * d.scale;
    d.egl_window = wl_egl_window_create(d.surface, buf_w, buf_h);
    d.applied_buffer_w = buf_w;
    d.applied_buffer_h = buf_h;

    d.egl_surface = eglCreateWindowSurface(
        d.egl_display, d.egl_config,
        reinterpret_cast<EGLNativeWindowType>(d.egl_window), nullptr);
    if (d.egl_surface == EGL_NO_SURFACE) return false;

    if (!eglMakeCurrent(d.egl_display, d.egl_surface,
                        d.egl_surface, d.egl_context)) return false;

    eglSwapInterval(d.egl_display, 0);
    return true;
}

// --- Event dispatch helpers ---

static void dispatch_pending(GlfwWindow::Impl& d) {
    while (wl_display_prepare_read(d.display) != 0)
        wl_display_dispatch_pending(d.display);
    wl_display_flush(d.display);

    struct pollfd pfd = { wl_display_get_fd(d.display), POLLIN, 0 };
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
        wl_display_read_events(d.display);
    else
        wl_display_cancel_read(d.display);

    wl_display_dispatch_pending(d.display);
}

static void wait_and_dispatch(GlfwWindow::Impl& d, int timeout_ms) {
    wl_display_dispatch_pending(d.display);

    while (wl_display_prepare_read(d.display) != 0)
        wl_display_dispatch_pending(d.display);
    wl_display_flush(d.display);

    struct pollfd fds[2] = {
        { wl_display_get_fd(d.display), POLLIN, 0 },
        { s_wake_fd, POLLIN, 0 },
    };
    int nfds = (s_wake_fd >= 0) ? 2 : 1;
    poll(fds, nfds, timeout_ms);

    if (fds[0].revents & POLLIN)
        wl_display_read_events(d.display);
    else
        wl_display_cancel_read(d.display);

    if (nfds > 1 && (fds[1].revents & POLLIN))
        consume_wake();

    wl_display_dispatch_pending(d.display);
}

// ============================================================================
// X11 backend (stub — to be implemented)
// ============================================================================

#elif defined(HAVE_X11)

struct GlfwWindow::Impl {
    Display*    dpy = nullptr;
    Window      win = 0;
    EGLDisplay  egl_dpy = EGL_NO_DISPLAY;
    EGLContext  egl_ctx = EGL_NO_CONTEXT;
    EGLSurface  egl_srf = EGL_NO_SURFACE;
    EGLConfig   egl_cfg = nullptr;

    int width = 900;
    int height = 700;
    int scale = 1;
    bool running = true;
    bool maximized = false;
    GlfwWindow* owner = nullptr;
};

#else

struct GlfwWindow::Impl {
    int width = 900;
    int height = 700;
    int scale = 1;
    bool running = true;
    bool maximized = false;
};

#endif

// ============================================================================
// GlfwWindow public methods
// ============================================================================

GlfwWindow::GlfwWindow() : impl_(new Impl()) {}

GlfwWindow::~GlfwWindow() {
#ifdef HAVE_WAYLAND
    auto& d = *impl_;
    if (d.compose_prog) glDeleteProgram(d.compose_prog);
    if (d.quad_vbo)     glDeleteBuffers(1, &d.quad_vbo);
    if (d.quad_vao)     glDeleteVertexArrays(1, &d.quad_vao);
    if (d.fbo_tex)      glDeleteTextures(1, &d.fbo_tex);
    if (d.fbo)          glDeleteFramebuffers(1, &d.fbo);
    if (d.close_prog)   glDeleteProgram(d.close_prog);
    if (d.close_vbo)    glDeleteBuffers(1, &d.close_vbo);
    if (d.close_vao)    glDeleteVertexArrays(1, &d.close_vao);
    if (d.deco) zxdg_toplevel_decoration_v1_destroy(d.deco);
    if (d.egl_surface != EGL_NO_SURFACE)
        eglDestroySurface(d.egl_display, d.egl_surface);
    if (d.egl_window) wl_egl_window_destroy(d.egl_window);
    if (d.egl_context != EGL_NO_CONTEXT)
        eglDestroyContext(d.egl_display, d.egl_context);
    if (d.egl_display != EGL_NO_DISPLAY) eglTerminate(d.egl_display);
    if (d.xkb_st) xkb_state_unref(d.xkb_st);
    if (d.xkb_km) xkb_keymap_unref(d.xkb_km);
    if (d.xkb_ctx) xkb_context_unref(d.xkb_ctx);
    if (d.display) wl_display_disconnect(d.display);
    if (s_wake_fd >= 0) { close(s_wake_fd); s_wake_fd = -1; }
#endif
    delete impl_;
}

bool GlfwWindow::Init(int width, int height, const char* title) {
    auto& d = *impl_;
    d.owner = this;
    d.width = width;
    d.height = height;
    d.title = title;

#ifdef HAVE_WAYLAND
    if (!std::getenv("WAYLAND_DISPLAY")) {
        std::fprintf(stderr, "grit: WAYLAND_DISPLAY not set, cannot init Wayland\n");
        return false;
    }

    s_wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    d.display = wl_display_connect(nullptr);
    if (!d.display) {
        std::fprintf(stderr, "grit: wl_display_connect failed\n");
        return false;
    }

    wl_registry* reg = wl_display_get_registry(d.display);
    wl_registry_add_listener(reg, &registry_listener, &d);
    wl_display_roundtrip(d.display);

    if (!d.compositor || !d.wm_base) {
        std::fprintf(stderr, "grit: missing compositor or xdg_wm_base\n");
        wl_display_disconnect(d.display);
        d.display = nullptr;
        return false;
    }
    wl_display_roundtrip(d.display);

    std::fprintf(stderr, "grit: Wayland backend connected\n");

    d.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    if (d.shm) {
        const char* size_env = std::getenv("XCURSOR_SIZE");
        int cursor_size = size_env ? std::atoi(size_env) : 24;
        if (cursor_size <= 0) cursor_size = 24;
        d.cursor_theme = wl_cursor_theme_load(
            std::getenv("XCURSOR_THEME"), cursor_size, d.shm);
        d.cursor_surface = wl_compositor_create_surface(d.compositor);
    }

    d.surface = wl_compositor_create_surface(d.compositor);
    wl_surface_add_listener(d.surface, &surface_listener, &d);
    d.xsurface = xdg_wm_base_get_xdg_surface(d.wm_base, d.surface);
    xdg_surface_add_listener(d.xsurface, &xdg_surface_listener_impl, &d);
    d.toplevel = xdg_surface_get_toplevel(d.xsurface);
    xdg_toplevel_add_listener(d.toplevel, &toplevel_listener, &d);
    xdg_toplevel_set_app_id(d.toplevel, "gritcode");
    xdg_toplevel_set_title(d.toplevel, title);
    xdg_toplevel_set_min_size(d.toplevel, d.min_width, d.min_height);

    if (d.deco_mgr) {
        d.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(
            d.deco_mgr, d.toplevel);
        zxdg_toplevel_decoration_v1_add_listener(d.deco, &deco_listener, &d);
        zxdg_toplevel_decoration_v1_set_mode(
            d.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else {
        std::fprintf(stderr, "grit: no xdg-decoration-manager, using CSD\n");
        d.use_csd = true;
    }

    wl_surface_commit(d.surface);
    wl_display_roundtrip(d.display);

    if (!init_egl(d)) {
        std::fprintf(stderr, "grit: EGL init failed\n");
        return false;
    }
    if (!init_close_button(d)) {
        std::fprintf(stderr, "grit: close-button init failed\n");
        return false;
    }
    if (!init_compose(d)) {
        std::fprintf(stderr, "grit: compose shader init failed\n");
        return false;
    }

    wl_sync_surface_geometry(d);

    d.egl_ready = true;
    std::fprintf(stderr, "grit: window ready (%dx%d, scale=%d)\n",
                 d.width, d.height, d.scale);
    return true;

#else
    std::fprintf(stderr, "grit: no display backend compiled in\n");
    return false;
#endif
}

void GlfwWindow::Show() {
    // Wayland windows are shown on first commit (already done in Init)
}

void GlfwWindow::SetTitle(const char* title) {
#ifdef HAVE_WAYLAND
    if (impl_->toplevel)
        xdg_toplevel_set_title(impl_->toplevel, title);
#endif
}

bool GlfwWindow::ShouldClose() const { return !impl_->running; }

void GlfwWindow::RequestClose() { impl_->running = false; }

void GlfwWindow::PollEvents() {
#ifdef HAVE_WAYLAND
    if (impl_->display)
        dispatch_pending(*impl_);
#endif
}

void GlfwWindow::WaitEvents() {
#ifdef HAVE_WAYLAND
    if (impl_->display)
        wait_and_dispatch(*impl_, -1);
#endif
}

void GlfwWindow::WaitEventsTimeout(double timeout) {
#ifdef HAVE_WAYLAND
    if (impl_->display)
        wait_and_dispatch(*impl_, (int)(timeout * 1000.0));
#endif
}

// Called by the app just before GLRenderer::BeginFrame. For CSD, bind the
// content FBO at the current buffer size and clear it transparent so the
// shadow padding outside the window rect stays empty — the compose pass
// later turns that transparency into the drop shadow.
void GlfwWindow::BeginFrame() {
#ifdef HAVE_WAYLAND
    auto& d = *impl_;
    if (!d.egl_ready) return;

    if (d.use_csd) {
        const int sh = wl_eff_shadow(d);
        const int W  = (d.width  + 2 * sh) * d.scale;
        const int H  = (d.height + 2 * sh) * d.scale;
        ensure_fbo(d, W, H);
        glBindFramebuffer(GL_FRAMEBUFFER, d.fbo);
        glViewport(0, 0, W, H);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Paint the titlebar band now; the app's viewport sits below it so
        // nothing overdraws.
        const int bar_x = sh * d.scale;
        const int bar_w = d.width * d.scale;
        const int bar_h = TITLEBAR_H * d.scale;
        const int y_top = H - (sh * d.scale);
        const int bar_y = y_top - bar_h;
        glEnable(GL_SCISSOR_TEST);
        glScissor(bar_x, bar_y, bar_w, bar_h);
        glClearColor(TITLEBAR_R, TITLEBAR_G, TITLEBAR_B, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
#endif
}

void GlfwWindow::SwapBuffers() {
#ifdef HAVE_WAYLAND
    auto& d = *impl_;
    if (!d.egl_ready) return;
    if (d.use_csd) compose_frame(d);
    eglSwapBuffers(d.egl_display, d.egl_surface);
#endif
}

int GlfwWindow::Width() const { return impl_->width * impl_->scale; }
int GlfwWindow::Height() const {
    int h = impl_->height * impl_->scale;
    if (impl_->use_csd) h -= TITLEBAR_H * impl_->scale;
    return h < 1 ? 1 : h;
}
float GlfwWindow::Scale() const { return (float)impl_->scale; }

int GlfwWindow::ViewportX() const {
    return wl_eff_shadow(*impl_) * impl_->scale;
}

int GlfwWindow::ViewportY() const {
    // Bottom-left origin: the content rect sits `sh * scale` above the
    // buffer floor (the bottom shadow row stays empty and is drawn as
    // shadow by the compose pass).
    return wl_eff_shadow(*impl_) * impl_->scale;
}

int GlfwWindow::FramebufferH() const {
    const int sh = wl_eff_shadow(*impl_);
    return (impl_->height + 2 * sh) * impl_->scale;
}

void GlfwWindow::SetClipboard(const std::string&) {
    // TODO: wl_data_device / X11 selection
}

void GlfwWindow::Minimize() {
#ifdef HAVE_WAYLAND
    if (impl_->toplevel) xdg_toplevel_set_minimized(impl_->toplevel);
#endif
}

void GlfwWindow::ToggleMaximize() {
#ifdef HAVE_WAYLAND
    if (!impl_->toplevel) return;
    if (impl_->maximized) xdg_toplevel_unset_maximized(impl_->toplevel);
    else                  xdg_toplevel_set_maximized(impl_->toplevel);
#endif
}

bool GlfwWindow::IsMaximized() const { return impl_->maximized; }

void GlfwWindow::SetTitlebarConfigPx(int height, const RectI& closeBtn) {
    impl_->titlebar_h = height;
    impl_->close_btn = closeBtn;
}

void GlfwWindow::PostEmptyEvent() {
    wake_event_loop();
}
