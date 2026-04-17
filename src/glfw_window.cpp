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

#include "glfw_window.h"
#include "types.h"
#include <cstdio>

#ifdef GRIT_MACOS
extern "C" void MacStyleWindowChrome(GLFWwindow* gw, float r, float g, float b);
#endif

GlfwWindow::GlfwWindow() = default;

GlfwWindow::~GlfwWindow() {
    if (window_) glfwDestroyWindow(window_);
    glfwTerminate();
}

bool GlfwWindow::Init(int width, int height, const char* title) {
    if (!glfwInit()) {
        fprintf(stderr, "GLFW init failed\n");
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // Don't show until first frame
#ifdef GRIT_LINUX
    glfwWindowHintString(GLFW_X11_CLASS_NAME, "grit");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, "grit");
#ifdef GLFW_WAYLAND_APP_ID
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, "grit");
#endif
#endif

    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window_) {
        fprintf(stderr, "GLFW window creation failed\n");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    // Don't use swap interval 1 — on Wayland, SwapBuffers blocks
    // indefinitely waiting for frame callbacks on invisible surfaces
    // (non-focused workspaces). We throttle via WaitEvents instead.
    glfwSwapInterval(0);

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, FramebufferSizeCb);
    glfwSetWindowContentScaleCallback(window_, WindowContentScaleCb);
    glfwSetMouseButtonCallback(window_, MouseButtonCb);
    glfwSetCursorPosCallback(window_, CursorPosCb);
    glfwSetScrollCallback(window_, ScrollCallbackCb);
    glfwSetKeyCallback(window_, KeyCallbackCb);
    glfwSetCharCallback(window_, CharCallbackCb);

    glfwGetWindowSize(window_, &winW_, &winH_);
    glfwGetFramebufferSize(window_, &fbW_, &fbH_);
    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(window_, &sx, &sy);
    contentScale_ = sx;
    UpdateScale();

#ifdef GRIT_MACOS
    // Match the title bar to the GL clear color in gl_renderer.cpp so the
    // window reads as one surface, Terminal.app-style.
    MacStyleWindowChrome(window_, 0.12f, 0.12f, 0.13f);
#endif

    return true;
}

void GlfwWindow::UpdateScale() {
    // bufferScale_ converts GLFW's logical cursor/window coords into
    // framebuffer pixels. On X11 winW == fbW so this stays at 1; on
    // Wayland / macOS it jumps to the compositor buffer scale.
    bufferScale_ = (winW_ > 0) ? (float)fbW_ / winW_ : 1.0f;
}

void GlfwWindow::Show() { glfwShowWindow(window_); }
void GlfwWindow::SetTitle(const char* title) { glfwSetWindowTitle(window_, title); }

bool GlfwWindow::ShouldClose() const {
    return glfwWindowShouldClose(window_);
}

void GlfwWindow::PollEvents() { glfwPollEvents(); }
void GlfwWindow::WaitEvents() { glfwWaitEventsTimeout(0.5); }  // Wake periodically for bg events
void GlfwWindow::SwapBuffers() { glfwSwapBuffers(window_); }

void GlfwWindow::SetClipboard(const std::string& text) {
    glfwSetClipboardString(window_, text.c_str());
}

// --- Callbacks ---

void GlfwWindow::FramebufferSizeCb(GLFWwindow* win, int w, int h) {
    auto* self = (GlfwWindow*)glfwGetWindowUserPointer(win);
    self->fbW_ = w;
    self->fbH_ = h;
    glfwGetWindowSize(win, &self->winW_, &self->winH_);
    self->UpdateScale();
    if (self->resizeCb_) self->resizeCb_(w, h, self->contentScale_);
}

void GlfwWindow::WindowContentScaleCb(GLFWwindow* win, float xs, float /*ys*/) {
    // Fires when the window moves to a monitor with a different content
    // scale (X11 + XRandR reconfig, Wayland per-monitor scale, macOS
    // main→external 2x→1x). Re-issue the resize callback so scroll_view
    // and widgets rebuild at the new DPI.
    auto* self = (GlfwWindow*)glfwGetWindowUserPointer(win);
    self->contentScale_ = xs;
    if (self->resizeCb_) self->resizeCb_(self->fbW_, self->fbH_, self->contentScale_);
}

void GlfwWindow::MouseButtonCb(GLFWwindow* win, int button, int action, int mods) {
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    auto* self = (GlfwWindow*)glfwGetWindowUserPointer(win);
    bool pressed = (action == GLFW_PRESS);
    self->leftDown_ = pressed;
    bool shift = (mods & GLFW_MOD_SHIFT);
    float px = (float)self->mouseX_ * self->bufferScale_;
    float py = (float)self->mouseY_ * self->bufferScale_;
    if (self->mouseBtnCb_) self->mouseBtnCb_(px, py, pressed, shift);
}

void GlfwWindow::CursorPosCb(GLFWwindow* win, double x, double y) {
    auto* self = (GlfwWindow*)glfwGetWindowUserPointer(win);
    self->mouseX_ = x;
    self->mouseY_ = y;
    float px = (float)x * self->bufferScale_;
    float py = (float)y * self->bufferScale_;
    if (self->mouseMoveCb_) self->mouseMoveCb_(px, py, self->leftDown_);
}

void GlfwWindow::ScrollCallbackCb(GLFWwindow* win, double, double yoff) {
    auto* self = (GlfwWindow*)glfwGetWindowUserPointer(win);
    if (self->scrollCb_) self->scrollCb_((float)(yoff));
}

void GlfwWindow::KeyCallbackCb(GLFWwindow* win, int key, int, int action, int mods) {
    auto* self = (GlfwWindow*)glfwGetWindowUserPointer(win);
    if (!self->keyCb_) return;
    bool pressed = (action == GLFW_PRESS || action == GLFW_REPEAT);

    // Map GLFW keys to xkb-like keysyms for compatibility with existing code
    int sym = 0;
    switch (key) {
    case GLFW_KEY_ESCAPE:    sym = 0xff1b; break;
    case GLFW_KEY_BACKSPACE: sym = 0xff08; break;
    case GLFW_KEY_DELETE:    sym = 0xffff; break;
    case GLFW_KEY_ENTER:     sym = 0xff0d; break;
    case GLFW_KEY_LEFT:      sym = 0xff51; break;
    case GLFW_KEY_RIGHT:     sym = 0xff53; break;
    case GLFW_KEY_UP:        sym = 0xff52; break;
    case GLFW_KEY_DOWN:      sym = 0xff54; break;
    case GLFW_KEY_HOME:      sym = 0xff50; break;
    case GLFW_KEY_END:       sym = 0xff57; break;
    case GLFW_KEY_PAGE_UP:   sym = 0xff55; break;
    case GLFW_KEY_PAGE_DOWN: sym = 0xff56; break;
    case GLFW_KEY_SPACE:     sym = 0x20; break;
    case GLFW_KEY_A:         sym = Key::A; break;
    case GLFW_KEY_C:         sym = Key::C; break;
    case GLFW_KEY_V:         sym = 'V'; break;
    default: break;
    }

    int xmods = 0;
    if (mods & GLFW_MOD_CONTROL) xmods |= Mod::Ctrl;
#ifdef __APPLE__
    if (mods & GLFW_MOD_SUPER) xmods |= Mod::Ctrl;  // Cmd acts as Ctrl on macOS
    self->superHeld_ = (mods & GLFW_MOD_SUPER) != 0;
#endif
    if (mods & GLFW_MOD_SHIFT) xmods |= Mod::Shift;

    if (sym && pressed) self->keyCb_(sym, xmods, true);
}

void GlfwWindow::CharCallbackCb(GLFWwindow* win, unsigned int codepoint) {
    auto* self = (GlfwWindow*)glfwGetWindowUserPointer(win);
    // On macOS, Cmd+key generates both key and char events — suppress the char
    if (self->superHeld_) return;
    if (self->charCb_) self->charCb_(codepoint);
}
