// Gritcode — Platform-native window (macOS).
//
// Implements AppWindow using NSWindow + NSOpenGLView.  The app's Run() loop
// remains in control: we pull NSEvents one at a time (or block/timeout) and
// dispatch them to the responder chain.  The NSOpenGLView subclass forwards
// input events to the C++ callbacks stored on its instance.
//
// macOS-specific conveniences:
//   * Transparent titlebar, dark appearance (the look the user expects from
//     Terminal.app) — see setupTitlebar.
//   * NSPasteboard for the clipboard.
//
// Copyright (C) 2026 luke@devmindscape.com. GPL v3.

#ifndef __APPLE__
#error "This file is macOS only"
#endif

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>

#include "window.h"
#include "keysyms.h"

#include <string>

static constexpr int MOD_CTRL  = 1;
static constexpr int MOD_SHIFT = 2;
static constexpr int MOD_ALT   = 4;
static constexpr int MOD_SUPER = 8;

// Map NSEvent.keyCode (Mac virtual keys) → xkb-style keysyms used throughout
// the app (so `Key::Escape`, `XKB_KEY_Left`, etc. keep working unchanged).
static int MapMacKey(unsigned short vk) {
    switch (vk) {
    case 0x35: return 0xff1b; // Escape → XKB_KEY_Escape
    case 0x24: return 0xff0d; // Return
    case 0x4C: return 0xff0d; // Numpad Enter
    case 0x30: return 0x09;   // Tab
    case 0x33: return 0xff08; // Delete (backspace) → XKB_KEY_BackSpace
    case 0x75: return 0xffff; // Fwd Delete → XKB_KEY_Delete
    case 0x7B: return 0xff51; // Left
    case 0x7C: return 0xff53; // Right
    case 0x7E: return 0xff52; // Up
    case 0x7D: return 0xff54; // Down
    case 0x73: return 0xff50; // Home
    case 0x77: return 0xff57; // End
    case 0x74: return 0xff55; // PageUp
    case 0x79: return 0xff56; // PageDown
    case 0x31: return ' ';
    default:   return 0;
    }
}

static int MacMods(NSEventModifierFlags f) {
    int m = 0;
    if (f & NSEventModifierFlagShift)   m |= MOD_SHIFT;
    if (f & NSEventModifierFlagControl) m |= MOD_CTRL;
    if (f & NSEventModifierFlagOption)  m |= MOD_ALT;
    // Apple-Command is the natural shortcut key on macOS — map it to the same
    // slot as Ctrl so Cmd+C / Cmd+V / Cmd+A work like users expect.  Control
    // stays as Control for embedded-terminal-style bindings if anyone uses
    // them.
    if (f & NSEventModifierFlagCommand) m |= MOD_CTRL;
    if (f & NSEventModifierFlagCommand) m |= MOD_SUPER;
    return m;
}

// ---- ObjC classes ---------------------------------------------------------

@class GritView;
@class GritWindowDelegate;

struct MacState {
    NSWindow* window = nil;
    GritView* view = nil;
    GritWindowDelegate* delegate = nil;

    int logicalW = 1000;
    int logicalH = 750;
    int bufferW = 1000;
    int bufferH = 750;
    float scale = 1.0f;

    bool shouldClose = false;

    AppWindow::ResizeCb    resizeCb;
    AppWindow::MouseBtnCb  mouseBtnCb;
    AppWindow::MouseMoveCb mouseMoveCb;
    AppWindow::ScrollCb    scrollCb;
    AppWindow::KeyCb       keyCb;
    AppWindow::CharCb      charCb;
};

@interface GritView : NSOpenGLView {
@public
    MacState* st_;
    BOOL trackingInstalled_;
}
- (id)initWithFrame:(NSRect)frame state:(MacState*)st;
@end

@interface GritWindowDelegate : NSObject<NSWindowDelegate> {
@public
    MacState* st_;
}
@end

@implementation GritWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    st_->shouldClose = true;
    return NO;  // don't actually close; let app's main loop exit cleanly
}
- (void)windowDidResize:(NSNotification*)n {
    if (!st_->view) return;
    NSSize logical = [st_->view bounds].size;
    NSSize backing = [st_->view convertRectToBacking:[st_->view bounds]].size;
    st_->logicalW = (int)logical.width;
    st_->logicalH = (int)logical.height;
    st_->bufferW  = (int)backing.width;
    st_->bufferH  = (int)backing.height;
    if (st_->resizeCb) st_->resizeCb(st_->bufferW, st_->bufferH, st_->scale);
}
- (void)windowDidChangeBackingProperties:(NSNotification*)n {
    if (!st_->window) return;
    st_->scale = (float)[st_->window backingScaleFactor];
    NSSize backing = [st_->view convertRectToBacking:[st_->view bounds]].size;
    st_->bufferW = (int)backing.width;
    st_->bufferH = (int)backing.height;
    if (st_->resizeCb) st_->resizeCb(st_->bufferW, st_->bufferH, st_->scale);
}
@end

@implementation GritView
- (id)initWithFrame:(NSRect)frame state:(MacState*)st {
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
        NSOpenGLPFAColorSize,     24,
        NSOpenGLPFAAlphaSize,     8,
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAAccelerated,
        0
    };
    NSOpenGLPixelFormat* fmt = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!fmt) {
        NSLog(@"[grit] failed to create NSOpenGLPixelFormat");
        return nil;
    }
    if ((self = [super initWithFrame:frame pixelFormat:fmt])) {
        st_ = st;
        trackingInstalled_ = NO;
        [self setWantsBestResolutionOpenGLSurface:YES];
    }
    return self;
}
- (BOOL)isFlipped        { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)canBecomeKeyView      { return YES; }

- (void)prepareOpenGL {
    [super prepareOpenGL];
    GLint zero = 0;
    [[self openGLContext] setValues:&zero forParameter:NSOpenGLContextParameterSwapInterval];
    [[self openGLContext] makeCurrentContext];
}

- (void)reshape {
    [super reshape];
    [[self openGLContext] update];
    NSSize logical = [self bounds].size;
    NSSize backing = [self convertRectToBacking:[self bounds]].size;
    st_->logicalW = (int)logical.width;
    st_->logicalH = (int)logical.height;
    st_->bufferW  = (int)backing.width;
    st_->bufferH  = (int)backing.height;
    st_->scale    = (float)[[self window] backingScaleFactor];
    if (st_->resizeCb) st_->resizeCb(st_->bufferW, st_->bufferH, st_->scale);
}

- (void)updateTrackingAreas {
    if (trackingInstalled_) {
        for (NSTrackingArea* a in [self trackingAreas]) [self removeTrackingArea:a];
    }
    NSTrackingAreaOptions opts = NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                                 NSTrackingActiveInKeyWindow     | NSTrackingInVisibleRect;
    NSTrackingArea* area = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                                         options:opts owner:self userInfo:nil];
    [self addTrackingArea:area];
    trackingInstalled_ = YES;
    [super updateTrackingAreas];
}

- (NSPoint)bufferPointFromEvent:(NSEvent*)e {
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    // Convert logical → backing pixels (HiDPI).
    NSPoint bp = [self convertPointToBacking:p];
    // convertPointToBacking keeps Y origin at top since view isFlipped=YES,
    // but on flipped views AppKit actually flips back; safer to recompute.
    return NSMakePoint(p.x * st_->scale, p.y * st_->scale);
    (void)bp;
}

- (void)mouseDown:(NSEvent*)e {
    NSPoint p = [self bufferPointFromEvent:e];
    int mods = MacMods([e modifierFlags]);
    if (st_->mouseBtnCb) st_->mouseBtnCb((float)p.x, (float)p.y, true, (mods & MOD_SHIFT) != 0);
}
- (void)mouseUp:(NSEvent*)e {
    NSPoint p = [self bufferPointFromEvent:e];
    if (st_->mouseBtnCb) st_->mouseBtnCb((float)p.x, (float)p.y, false, false);
}
- (void)mouseMoved:(NSEvent*)e {
    NSPoint p = [self bufferPointFromEvent:e];
    if (st_->mouseMoveCb) st_->mouseMoveCb((float)p.x, (float)p.y, false);
}
- (void)mouseDragged:(NSEvent*)e {
    NSPoint p = [self bufferPointFromEvent:e];
    if (st_->mouseMoveCb) st_->mouseMoveCb((float)p.x, (float)p.y, true);
}
- (void)scrollWheel:(NSEvent*)e {
    CGFloat dy = [e scrollingDeltaY];
    if ([e hasPreciseScrollingDeltas]) {
        // Trackpad: divide to roughly match line-based scroll
        if (st_->scrollCb) st_->scrollCb((float)(dy / 10.0));
    } else {
        if (st_->scrollCb) st_->scrollCb((float)dy);
    }
}

- (void)keyDown:(NSEvent*)e {
    int mods = MacMods([e modifierFlags]);
    int mapped = MapMacKey([e keyCode]);
    if (mapped && st_->keyCb) st_->keyCb(mapped, mods, true);

    if (st_->charCb && !(mods & (MOD_CTRL | MOD_SUPER))) {
        NSString* chars = [e characters];
        for (NSUInteger i = 0; i < [chars length]; ) {
            unichar c0 = [chars characterAtIndex:i];
            uint32_t cp = 0; NSUInteger used = 1;
            if (c0 >= 0xD800 && c0 <= 0xDBFF && i + 1 < [chars length]) {
                unichar c1 = [chars characterAtIndex:i + 1];
                if (c1 >= 0xDC00 && c1 <= 0xDFFF) {
                    cp = 0x10000 + ((uint32_t)(c0 - 0xD800) << 10) + (uint32_t)(c1 - 0xDC00);
                    used = 2;
                }
            } else {
                cp = c0;
            }
            if (cp >= 0x20 && cp != 0x7F) st_->charCb(cp);
            i += used;
        }
    }

    if (mapped == 0 && st_->keyCb) {
        // Send a synthetic "ASCII key" so handlers like Cmd+A / Cmd+C work.
        NSString* chars = [e charactersIgnoringModifiers];
        if ([chars length] > 0) {
            unichar c = [chars characterAtIndex:0];
            if (c >= 'a' && c <= 'z') c = (unichar)(c - 32);  // uppercase for Key::A etc.
            if (c < 0x80) st_->keyCb((int)c, mods, true);
        }
    }
}
- (void)keyUp:(NSEvent*)e {
    int mods = MacMods([e modifierFlags]);
    int mapped = MapMacKey([e keyCode]);
    if (mapped && st_->keyCb) st_->keyCb(mapped, mods, false);
}

// Quiet the system beep when modifier-only keypresses (e.g. raw Cmd) go
// unanswered.
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    // Route Cmd-keys through our keyDown handler.
    if ([event modifierFlags] & NSEventModifierFlagCommand) {
        [self keyDown:event];
        return YES;
    }
    return NO;
}

- (void)drawRect:(NSRect)dirtyRect {
    // Intentionally empty — the app drives rendering via BeginContentFrame/
    // EndContentFrame from its own Run() loop.
    (void)dirtyRect;
}
@end

// ---- AppWindow pimpl ------------------------------------------------------

struct AppWindow::Impl {
    MacState* mac = nullptr;
};

AppWindow::AppWindow() : impl_(new Impl()) {}
AppWindow::~AppWindow() {
    if (impl_->mac) {
        [impl_->mac->window close];
        delete impl_->mac;
    }
    delete impl_;
}

bool AppWindow::Init(int width, int height, const char* title) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        auto* st = new MacState();
        st->logicalW = width;
        st->logicalH = height;

        NSRect frame = NSMakeRect(100, 100, width, height);
        NSWindowStyleMask mask = NSWindowStyleMaskTitled
                               | NSWindowStyleMaskClosable
                               | NSWindowStyleMaskMiniaturizable
                               | NSWindowStyleMaskResizable;
        NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
                                                    styleMask:mask
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        [win setTitle:[NSString stringWithUTF8String:title ? title : "Gritcode"]];
        [win setContentMinSize:NSMakeSize(480, 360)];
        [win center];
        [win setReleasedWhenClosed:NO];

        // Terminal.app-style chrome: transparent titlebar blends into the
        // window background, dark appearance for the traffic-light glyphs.
        win.titlebarAppearsTransparent = YES;
        win.backgroundColor = [NSColor colorWithSRGBRed:0.09 green:0.09 blue:0.11 alpha:1.0];
        win.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];

        GritWindowDelegate* del = [[GritWindowDelegate alloc] init];
        del->st_ = st;
        [win setDelegate:del];
        st->delegate = del;

        GritView* view = [[GritView alloc] initWithFrame:frame state:st];
        if (!view) { delete st; return false; }
        [win setContentView:view];
        [win makeFirstResponder:view];
        st->window = win;
        st->view = view;

        // Trigger GL context creation + initial reshape (sets scale / bufW / bufH).
        [view prepareOpenGL];
        [view reshape];
        st->scale = (float)[win backingScaleFactor];
        if (st->scale <= 0) st->scale = 1.0f;

        impl_->mac = st;
        return true;
    }
}

void AppWindow::Show() {
    if (!impl_->mac) return;
    @autoreleasepool {
        [impl_->mac->window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void AppWindow::SetTitle(const char* title) {
    if (!impl_->mac || !title) return;
    @autoreleasepool {
        [impl_->mac->window setTitle:[NSString stringWithUTF8String:title]];
    }
}

bool AppWindow::ShouldClose() const {
    return impl_->mac && impl_->mac->shouldClose;
}
void AppWindow::SetShouldClose(bool close) {
    if (impl_->mac) impl_->mac->shouldClose = close;
}

static void mac_pump(MacState* st, NSDate* until) {
    (void)st;
    @autoreleasepool {
        for (;;) {
            NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:until
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES];
            if (!ev) break;
            [NSApp sendEvent:ev];
            [NSApp updateWindows];
            // After the first event, drain any other queued events non-blockingly
            until = [NSDate distantPast];
        }
    }
}

void AppWindow::PollEvents() {
    if (!impl_->mac) return;
    mac_pump(impl_->mac, [NSDate distantPast]);
}
void AppWindow::WaitEvents() {
    if (!impl_->mac) return;
    mac_pump(impl_->mac, [NSDate distantFuture]);
}
void AppWindow::WaitEventsTimeout(double timeout) {
    if (!impl_->mac) return;
    mac_pump(impl_->mac, [NSDate dateWithTimeIntervalSinceNow:timeout]);
}

AppWindow::ContentFrame AppWindow::BeginContentFrame() {
    if (!impl_->mac) return {0, 0, 0, 0, 0};
    @autoreleasepool {
        [[impl_->mac->view openGLContext] makeCurrentContext];
        NSSize backing = [impl_->mac->view convertRectToBacking:[impl_->mac->view bounds]].size;
        impl_->mac->bufferW = (int)backing.width;
        impl_->mac->bufferH = (int)backing.height;
        glViewport(0, 0, impl_->mac->bufferW, impl_->mac->bufferH);
    }
    return { impl_->mac->bufferW, impl_->mac->bufferH,
             impl_->mac->logicalW, impl_->mac->logicalH, 0 };
}
void AppWindow::EndContentFrame() {
    if (!impl_->mac) return;
    @autoreleasepool {
        [[impl_->mac->view openGLContext] flushBuffer];
    }
}
void AppWindow::SwapBuffers() { EndContentFrame(); }

int AppWindow::Width() const      { return impl_->mac ? impl_->mac->bufferW : 0; }
int AppWindow::Height() const     { return impl_->mac ? impl_->mac->bufferH : 0; }
int AppWindow::FullWidth() const  { return impl_->mac ? impl_->mac->logicalW : 0; }
int AppWindow::FullHeight() const { return impl_->mac ? impl_->mac->logicalH : 0; }
int AppWindow::LogicalW() const   { return impl_->mac ? impl_->mac->logicalW : 0; }
int AppWindow::LogicalH() const   { return impl_->mac ? impl_->mac->logicalH : 0; }
float AppWindow::Scale() const    { return impl_->mac ? impl_->mac->scale : 1.0f; }

void AppWindow::OnResize(ResizeCb cb)          { if (impl_->mac) impl_->mac->resizeCb    = std::move(cb); }
void AppWindow::OnMouseButton(MouseBtnCb cb)   { if (impl_->mac) impl_->mac->mouseBtnCb  = std::move(cb); }
void AppWindow::OnMouseMove(MouseMoveCb cb)    { if (impl_->mac) impl_->mac->mouseMoveCb = std::move(cb); }
void AppWindow::OnScrollEvent(ScrollCb cb)     { if (impl_->mac) impl_->mac->scrollCb    = std::move(cb); }
void AppWindow::OnKeyEvent(KeyCb cb)           { if (impl_->mac) impl_->mac->keyCb       = std::move(cb); }
void AppWindow::OnCharEvent(CharCb cb)         { if (impl_->mac) impl_->mac->charCb      = std::move(cb); }

void AppWindow::SetClipboard(const std::string& text) {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        NSString* s = [[NSString alloc] initWithBytes:text.data()
                                               length:text.size()
                                             encoding:NSUTF8StringEncoding];
        if (s) [pb setString:s forType:NSPasteboardTypeString];
    }
}
std::string AppWindow::GetClipboard() {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        NSString* s = [pb stringForType:NSPasteboardTypeString];
        if (!s) return "";
        const char* u = [s UTF8String];
        return u ? std::string(u) : std::string();
    }
}

void* AppWindow::NativeHandle() const {
    return impl_->mac ? (__bridge void*)impl_->mac->window : nullptr;
}
