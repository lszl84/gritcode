#include "wayland_window.h"
#include "types.h"
#include "xdg-shell-protocol.h"
#include "viewporter-protocol.h"
#include "fractional-scale-protocol.h"
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// --- Listener trampolines ---

static const wl_registry_listener regListener = {
    WaylandWindow::RegGlobal, WaylandWindow::RegGlobalRemove};
static const xdg_wm_base_listener wmBaseListener = {WaylandWindow::WmBasePing};
static const xdg_surface_listener xdgSurfListener = {WaylandWindow::XdgSurfConfigure};
static const xdg_toplevel_listener toplevelListener = {
    WaylandWindow::ToplevelConfigure, WaylandWindow::ToplevelClose,
    nullptr, nullptr}; // configure_bounds, wm_capabilities
static const wl_seat_listener seatListener = {WaylandWindow::SeatCaps, WaylandWindow::SeatName};

static const wl_pointer_listener ptrListener = {
    WaylandWindow::PtrEnter, WaylandWindow::PtrLeave, WaylandWindow::PtrMotion,
    WaylandWindow::PtrButton, WaylandWindow::PtrAxis, WaylandWindow::PtrFrame,
    WaylandWindow::PtrAxisSource, WaylandWindow::PtrAxisStop, WaylandWindow::PtrAxisDiscrete,
    nullptr}; // axis_value120

static const wl_keyboard_listener kbListener = {
    WaylandWindow::KbKeymap, WaylandWindow::KbEnter, WaylandWindow::KbLeave,
    WaylandWindow::KbKey, WaylandWindow::KbModifiers, WaylandWindow::KbRepeatInfo};

static const wl_buffer_listener bufListener = {WaylandWindow::BufRelease};
static const wl_callback_listener frameListener = {WaylandWindow::FrameDone};
static const wp_fractional_scale_v1_listener fracScaleListener = {WaylandWindow::FracScalePreferred};

// --- Implementation ---

WaylandWindow::WaylandWindow() = default;

WaylandWindow::~WaylandWindow() {
    DestroyBuffers();
    if (cursorSurface_) wl_surface_destroy(cursorSurface_);
    if (cursorTheme_) wl_cursor_theme_destroy(cursorTheme_);
    if (fracScale_) wp_fractional_scale_v1_destroy(fracScale_);
    if (viewport_) wp_viewport_destroy(viewport_);
    if (toplevel_) xdg_toplevel_destroy(toplevel_);
    if (xdgSurface_) xdg_surface_destroy(xdgSurface_);
    if (surface_) wl_surface_destroy(surface_);
    if (keyboard_) wl_keyboard_destroy(keyboard_);
    if (pointer_) wl_pointer_destroy(pointer_);
    if (wmBase_) xdg_wm_base_destroy(wmBase_);
    if (seat_) wl_seat_destroy(seat_);
    if (fracScaleMgr_) wp_fractional_scale_manager_v1_destroy(fracScaleMgr_);
    if (viewporter_) wp_viewporter_destroy(viewporter_);
    if (shm_) wl_shm_destroy(shm_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (xkbState_) xkb_state_unref(xkbState_);
    if (xkbKeymap_) xkb_keymap_unref(xkbKeymap_);
    if (xkbCtx_) xkb_context_unref(xkbCtx_);
    if (display_) { wl_display_flush(display_); wl_display_disconnect(display_); }
}

bool WaylandWindow::Init(int w, int h, const char* title) {
    logW_ = w; logH_ = h;
    pendingW_ = w; pendingH_ = h;

    display_ = wl_display_connect(nullptr);
    if (!display_) { fprintf(stderr, "Failed to connect to Wayland\n"); return false; }

    xkbCtx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &regListener, this);
    wl_display_roundtrip(display_);

    if (!compositor_ || !shm_ || !wmBase_) {
        fprintf(stderr, "Missing Wayland globals\n"); return false;
    }

    surface_ = wl_compositor_create_surface(compositor_);

    // Fractional scale: attach before first commit
    if (fracScaleMgr_) {
        fracScale_ = wp_fractional_scale_manager_v1_get_fractional_scale(fracScaleMgr_, surface_);
        wp_fractional_scale_v1_add_listener(fracScale_, &fracScaleListener, this);
    }
    if (viewporter_)
        viewport_ = wp_viewporter_get_viewport(viewporter_, surface_);

    xdgSurface_ = xdg_wm_base_get_xdg_surface(wmBase_, surface_);
    xdg_surface_add_listener(xdgSurface_, &xdgSurfListener, this);
    toplevel_ = xdg_surface_get_toplevel(xdgSurface_);
    xdg_toplevel_add_listener(toplevel_, &toplevelListener, this);
    xdg_toplevel_set_title(toplevel_, title);
    xdg_toplevel_set_app_id(toplevel_, "harfbuzzscroll");
    wl_surface_commit(surface_);

    // Wait for configure
    while (!configured_) wl_display_roundtrip(display_);
    // Roundtrips until fractional scale arrives (or give up after a few)
    for (int i = 0; i < 5 && pendingScale120_ == 120 && fracScale_; i++)
        wl_display_roundtrip(display_);

    ApplyScale();

    // Cursor
    int cursorSize = (int)(24 * scale_);
    cursorTheme_ = wl_cursor_theme_load(nullptr, cursorSize, shm_);
    if (cursorTheme_) {
        ibeamCursor_ = wl_cursor_theme_get_cursor(cursorTheme_, "text");
        cursorSurface_ = wl_compositor_create_surface(compositor_);
    }

    CreateBuffers();
    return true;
}

void WaylandWindow::ApplyScale() {
    scale_ = pendingScale120_ / 120.0f;
    physW_ = (int)(logW_ * scale_ + 0.5f);
    physH_ = (int)(logH_ * scale_ + 0.5f);
    if (viewport_)
        wp_viewport_set_destination(viewport_, logW_, logH_);
}

void WaylandWindow::CreateBuffers() {
    DestroyBuffers();
    if (physW_ <= 0 || physH_ <= 0) return;

    size_t stride = physW_ * 4;
    size_t bufSize = stride * physH_;
    size_t poolSize = bufSize * 2;

    int fd = memfd_create("harfbuzzscroll-shm", MFD_CLOEXEC);
    if (fd < 0) { perror("memfd_create"); return; }
    ftruncate(fd, poolSize);

    void* data = mmap(nullptr, poolSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { perror("mmap"); close(fd); return; }

    wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, poolSize);

    for (int i = 0; i < 2; i++) {
        bufs_[i].pixels = (uint32_t*)((uint8_t*)data + i * bufSize);
        bufs_[i].size = bufSize;
        bufs_[i].wlBuf = wl_shm_pool_create_buffer(pool, i * bufSize,
            physW_, physH_, stride, WL_SHM_FORMAT_ARGB8888);
        bufs_[i].busy = false;
        wl_buffer_add_listener(bufs_[i].wlBuf, &bufListener, &bufs_[i]);
    }

    wl_shm_pool_destroy(pool);
    close(fd);
}

void WaylandWindow::DestroyBuffers() {
    for (auto& b : bufs_) {
        if (b.wlBuf) wl_buffer_destroy(b.wlBuf);
        b.wlBuf = nullptr;
        b.pixels = nullptr;
        b.busy = false;
    }
}

WaylandWindow::Buffer* WaylandWindow::GetFreeBuffer() {
    for (auto& b : bufs_)
        if (!b.busy) return &b;
    return nullptr;
}

uint32_t* WaylandWindow::BeginFrame() {
    if (!frameReady_) return nullptr;  // Wait for compositor to be ready
    Buffer* buf = GetFreeBuffer();
    if (!buf) return nullptr;
    return buf->pixels;
}

void WaylandWindow::EndFrame() {
    Buffer* buf = GetFreeBuffer();
    if (!buf) return;
    buf->busy = true;

    wl_surface_attach(surface_, buf->wlBuf, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, physW_, physH_);

    wl_callback* cb = wl_surface_frame(surface_);
    wl_callback_add_listener(cb, &frameListener, this);

    wl_surface_commit(surface_);
    frameReady_ = false;
}

void WaylandWindow::PollEvents() {
    // Flush outgoing, then non-blocking read of incoming events
    wl_display_flush(display_);
    struct pollfd pfd = {wl_display_get_fd(display_), POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0)
        wl_display_dispatch(display_);
    else
        wl_display_dispatch_pending(display_);
}

void WaylandWindow::WaitEvents() {
    wl_display_dispatch(display_);
}

void WaylandWindow::SetClipboard(const std::string& text) {
    FILE* p = popen("wl-copy", "w");
    if (p) { fwrite(text.data(), 1, text.size(), p); pclose(p); }
}

// --- Registry ---

void WaylandWindow::RegGlobal(void* data, wl_registry* reg, uint32_t name,
                               const char* iface, uint32_t ver) {
    auto* w = (WaylandWindow*)data;
    if (!strcmp(iface, wl_compositor_interface.name))
        w->compositor_ = (wl_compositor*)wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        w->shm_ = (wl_shm*)wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        w->wmBase_ = (xdg_wm_base*)wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(w->wmBase_, &wmBaseListener, w);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        w->seat_ = (wl_seat*)wl_registry_bind(reg, name, &wl_seat_interface, 5);
        wl_seat_add_listener(w->seat_, &seatListener, w);
    } else if (!strcmp(iface, wp_viewporter_interface.name)) {
        w->viewporter_ = (wp_viewporter*)wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    } else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
        w->fracScaleMgr_ = (wp_fractional_scale_manager_v1*)wl_registry_bind(
            reg, name, &wp_fractional_scale_manager_v1_interface, 1);
    }
}

void WaylandWindow::RegGlobalRemove(void*, wl_registry*, uint32_t) {}

// --- xdg ---

void WaylandWindow::WmBasePing(void*, xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}

void WaylandWindow::XdgSurfConfigure(void* data, xdg_surface* surf, uint32_t serial) {
    auto* w = (WaylandWindow*)data;
    xdg_surface_ack_configure(surf, serial);

    if (w->pendingW_ > 0 && w->pendingH_ > 0) {
        bool changed = (w->pendingW_ != w->logW_ || w->pendingH_ != w->logH_);
        w->logW_ = w->pendingW_;
        w->logH_ = w->pendingH_;
        w->ApplyScale();
        if (changed && w->configured_) {
            w->CreateBuffers();
            if (w->resizeCb_) w->resizeCb_(w->physW_, w->physH_, w->scale_);
        }
    }
    w->configured_ = true;
}

void WaylandWindow::ToplevelConfigure(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array*) {
    auto* win = (WaylandWindow*)data;
    if (w > 0 && h > 0) { win->pendingW_ = w; win->pendingH_ = h; }
}

void WaylandWindow::ToplevelClose(void* data, xdg_toplevel*) {
    ((WaylandWindow*)data)->shouldClose_ = true;
}

// --- Seat ---

void WaylandWindow::SeatCaps(void* data, wl_seat* seat, uint32_t caps) {
    auto* w = (WaylandWindow*)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w->pointer_) {
        w->pointer_ = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(w->pointer_, &ptrListener, w);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w->keyboard_) {
        w->keyboard_ = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(w->keyboard_, &kbListener, w);
    }
}

void WaylandWindow::SeatName(void*, wl_seat*, const char*) {}

// --- Pointer ---

void WaylandWindow::PtrEnter(void* data, wl_pointer* ptr, uint32_t serial,
                              wl_surface*, wl_fixed_t x, wl_fixed_t y) {
    auto* w = (WaylandWindow*)data;
    w->pointerSerial_ = serial;
    w->ptrX_ = wl_fixed_to_double(x) * w->scale_;
    w->ptrY_ = wl_fixed_to_double(y) * w->scale_;
    // Set cursor
    if (w->ibeamCursor_ && w->ibeamCursor_->image_count > 0) {
        wl_cursor_image* img = w->ibeamCursor_->images[0];
        wl_buffer* buf = wl_cursor_image_get_buffer(img);
        wl_surface_attach(w->cursorSurface_, buf, 0, 0);
        wl_surface_damage(w->cursorSurface_, 0, 0, img->width, img->height);
        wl_surface_commit(w->cursorSurface_);
        wl_pointer_set_cursor(ptr, serial, w->cursorSurface_, img->hotspot_x, img->hotspot_y);
    }
}

void WaylandWindow::PtrLeave(void*, wl_pointer*, uint32_t, wl_surface*) {}

void WaylandWindow::PtrMotion(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    auto* w = (WaylandWindow*)data;
    w->ptrX_ = wl_fixed_to_double(x) * w->scale_;
    w->ptrY_ = wl_fixed_to_double(y) * w->scale_;
    if (w->mouseMoveCb_) w->mouseMoveCb_(w->ptrX_, w->ptrY_, w->leftDown_);
}

void WaylandWindow::PtrButton(void* data, wl_pointer*, uint32_t serial, uint32_t,
                               uint32_t button, uint32_t state) {
    auto* w = (WaylandWindow*)data;
    if (button == 0x110) { // BTN_LEFT
        bool pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);
        w->leftDown_ = pressed;
        bool shift = (w->mods_ & Mod::Shift) != 0;
        if (w->mouseBtnCb_) w->mouseBtnCb_(w->ptrX_, w->ptrY_, pressed, shift);
    }
}

void WaylandWindow::PtrAxis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    auto* w = (WaylandWindow*)data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && w->scrollCb_) {
        float delta = -wl_fixed_to_double(value) / 10.0f;
        w->scrollCb_(delta);
    }
}

void WaylandWindow::PtrFrame(void*, wl_pointer*) {}
void WaylandWindow::PtrAxisSource(void*, wl_pointer*, uint32_t) {}
void WaylandWindow::PtrAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
void WaylandWindow::PtrAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}

// --- Keyboard ---

void WaylandWindow::KbKeymap(void* data, wl_keyboard*, uint32_t fmt, int32_t fd, uint32_t sz) {
    auto* w = (WaylandWindow*)data;
    if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }
    char* map = (char*)mmap(nullptr, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { close(fd); return; }

    if (w->xkbKeymap_) xkb_keymap_unref(w->xkbKeymap_);
    if (w->xkbState_) xkb_state_unref(w->xkbState_);
    w->xkbKeymap_ = xkb_keymap_new_from_string(w->xkbCtx_, map,
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    w->xkbState_ = xkb_state_new(w->xkbKeymap_);
    munmap(map, sz);
    close(fd);
}

void WaylandWindow::KbEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void WaylandWindow::KbLeave(void*, wl_keyboard*, uint32_t, wl_surface*) {}

void WaylandWindow::KbKey(void* data, wl_keyboard*, uint32_t, uint32_t,
                           uint32_t keycode, uint32_t state) {
    auto* w = (WaylandWindow*)data;
    if (!w->xkbState_) return;
    bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);

    xkb_keysym_t sym = xkb_state_key_get_one_sym(w->xkbState_, keycode + 8);

    // Pass raw keysym for special keys (backspace, enter, arrows, etc.)
    if (w->keyCb_ && pressed) {
        int key = 0;
        switch (sym) {
        case XKB_KEY_Escape: key = Key::Escape; break;
        case XKB_KEY_space: key = Key::Space; break;
        case XKB_KEY_Up: key = Key::Up; break;
        case XKB_KEY_Down: key = Key::Down; break;
        case XKB_KEY_Prior: key = Key::PageUp; break;
        case XKB_KEY_Next: key = Key::PageDown; break;
        case XKB_KEY_Home: key = Key::Home; break;
        case XKB_KEY_End: key = Key::End; break;
        case XKB_KEY_a: case XKB_KEY_A: key = Key::A; break;
        case XKB_KEY_c: case XKB_KEY_C: key = Key::C; break;
        default: break;
        }
        if (key) w->keyCb_(key, w->mods_, pressed);

        // Also pass raw keysym for text input (backspace, delete, enter, arrows)
        w->keyCb_(sym, w->mods_, pressed);
    }

    // Character input: get UTF-32 codepoint for printable characters
    if (pressed && w->charCb_) {
        uint32_t cp = xkb_state_key_get_utf32(w->xkbState_, keycode + 8);
        if (cp >= 32 && cp != 127) {  // printable, not DEL
            w->charCb_(cp);
        }
    }
}

void WaylandWindow::KbModifiers(void* data, wl_keyboard*, uint32_t,
                                 uint32_t depressed, uint32_t latched,
                                 uint32_t locked, uint32_t group) {
    auto* w = (WaylandWindow*)data;
    if (!w->xkbState_) return;
    xkb_state_update_mask(w->xkbState_, depressed, latched, locked, 0, 0, group);

    w->mods_ = 0;
    if (xkb_state_mod_name_is_active(w->xkbState_, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE))
        w->mods_ |= Mod::Ctrl;
    if (xkb_state_mod_name_is_active(w->xkbState_, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE))
        w->mods_ |= Mod::Shift;
}

void WaylandWindow::KbRepeatInfo(void*, wl_keyboard*, int32_t, int32_t) {}

// --- Buffer / Frame ---

void WaylandWindow::BufRelease(void* data, wl_buffer*) {
    ((Buffer*)data)->busy = false;
}

void WaylandWindow::FrameDone(void* data, wl_callback* cb, uint32_t) {
    wl_callback_destroy(cb);
    ((WaylandWindow*)data)->frameReady_ = true;
}

void WaylandWindow::FracScalePreferred(void* data, wp_fractional_scale_v1*, uint32_t scale120) {
    auto* w = (WaylandWindow*)data;
    w->pendingScale120_ = (int)scale120;
    w->ApplyScale();
    w->CreateBuffers();
    if (w->resizeCb_) w->resizeCb_(w->physW_, w->physH_, w->scale_);
}
