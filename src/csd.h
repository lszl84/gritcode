// Gritcode — Client-side decorations (Wayland CSD)
// Extracted from tt's platform layer. Shadow + rounded corners + titlebar
// + close button, composited in a single pass.

#pragma once
#include <GL/gl.h>

class CsdCompositor {
public:
    bool Init();
    void Shutdown();

    // Call before app renders. Binds the content FBO and returns the
    // content dimensions (window minus titlebar).
    struct Frame {
        int width = 0;   // content width in pixels
        int height = 0;  // content height in pixels
    };
    Frame BeginFrame(int windowW, int windowH, int scale);

    // Call after app renders. Composites titlebar/shadow/close button
    // onto the default framebuffer and returns the full buffer size.
    void EndFrame(int windowW, int windowH, int scale);

    // Hit testing (in window logical coords, including titlebar)
    bool InCloseButton(int x, int y, int windowW, int scale) const;
    bool InTitlebar(int x, int y, int scale) const;
    uint32_t ResizeEdge(int x, int y, int windowW, int windowH) const;

    // Animation state
    void SetCloseHover(bool hover) { closeHover_ = hover; }
    void SetClosePressed(bool pressed) { closePressed_ = pressed; }
    bool CloseHover() const { return closeHover_; }

    static constexpr int TITLEBAR_H = 40;
    static constexpr int SHADOW_EXTENT = 32;
    static constexpr int CORNER_RADIUS = 12;

private:
    GLuint closeProg_ = 0, closeVao_ = 0, closeVbo_ = 0;
    GLint uRect_ = -1, uScreen_ = -1, uHover_ = -1, uPressed_ = -1, uBarColor_ = -1;

    GLuint composeProg_ = 0, quadVao_ = 0, quadVbo_ = 0;
    GLint cTex_ = -1, cScreen_ = -1, cWindow_ = -1, cRadius_ = -1,
          cSigma_ = -1, cShadow_ = -1, cOutline_ = -1;

    GLuint contentFbo_ = 0, contentTex_ = 0;
    int contentFboW_ = 0, contentFboH_ = 0;

    GLuint composeFbo_ = 0, composeTex_ = 0;
    int composeFboW_ = 0, composeFboH_ = 0;

    float closeHoverAmt_ = 0.0f;
    bool closeHover_ = false;
    bool closePressed_ = false;

    void EnsureContentFbo(int w, int h);
    void EnsureComposeFbo(int w, int h);
    void DrawCloseButton(int windowW, int windowH, int scale);
    bool CompileShaders();
};
