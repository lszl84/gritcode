// Native window backend for Gritcode — direct Wayland (primary) and X11 (fallback).
// Smooth resize via frame callbacks (Wayland) and _NET_WM_SYNC_REQUEST (X11).
// No GLFW dependency.
//
// Copyright (C) 2026 luke@devmindscape.com
// SPDX-License-Identifier: GPL-3.0-or-later

#include "window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Stub implementation — will be replaced with full Wayland/X11 backends.
// For now, fails gracefully so the build compiles and we can iterate.
// ---------------------------------------------------------------------------

GlfwWindow::GlfwWindow() = default;
GlfwWindow::~GlfwWindow() = default;

bool GlfwWindow::Init(int, int, const char*) {
    std::fprintf(stderr, "grit: native window backend not yet implemented\n");
    return false;
}
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