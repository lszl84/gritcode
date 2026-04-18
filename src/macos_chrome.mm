// macOS window chrome: make the title bar blend into the app background
// the way Terminal.app does. SDL3 exposes the underlying NSWindow via
// properties, so we reach into it directly after window creation.

#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>

extern "C" void MacStyleWindowChrome(void* sdlWindow, float r, float g, float b) {
    if (!sdlWindow) return;
    SDL_PropertiesID props = SDL_GetWindowProperties((SDL_Window*)sdlWindow);
    NSWindow* nsw = (__bridge NSWindow*)SDL_GetPointerProperty(props,
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (!nsw) return;

    nsw.titlebarAppearsTransparent = YES;
    nsw.backgroundColor = [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];
    nsw.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
}
