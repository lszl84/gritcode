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

    // Titlebar transparency alone gives us the Terminal.app look: the title
    // bar draws as part of the window background instead of the default
    // light grey, and the window still has a draggable title region and
    // normal content layout. Do NOT add NSWindowStyleMaskFullSizeContentView
    // — that extends the content view under the title bar, which in our
    // case causes GL to draw on top of the traffic-light buttons and
    // leaves nothing for the user to grab when dragging.
    nsw.titlebarAppearsTransparent = YES;
    nsw.backgroundColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];

    // Dark appearance so the traffic-light glyphs render with the right
    // contrast against our dark background instead of faded-grey.
    nsw.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
}
