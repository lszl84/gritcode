// macOS window chrome: make the title bar blend into the app background
// the way Terminal.app does. GLFW doesn't expose this so we reach into
// the NSWindow directly after window creation.

#import <Cocoa/Cocoa.h>

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

extern "C" void MacStyleWindowChrome(GLFWwindow* gw, float r, float g, float b) {
    if (!gw) return;
    NSWindow* nsw = glfwGetCocoaWindow(gw);
    if (!nsw) return;

    nsw.titlebarAppearsTransparent = YES;
    nsw.styleMask |= NSWindowStyleMaskFullSizeContentView;
    nsw.backgroundColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];

    // Dark appearance so the traffic-light glyphs render with the right
    // contrast against our dark background instead of faded-grey.
    nsw.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
}
