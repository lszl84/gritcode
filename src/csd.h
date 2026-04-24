// Gritcode — Client-side decorations (Wayland CSD)
// Shadow + rounded corners + titlebar + close button, rendered via three
// FBO passes ending in an SDF compose shader that applies a soft drop-shadow
// and clips to a rounded-rect. Pattern ported from sibling project tt.

#pragma once
#include <GL/gl.h>
#include <cstdint>

class CsdCompositor {
public:
    bool Init();
    void Shutdown();

    struct Frame {
        int width  = 0;  // content width in pixels (scale applied)
        int height = 0;  // content height in pixels (scale applied)
    };
    // Bind the content FBO and return its pixel dimensions. Call before the
    // app issues any draw calls for the frame.
    Frame BeginFrame(int windowW, int windowH, int scale);

    // Compose titlebar + content + close button into the main FBO, then
    // run the shadow/corner shader onto the default framebuffer. `floating`
    // controls whether the drop-shadow + rounded corners are active — they
    // collapse to zero when the window is tiled / maximized / fullscreen.
    void EndFrame(int windowW, int windowH, int scale, bool floating);

    // Hit tests. x/y are in logical surface coords (i.e. with the window's
    // origin at (0,0), titlebar occupying y < TITLEBAR_H).
    bool  InCloseButton(int x, int y, int windowW) const;
    bool  InTitlebar(int x, int y) const;
    // Returns xdg_toplevel resize-edge value (0 = none, 1..10 per xdg-shell).
    uint32_t ResizeEdge(int x, int y, int windowW, int windowH) const;
    // CSS-style cursor name for a given resize edge (e.g. "nw-resize").
    // Returns nullptr when edge is 0.
    static const char* CursorNameForEdge(uint32_t edge);

    void SetCloseHover(bool hover)     { closeHover_ = hover; }
    void SetClosePressed(bool pressed) { closePressed_ = pressed; }
    bool CloseHover() const            { return closeHover_; }

    static constexpr int TITLEBAR_H     = 40;
    static constexpr int SHADOW_EXTENT  = 32;   // logical px of shadow on each side
    static constexpr int CORNER_RADIUS  = 12;   // logical px, only while floating
    static constexpr int BORDER_GRAB    = 8;    // logical px hit-zone for edge drag
    static constexpr int CLOSE_SIZE     = 24;
    static constexpr int CLOSE_MARGIN   = 8;

private:
    // Pass 1 output — app content (no titlebar).
    GLuint contentFbo_ = 0, contentTex_ = 0;
    int    contentFboW_ = 0, contentFboH_ = 0;

    // Pass 2 output — window-sized composite (content + titlebar + close).
    GLuint mainFbo_ = 0, mainTex_ = 0;
    int    mainFboW_ = 0, mainFboH_ = 0;

    // Close button shader.
    GLuint closeProg_ = 0, closeVao_ = 0, closeVbo_ = 0;
    GLint  uCloseRect_ = -1, uCloseScreen_ = -1, uCloseHover_ = -1,
           uClosePressed_ = -1, uCloseBarColor_ = -1;

    // Shadow + rounded-corner compose shader.
    GLuint composeProg_ = 0, quadVao_ = 0, quadVbo_ = 0;
    GLint  uCScreen_ = -1, uCWindow_ = -1, uCRadius_ = -1,
           uCSigma_ = -1, uCShadow_ = -1, uCOutline_ = -1, uCTex_ = -1;

    float closeHoverAmt_ = 0.0f;
    bool  closeHover_ = false;
    bool  closePressed_ = false;

    bool CompileShaders();
    void EnsureFbo(GLuint* fbo, GLuint* tex, int* curW, int* curH, int w, int h);
    void DrawCloseButton(int mainW, int mainH, int scale);
};
