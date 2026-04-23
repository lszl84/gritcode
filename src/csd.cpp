// Gritcode — Client-side decorations for Wayland.
//
// Kept deliberately minimal so Mesa's GLES driver doesn't trip on anything
// exotic: titlebar is a flat scissored clear, close button is a disc with
// an X, content is blitted via glBlitFramebuffer. No rounded corners, no
// gaussian shadow — those tickled a gallium crash during development.
//
// Copyright (C) 2026 luke@devmindscape.com. GPL v3.

#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "csd.h"

#include <cstdio>
#include <cmath>
#include <algorithm>

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

// Close button: a flat circle that lights up on hover, with a crisp X.
// Using only basic ops — length, smoothstep, abs, min — avoids the SDF
// corner-radius path that crashed mesa on earlier iterations.
static const char* kCloseVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
uniform vec4 uRect;
uniform vec2 uScreen;
out vec2 vUV;
void main() {
    vec2 px  = uRect.xy + aPos * uRect.zw;
    vec2 ndc = vec2(px.x / uScreen.x, 1.0 - px.y / uScreen.y) * 2.0 - 1.0;
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

    // Disc background (AA via smoothstep against r).
    float disc = 1.0 - smoothstep(0.95, 1.0, r);

    // Rotated X: two thin rounded-cap rectangles.
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

static constexpr int CLOSE_SIZE   = 24;
static constexpr int CLOSE_MARGIN = 8;
static constexpr float TITLEBAR_R = 0.13f;
static constexpr float TITLEBAR_G = 0.14f;
static constexpr float TITLEBAR_B = 0.18f;

bool CsdCompositor::CompileShaders() {
    closeProg_ = link_program(kCloseVS, kCloseFS);
    if (!closeProg_) return false;
    uRect_     = glGetUniformLocation(closeProg_, "uRect");
    uScreen_   = glGetUniformLocation(closeProg_, "uScreen");
    uHover_    = glGetUniformLocation(closeProg_, "uHover");
    uPressed_  = glGetUniformLocation(closeProg_, "uPressed");
    uBarColor_ = glGetUniformLocation(closeProg_, "uBarColor");

    const float quad[] = { 0,0, 1,0, 1,1, 0,0, 1,1, 0,1 };
    glGenVertexArrays(1, &closeVao_);
    glBindVertexArray(closeVao_);
    glGenBuffers(1, &closeVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, closeVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    return true;
}

bool CsdCompositor::Init() {
    return CompileShaders();
}

void CsdCompositor::Shutdown() {
    if (closeProg_) { glDeleteProgram(closeProg_); closeProg_ = 0; }
    if (closeVao_)  { glDeleteVertexArrays(1, &closeVao_); closeVao_ = 0; }
    if (closeVbo_)  { glDeleteBuffers(1, &closeVbo_); closeVbo_ = 0; }
    if (contentFbo_) { glDeleteFramebuffers(1, &contentFbo_); contentFbo_ = 0; }
    if (contentTex_) { glDeleteTextures(1, &contentTex_); contentTex_ = 0; }
}

void CsdCompositor::EnsureContentFbo(int w, int h) {
    if (w == contentFboW_ && h == contentFboH_ && contentFbo_) return;
    if (!contentFbo_) glGenFramebuffers(1, &contentFbo_);
    if (!contentTex_) glGenTextures(1, &contentTex_);
    glBindTexture(GL_TEXTURE_2D, contentTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, contentFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, contentTex_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    contentFboW_ = w; contentFboH_ = h;
}

void CsdCompositor::EnsureComposeFbo(int, int) {
    // No longer used — we draw the final frame directly to the default FB
    // and avoid the shader-heavy compose pass. Kept as a stub because csd.h
    // still declares it.
}

CsdCompositor::Frame CsdCompositor::BeginFrame(int windowW, int windowH, int scale) {
    int contentW = windowW * scale;
    int contentH = (windowH - TITLEBAR_H) * scale;
    if (contentH < 1) contentH = 1;
    EnsureContentFbo(contentW, contentH);
    glBindFramebuffer(GL_FRAMEBUFFER, contentFbo_);
    glViewport(0, 0, contentW, contentH);
    return { contentW, contentH };
}

void CsdCompositor::DrawCloseButton(int windowW, int windowH, int scale) {
    const int S = scale;
    const int W = windowW * S;
    const int H = windowH * S;
    int cx = (windowW - CLOSE_MARGIN - CLOSE_SIZE) * S;
    int cy = ((TITLEBAR_H - CLOSE_SIZE) / 2) * S;
    int cw = CLOSE_SIZE * S;
    int ch = CLOSE_SIZE * S;

    glDisable(GL_BLEND);
    glUseProgram(closeProg_);
    glUniform4f(uRect_, (float)cx, (float)cy, (float)cw, (float)ch);
    glUniform2f(uScreen_, (float)W, (float)H);
    glUniform1f(uHover_, closeHoverAmt_);
    glUniform1f(uPressed_, closePressed_ ? 1.0f : 0.0f);
    glUniform3f(uBarColor_, TITLEBAR_R, TITLEBAR_G, TITLEBAR_B);
    glBindVertexArray(closeVao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

void CsdCompositor::EndFrame(int windowW, int windowH, int scale) {
    const int S = scale;
    const int W = windowW * S;
    const int H = windowH * S;
    int contentW = windowW * S;
    int contentH = (windowH - TITLEBAR_H) * S;

    // Smooth-animate hover intensity each frame.
    float target = closeHover_ ? 1.0f : 0.0f;
    const float speed = 0.2f;
    if (target > closeHoverAmt_) closeHoverAmt_ = std::min(target, closeHoverAmt_ + speed);
    else                         closeHoverAmt_ = std::max(target, closeHoverAmt_ - speed);

    // (1) Titlebar — flat color via scissored clear.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_SCISSOR_TEST);
    glScissor(0, H - TITLEBAR_H * S, W, TITLEBAR_H * S);
    glClearColor(TITLEBAR_R, TITLEBAR_G, TITLEBAR_B, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    // (2) Blit content FBO into place below the titlebar.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, contentFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    // Content was rendered into the FBO with Y going up (GL default). Blit
    // directly: src 0..contentH → dst 0..contentH of the default framebuffer.
    glBlitFramebuffer(0, 0, contentW, contentH,
                      0, 0, contentW, contentH,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // (3) Close button draw on top.
    glViewport(0, 0, W, H);
    DrawCloseButton(windowW, windowH, scale);
}

bool CsdCompositor::InCloseButton(int x, int y, int windowW, int scale) const {
    (void)scale;
    int cx = windowW - CLOSE_MARGIN - CLOSE_SIZE;
    int cy = (TITLEBAR_H - CLOSE_SIZE) / 2;
    return x >= cx && x < cx + CLOSE_SIZE && y >= cy && y < cy + CLOSE_SIZE;
}

bool CsdCompositor::InTitlebar(int, int y, int) const {
    return y >= 0 && y < TITLEBAR_H;
}

uint32_t CsdCompositor::ResizeEdge(int x, int y, int windowW, int windowH) const {
    constexpr int GRAB = 6;
    bool L = x < GRAB, R = x > windowW - GRAB;
    bool T = y < GRAB, B = y > windowH - GRAB;
    if (T && L) return 1; if (T && R) return 2;
    if (B && L) return 3; if (B && R) return 4;
    if (L) return 5; if (R) return 6;
    if (T) return 7; if (B) return 8;
    return 0;
}
