#include "glfw_window.h"
#include <cstdio>

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

    window_ = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!window_) {
        fprintf(stderr, "GLFW window creation failed\n");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);  // VSync

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, FramebufferSizeCb);
    glfwSetMouseButtonCallback(window_, MouseButtonCb);
    glfwSetCursorPosCallback(window_, CursorPosCb);
    glfwSetScrollCallback(window_, ScrollCallbackCb);
    glfwSetKeyCallback(window_, KeyCallbackCb);
    glfwSetCharCallback(window_, CharCallbackCb);

    glfwGetWindowSize(window_, &winW_, &winH_);
    glfwGetFramebufferSize(window_, &fbW_, &fbH_);
    UpdateScale();

    return true;
}

void GlfwWindow::UpdateScale() {
    scale_ = (winW_ > 0) ? (float)fbW_ / winW_ : 1.0f;
}

bool GlfwWindow::ShouldClose() const {
    return glfwWindowShouldClose(window_);
}

void GlfwWindow::PollEvents() { glfwPollEvents(); }
void GlfwWindow::WaitEvents() { glfwWaitEvents(); }
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
    if (self->resizeCb_) self->resizeCb_(w, h, self->scale_);
}

void GlfwWindow::MouseButtonCb(GLFWwindow* win, int button, int action, int mods) {
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;
    auto* self = (GlfwWindow*)glfwGetWindowUserPointer(win);
    bool pressed = (action == GLFW_PRESS);
    self->leftDown_ = pressed;
    bool shift = (mods & GLFW_MOD_SHIFT);
    float px = (float)self->mouseX_ * self->scale_;
    float py = (float)self->mouseY_ * self->scale_;
    if (self->mouseBtnCb_) self->mouseBtnCb_(px, py, pressed, shift);
}

void GlfwWindow::CursorPosCb(GLFWwindow* win, double x, double y) {
    auto* self = (GlfwWindow*)glfwGetWindowUserPointer(win);
    self->mouseX_ = x;
    self->mouseY_ = y;
    float px = (float)x * self->scale_;
    float py = (float)y * self->scale_;
    if (self->mouseMoveCb_) self->mouseMoveCb_(px, py, self->leftDown_);
}

void GlfwWindow::ScrollCallbackCb(GLFWwindow* win, double, double yoff) {
    auto* self = (GlfwWindow*)glfwGetWindowUserPointer(win);
    if (self->scrollCb_) self->scrollCb_((float)(-yoff * 20));
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
    case GLFW_KEY_A:         sym = (mods & GLFW_MOD_SHIFT) ? 0x41 : 0x61; break;
    case GLFW_KEY_C:         sym = (mods & GLFW_MOD_SHIFT) ? 0x43 : 0x63; break;
    default: break;
    }

    int xmods = 0;
    if (mods & GLFW_MOD_CONTROL) xmods |= 4;  // MOD_CTRL
    if (mods & GLFW_MOD_SHIFT) xmods |= 1;    // MOD_SHIFT

    if (sym && pressed) self->keyCb_(sym, xmods, true);
}

void GlfwWindow::CharCallbackCb(GLFWwindow* win, unsigned int codepoint) {
    auto* self = (GlfwWindow*)glfwGetWindowUserPointer(win);
    if (self->charCb_) self->charCb_(codepoint);
}
