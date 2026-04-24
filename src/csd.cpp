// Gritcode — Client-side decorations for Wayland.
//
// Three-pass composition:
//   1. App renders into contentFbo (content area only, no titlebar).
//   2. We assemble a window-sized mainFbo: flat titlebar clear, content blit,
//      close button draw on top.
//   3. A compose shader writes the mainFbo into the default framebuffer,
//      which is shadow-padded on all four sides; the shader applies a
//      rounded-rect SDF clip (so corners go transparent) and a gaussian
//      drop shadow outside the rounded rect.
//
// Coordinate convention: GL y-up throughout. mainFbo's "top row" (high y)
// contains the titlebar; the content blit puts the content area below it.
// This matches the way the app's renderer is already oriented (we aren't
// flipping anything it wasn't flipping before).
//
// Output alpha is PRE-MULTIPLIED so it composites correctly against the
// compositor's wallpaper — shadow pixels are (0, 0, 0, α).

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "csd.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>

// -- tiny shader helpers ----------------------------------------------------

static bool compile_shader(GLuint sh, const char* src) {
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        fprintf(stderr, "[CSD] shader compile error: %s\n", log);
        return false;
    }
    return true;
}

static GLuint link_program(const char* vs_src, const char* fs_src) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    if (!compile_shader(vs, vs_src) || !compile_shader(fs, fs_src)) {
        glDeleteShader(vs); glDeleteShader(fs);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint linked = 0; glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fprintf(stderr, "[CSD] program link error: %s\n", log);
        glDeleteProgram(p); p = 0;
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// -- shaders ----------------------------------------------------------------

// Close button — a disc that lights up on hover with a rotated X inside.
static const char* kCloseVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec4 uRect;
uniform vec2 uScreen;
out vec2 vUV;
void main() {
    vec2 px  = uRect.xy + aPos * uRect.zw;
    vec2 ndc = vec2(px.x / uScreen.x, px.y / uScreen.y) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aPos;
}
)";

static const char* kCloseFS = R"(#version 330 core
in  vec2 vUV;
out vec4 fragColor;
uniform float uHover;
uniform float uPressed;
uniform vec3  uBarColor;
void main() {
    vec2 p = vUV * 2.0 - 1.0;
    float r = length(p);

    float disc = 1.0 - smoothstep(0.95, 1.0, r);

    float c = 0.70710678;
    vec2 q = vec2(c*p.x - c*p.y, c*p.x + c*p.y);
    float d1 = length(vec2(max(abs(q.x) - 0.34, 0.0), q.y)) - 0.055;
    float d2 = length(vec2(max(abs(q.y) - 0.34, 0.0), q.x)) - 0.055;
    float d = min(d1, d2);
    float xmask = 1.0 - smoothstep(0.0, 0.015, d);

    vec3 base   = vec3(0.245, 0.255, 0.295);
    vec3 hover  = vec3(0.320, 0.330, 0.370);
    vec3 press  = vec3(0.420, 0.430, 0.470);
    vec3 bg     = mix(base, hover, uHover);
    bg          = mix(bg, press, uPressed);
    vec3 disc_c = mix(bg, vec3(1.0), xmask);
    vec3 col    = mix(uBarColor, disc_c, disc);
    fragColor   = vec4(col, 1.0);
}
)";

// Full-screen compose: shadow + rounded corners over mainFbo.
// uScreen  = full buffer size (window + 2*shadow) * scale
// uWindow  = (shadowX*scale, shadowY*scale, winW*scale, winH*scale)
// uRadius  = corner radius in pixels (0 when tiled/maximized)
// uSigma   = shadow gaussian sigma in pixels
// uShadow  = shadow intensity (0 when not floating)
// uOutline = inner outline intensity (0 when not floating)
// uTex     = mainFbo (sized exactly uWindow.zw)
static const char* kComposeVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vPx;
uniform vec2 uScreen;
void main() {
    vec2 ndc = aPos * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vPx = aPos * uScreen;
}
)";

static const char* kComposeFS = R"(#version 330 core
in  vec2 vPx;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2  uScreen;
uniform vec4  uWindow;
uniform float uRadius;
uniform float uSigma;
uniform float uShadow;
uniform float uOutline;
void main() {
    vec2 center = uWindow.xy + uWindow.zw * 0.5;
    vec2 halfs  = uWindow.zw * 0.5;
    vec2 q = abs(vPx - center) - halfs + vec2(uRadius);
    float sdf = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uRadius;
    float fw = max(fwidth(sdf), 1e-4);
    float inside = 1.0 - smoothstep(-fw * 0.5, fw * 0.5, sdf);
    float outline_band = smoothstep(-fw * 1.5, -fw * 0.5, sdf) *
                         (1.0 - smoothstep(-fw * 0.5, fw * 0.5, sdf));

    vec2 uv = (vPx - uWindow.xy) / max(uWindow.zw, vec2(1.0));
    vec4 content = vec4(0.0);
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
        content = texture(uTex, uv);
    }
    vec3 content_rgb = mix(content.rgb, content.rgb * (1.0 - uOutline), outline_band);

    float d = max(sdf, 0.0);
    float shadow_alpha = uShadow * exp(-(d * d) / (2.0 * uSigma * uSigma));
    float out_a = inside + shadow_alpha * (1.0 - inside);
    vec3  out_rgb = content_rgb * inside;   // already premultiplied
    fragColor = vec4(out_rgb, out_a);
}
)";

static constexpr float TITLEBAR_R = 0.13f;
static constexpr float TITLEBAR_G = 0.14f;
static constexpr float TITLEBAR_B = 0.18f;
static constexpr float SHADOW_SIGMA = 14.0f;

bool CsdCompositor::CompileShaders() {
    closeProg_ = link_program(kCloseVS, kCloseFS);
    if (!closeProg_) return false;
    uCloseRect_     = glGetUniformLocation(closeProg_, "uRect");
    uCloseScreen_   = glGetUniformLocation(closeProg_, "uScreen");
    uCloseHover_    = glGetUniformLocation(closeProg_, "uHover");
    uClosePressed_  = glGetUniformLocation(closeProg_, "uPressed");
    uCloseBarColor_ = glGetUniformLocation(closeProg_, "uBarColor");

    composeProg_ = link_program(kComposeVS, kComposeFS);
    if (!composeProg_) return false;
    uCScreen_  = glGetUniformLocation(composeProg_, "uScreen");
    uCWindow_  = glGetUniformLocation(composeProg_, "uWindow");
    uCRadius_  = glGetUniformLocation(composeProg_, "uRadius");
    uCSigma_   = glGetUniformLocation(composeProg_, "uSigma");
    uCShadow_  = glGetUniformLocation(composeProg_, "uShadow");
    uCOutline_ = glGetUniformLocation(composeProg_, "uOutline");
    uCTex_     = glGetUniformLocation(composeProg_, "uTex");

    const float quad[] = { 0,0, 1,0, 1,1, 0,0, 1,1, 0,1 };

    glGenVertexArrays(1, &closeVao_);
    glBindVertexArray(closeVao_);
    glGenBuffers(1, &closeVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, closeVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glGenVertexArrays(1, &quadVao_);
    glBindVertexArray(quadVao_);
    glGenBuffers(1, &quadVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glBindVertexArray(0);
    return true;
}

bool CsdCompositor::Init() { return CompileShaders(); }

void CsdCompositor::Shutdown() {
    if (closeProg_) { glDeleteProgram(closeProg_); closeProg_ = 0; }
    if (closeVao_)  { glDeleteVertexArrays(1, &closeVao_); closeVao_ = 0; }
    if (closeVbo_)  { glDeleteBuffers(1, &closeVbo_); closeVbo_ = 0; }
    if (composeProg_) { glDeleteProgram(composeProg_); composeProg_ = 0; }
    if (quadVao_)     { glDeleteVertexArrays(1, &quadVao_); quadVao_ = 0; }
    if (quadVbo_)     { glDeleteBuffers(1, &quadVbo_); quadVbo_ = 0; }
    if (contentFbo_)  { glDeleteFramebuffers(1, &contentFbo_); contentFbo_ = 0; }
    if (contentTex_)  { glDeleteTextures(1, &contentTex_); contentTex_ = 0; }
    if (mainFbo_)     { glDeleteFramebuffers(1, &mainFbo_); mainFbo_ = 0; }
    if (mainTex_)     { glDeleteTextures(1, &mainTex_); mainTex_ = 0; }
}

void CsdCompositor::EnsureFbo(GLuint* fbo, GLuint* tex, int* curW, int* curH,
                              int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == *curW && h == *curH && *fbo) return;
    if (!*fbo) glGenFramebuffers(1, fbo);
    if (!*tex) glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    *curW = w; *curH = h;
}

CsdCompositor::Frame CsdCompositor::BeginFrame(int windowW, int windowH, int scale) {
    const int S = scale;
    const int contentW = windowW * S;
    const int contentH = (windowH - TITLEBAR_H) * S;
    EnsureFbo(&contentFbo_, &contentTex_, &contentFboW_, &contentFboH_, contentW, contentH);
    glBindFramebuffer(GL_FRAMEBUFFER, contentFbo_);
    glViewport(0, 0, contentW, contentH);
    return { contentW, contentH };
}

void CsdCompositor::DrawCloseButton(int mainW, int mainH, int scale) {
    const int S = scale;
    int cx = (mainW / S - CLOSE_MARGIN - CLOSE_SIZE) * S;
    // mainFbo is GL-y-up; titlebar occupies the TOP band of it. Close button
    // sits inside that band, centered vertically.
    int cy = mainH - ((TITLEBAR_H + CLOSE_SIZE) / 2) * S;
    int cw = CLOSE_SIZE * S;
    int ch = CLOSE_SIZE * S;

    glDisable(GL_BLEND);
    glUseProgram(closeProg_);
    glUniform4f(uCloseRect_, (float)cx, (float)cy, (float)cw, (float)ch);
    glUniform2f(uCloseScreen_, (float)mainW, (float)mainH);
    glUniform1f(uCloseHover_, closeHoverAmt_);
    glUniform1f(uClosePressed_, closePressed_ ? 1.0f : 0.0f);
    glUniform3f(uCloseBarColor_, TITLEBAR_R, TITLEBAR_G, TITLEBAR_B);
    glBindVertexArray(closeVao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void CsdCompositor::EndFrame(int windowW, int windowH, int scale, bool floating) {
    const int S = scale;
    const int winPxW = windowW * S;
    const int winPxH = windowH * S;
    const int contentPxW = winPxW;
    const int contentPxH = (windowH - TITLEBAR_H) * S;
    const int shadow = floating ? SHADOW_EXTENT : 0;
    const int radius = floating ? CORNER_RADIUS : 0;
    const int bufW = (windowW + 2 * shadow) * S;
    const int bufH = (windowH + 2 * shadow) * S;

    // Animate hover intensity.
    float target = closeHover_ ? 1.0f : 0.0f;
    const float speed = 0.2f;
    if (target > closeHoverAmt_) closeHoverAmt_ = std::min(target, closeHoverAmt_ + speed);
    else                         closeHoverAmt_ = std::max(target, closeHoverAmt_ - speed);

    // ---- Pass 2: assemble window-sized mainFbo -----------------------------
    EnsureFbo(&mainFbo_, &mainTex_, &mainFboW_, &mainFboH_, winPxW, winPxH);

    glBindFramebuffer(GL_FRAMEBUFFER, mainFbo_);
    glViewport(0, 0, winPxW, winPxH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(TITLEBAR_R, TITLEBAR_G, TITLEBAR_B, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Blit content into the bottom region of mainFbo. GL y-up: the titlebar
    // clear already covers the full texture; this just overwrites the content
    // portion (y=0..contentPxH).
    glBindFramebuffer(GL_READ_FRAMEBUFFER, contentFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, mainFbo_);
    glBlitFramebuffer(0, 0, contentPxW, contentPxH,
                      0, 0, contentPxW, contentPxH,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, mainFbo_);
    glViewport(0, 0, winPxW, winPxH);
    DrawCloseButton(winPxW, winPxH, S);

    // ---- Pass 3: compose mainFbo onto default FB with shadow + corners ----
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, bufW, bufH);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied-alpha output

    glUseProgram(composeProg_);
    glUniform2f(uCScreen_, (float)bufW, (float)bufH);
    glUniform4f(uCWindow_, (float)(shadow * S), (float)(shadow * S),
                           (float)winPxW,        (float)winPxH);
    glUniform1f(uCRadius_,  (float)(radius * S));
    glUniform1f(uCSigma_,   SHADOW_SIGMA * S);
    glUniform1f(uCShadow_,  floating ? 0.275f : 0.0f);
    glUniform1f(uCOutline_, floating ? 0.18f  : 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mainTex_);
    glUniform1i(uCTex_, 0);
    glBindVertexArray(quadVao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    // Leave blending in the state the app expects for its normal draw loop.
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

bool CsdCompositor::InCloseButton(int x, int y, int windowW) const {
    int cx = windowW - CLOSE_MARGIN - CLOSE_SIZE;
    int cy = (TITLEBAR_H - CLOSE_SIZE) / 2;
    return x >= cx && x < cx + CLOSE_SIZE && y >= cy && y < cy + CLOSE_SIZE;
}

bool CsdCompositor::InTitlebar(int, int y) const {
    return y >= 0 && y < TITLEBAR_H;
}

uint32_t CsdCompositor::ResizeEdge(int x, int y, int windowW, int windowH) const {
    // xdg-shell resize-edge constants. We hand back the numeric values directly
    // so csd.h can stay free of xdg-shell headers.
    //   NONE=0, TOP=1, BOTTOM=2, LEFT=4, TOP_LEFT=5, BOTTOM_LEFT=6,
    //   RIGHT=8, TOP_RIGHT=9, BOTTOM_RIGHT=10.
    const bool L = x < BORDER_GRAB;
    const bool R = x > windowW - BORDER_GRAB;
    const bool T = y < BORDER_GRAB;
    const bool B = y > windowH - BORDER_GRAB;
    if (T && L) return 5;
    if (T && R) return 9;
    if (B && L) return 6;
    if (B && R) return 10;
    if (T) return 1;
    if (B) return 2;
    if (L) return 4;
    if (R) return 8;
    return 0;
}

const char* CsdCompositor::CursorNameForEdge(uint32_t edge) {
    switch (edge) {
    case 1:  return "n-resize";
    case 2:  return "s-resize";
    case 4:  return "w-resize";
    case 5:  return "nw-resize";
    case 6:  return "sw-resize";
    case 8:  return "e-resize";
    case 9:  return "ne-resize";
    case 10: return "se-resize";
    default: return nullptr;
    }
}
