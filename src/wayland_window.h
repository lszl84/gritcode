// FastCode Native — GPU-rendered AI coding harness
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
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <functional>
#include <cstdint>
#include <string>

struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct wp_viewporter;
struct wp_viewport;
struct wp_fractional_scale_manager_v1;
struct wp_fractional_scale_v1;

class WaylandWindow {
public:
    WaylandWindow();
    ~WaylandWindow();

    bool Init(int width, int height, const char* title);
    bool ShouldClose() const { return shouldClose_; }
    void PollEvents();
    void WaitEvents();

    uint32_t* BeginFrame();
    void EndFrame();

    int Width() const { return physW_; }     // physical pixels (for rendering)
    int Height() const { return physH_; }
    int LogicalW() const { return logW_; }  // logical (for layout reference)
    int LogicalH() const { return logH_; }
    float Scale() const { return scale_; }

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

private:
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_keyboard* keyboard_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_wm_base* wmBase_ = nullptr;
    xdg_surface* xdgSurface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    wp_viewporter* viewporter_ = nullptr;
    wp_viewport* viewport_ = nullptr;
    wp_fractional_scale_manager_v1* fracScaleMgr_ = nullptr;
    wp_fractional_scale_v1* fracScale_ = nullptr;

    xkb_context* xkbCtx_ = nullptr;
    xkb_keymap* xkbKeymap_ = nullptr;
    xkb_state* xkbState_ = nullptr;

    wl_cursor_theme* cursorTheme_ = nullptr;
    wl_surface* cursorSurface_ = nullptr;
    wl_cursor* ibeamCursor_ = nullptr;
    uint32_t pointerSerial_ = 0;

    struct Buffer {
        wl_buffer* wlBuf = nullptr;
        uint32_t* pixels = nullptr;
        size_t size = 0;
        bool busy = false;
    };
    Buffer bufs_[2];
    int logW_ = 0, logH_ = 0;         // logical (surface) size
    int physW_ = 0, physH_ = 0;       // physical (buffer) size
    int pendingW_ = 0, pendingH_ = 0; // from xdg configure (logical)
    float scale_ = 1.0f;              // from fractional-scale or wl_output
    int pendingScale120_ = 120;        // fractional scale * 120
    bool configured_ = false;
    bool shouldClose_ = false;
    bool frameReady_ = true;

    float ptrX_ = 0, ptrY_ = 0;
    bool leftDown_ = false;
    uint32_t mods_ = 0;

    ResizeCb resizeCb_;
    MouseBtnCb mouseBtnCb_;
    MouseMoveCb mouseMoveCb_;
    ScrollCb scrollCb_;
    KeyCb keyCb_;
    CharCb charCb_;

    void CreateBuffers();
    void DestroyBuffers();
    Buffer* GetFreeBuffer();

public:
    // Wayland listener trampolines (public for C callback access)
    static void RegGlobal(void*, wl_registry*, uint32_t, const char*, uint32_t);
    static void RegGlobalRemove(void*, wl_registry*, uint32_t);
    static void WmBasePing(void*, xdg_wm_base*, uint32_t);
    static void XdgSurfConfigure(void*, xdg_surface*, uint32_t);
    static void ToplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*);
    static void ToplevelClose(void*, xdg_toplevel*);
    static void SeatCaps(void*, wl_seat*, uint32_t);
    static void SeatName(void*, wl_seat*, const char*);
    static void PtrEnter(void*, wl_pointer*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t);
    static void PtrLeave(void*, wl_pointer*, uint32_t, wl_surface*);
    static void PtrMotion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t);
    static void PtrButton(void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t);
    static void PtrAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t);
    static void PtrFrame(void*, wl_pointer*);
    static void PtrAxisSource(void*, wl_pointer*, uint32_t);
    static void PtrAxisStop(void*, wl_pointer*, uint32_t, uint32_t);
    static void PtrAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t);
    static void KbKeymap(void*, wl_keyboard*, uint32_t, int32_t, uint32_t);
    static void KbEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*);
    static void KbLeave(void*, wl_keyboard*, uint32_t, wl_surface*);
    static void KbKey(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t);
    static void KbModifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    static void KbRepeatInfo(void*, wl_keyboard*, int32_t, int32_t);
    static void BufRelease(void*, wl_buffer*);
    static void FrameDone(void*, wl_callback*, uint32_t);
    static void FracScalePreferred(void*, wp_fractional_scale_v1*, uint32_t);
    void ApplyScale();
};
