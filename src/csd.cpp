#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "csd.h"
#include <cstdio>
#include <cstring>
#include <cmath>

static bool compile_shader(GLuint shader, const char* src) {
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[CSD] shader compile error: %s\n", log);
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
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint linked = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[CSD] program link error: %s\n", log);
        glDeleteProgram(p);
        p = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return p;
}

// ---------------------------------------------------------------------------
// Close button shader
// ---------------------------------------------------------------------------
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
    vec2  p  = vUV * 2.0 - 1.0;
    float r  = length(p);
    float aa = fwidth(r);
    float disc = 1.0 - smoothstep(1.0 - aa, 1.0, r);
    mat2 R = mat2(0.70710678, -0.70710678, 0.70710678, 0.70710678);
    vec2 q = R * p;
    const float cap_r = 0.055, cap_L = 0.34;
    float d1 = length(vec2(max(abs(q.x) - cap_L, 0.0), q.y)) - cap_r;
    float d2 = length(vec2(max(abs(q.y) - cap_L, 0.0), q.x)) - cap_r;
    float dx  = min(d1, d2);
    float aax = fwidth(dx);
    float xmask = 1.0 - smoothstep(0.0, aax, dx);
    vec3 normal_c  = vec3(0.245, 0.255, 0.295);
    vec3 hover_c   = vec3(0.320, 0.330, 0.370);
    vec3 pressed_c = vec3(0.420, 0.430, 0.470);
    vec3 bg = mix(normal_c, hover_c, uHover);
    bg      = mix(bg, pressed_c, uPressed);
    vec3 disc_col = mix(bg, vec3(1.0), xmask);
    vec3 col      = mix(uBarColor, disc_col, disc);
    fragColor     = vec4(col, 1.0);
}
)";

// ---------------------------------------------------------------------------
// Compose shader (shadow + rounded corners)
// ---------------------------------------------------------------------------
static const char* kFullQuadVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vPx;
out vec2 vUV;
uniform vec2 uScreen;
void main() {
    vec2 px  = aPos * uScreen;
    vec2 ndc = vec2(aPos.x, 1.0 - aPos.y) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vPx = px;
    vUV = vec2(aPos.x, 1.0 - aPos.y);
}
)";

static const char* kComposeFS = R"(#version 330 core
in  vec2 vPx;
in  vec2 vUV;
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
    float fw = fwidth(sdf);
    float inside = 1.0 - smoothstep(-fw * 0.5, fw * 0.5, sdf);
    float outline_band = smoothstep(-fw * 1.5, -fw * 0.5, sdf) *
                         (1.0 - smoothstep(-fw * 0.5, fw * 0.5, sdf));
    vec4 content = texture(uTex, vUV);
    vec3 content_rgb = mix(content.rgb, content.rgb * (1.0 - uOutline), outline_band);
    float d = max(sdf, 0.0);
    float shadow_alpha = uShadow * exp(-(d * d) / (2.0 * uSigma * uSigma));
    float out_a = inside + shadow_alpha * (1.0 - inside);
    vec3  out_rgb = content_rgb * inside / max(out_a, 1e-4);
    fragColor = vec4(out_rgb, out_a);
}
)";

// ---------------------------------------------------------------------------
// CsdCompositor implementation
// ---------------------------------------------------------------------------

static constexpr int CLOSE_SIZE   = 24;
static constexpr int CLOSE_MARGIN = 8;
static constexpr float SHADOW_SIGMA = 14.0f;
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

    const float quad[] = {0,0, 1,0, 1,1, 0,0, 1,1, 0,1};
    glGenVertexArrays(1, &closeVao_);
    glBindVertexArray(closeVao_);
    glGenBuffers(1, &closeVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, closeVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);

    composeProg_ = link_program(kFullQuadVS, kComposeFS);
    if (!composeProg_) return false;
    cTex_     = glGetUniformLocation(composeProg_, "uTex");
    cScreen_  = glGetUniformLocation(composeProg_, "uScreen");
    cWindow_  = glGetUniformLocation(composeProg_, "uWindow");
    cRadius_  = glGetUniformLocation(composeProg_, "uRadius");
    cSigma_   = glGetUniformLocation(composeProg_, "uSigma");
    cShadow_  = glGetUniformLocation(composeProg_, "uShadow");
    cOutline_ = glGetUniformLocation(composeProg_, "uOutline");

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

bool CsdCompositor::Init() {
    return CompileShaders();
}

void CsdCompositor::Shutdown() {
    if (closeProg_) glDeleteProgram(closeProg_);
    if (closeVao_) glDeleteVertexArrays(1, &closeVao_);
    if (closeVbo_) glDeleteBuffers(1, &closeVbo_);
    if (composeProg_) glDeleteProgram(composeProg_);
    if (quadVao_) glDeleteVertexArrays(1, &quadVao_);
    if (quadVbo_) glDeleteBuffers(1, &quadVbo_);
    if (contentFbo_) glDeleteFramebuffers(1, &contentFbo_);
    if (contentTex_) glDeleteTextures(1, &contentTex_);
    if (composeFbo_) glDeleteFramebuffers(1, &composeFbo_);
    if (composeTex_) glDeleteTextures(1, &composeTex_);
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
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, contentFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, contentTex_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    contentFboW_ = w; contentFboH_ = h;
}

void CsdCompositor::EnsureComposeFbo(int w, int h) {
    if (w == composeFboW_ && h == composeFboH_ && composeFbo_) return;
    if (!composeFbo_) glGenFramebuffers(1, &composeFbo_);
    if (!composeTex_) glGenTextures(1, &composeTex_);
    glBindTexture(GL_TEXTURE_2D, composeTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, composeFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, composeTex_, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    composeFboW_ = w; composeFboH_ = h;
}

CsdCompositor::Frame CsdCompositor::BeginFrame(int windowW, int windowH, int scale) {
    int contentW = windowW * scale;
    int contentH = (windowH - TITLEBAR_H) * scale;
    EnsureContentFbo(contentW, contentH);
    glBindFramebuffer(GL_FRAMEBUFFER, contentFbo_);
    glViewport(0, 0, contentW, contentH);
    return {contentW, contentH};
}

void CsdCompositor::DrawCloseButton(int windowW, int windowH, int scale) {
    const int S = scale;
    const int sh = SHADOW_EXTENT;
    const int W = (windowW + 2 * sh) * S;
    const int H = (windowH + 2 * sh) * S;
    int cx = (sh + windowW - CLOSE_MARGIN - CLOSE_SIZE) * S;
    int cy = (sh + (TITLEBAR_H - CLOSE_SIZE) / 2) * S;
    int cw = CLOSE_SIZE * S;
    int ch = CLOSE_SIZE * S;

    glUseProgram(closeProg_);
    glUniform4f(uRect_, float(cx), float(cy), float(cw), float(ch));
    glUniform2f(uScreen_, float(W), float(H));
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
    const int sh = SHADOW_EXTENT;
    const int W = (windowW + 2 * sh) * S;
    const int H = (windowH + 2 * sh) * S;
    int contentW = windowW * S;
    int contentH = (windowH - TITLEBAR_H) * S;

    // Animate close button hover
    float target = closeHover_ ? 1.0f : 0.0f;
    const float speed = 0.3f;
    if (target > closeHoverAmt_) closeHoverAmt_ = std::min(target, closeHoverAmt_ + speed);
    else closeHoverAmt_ = std::max(target, closeHoverAmt_ - speed);

    // 1) Compose into intermediate FBO: titlebar + content + close button
    EnsureComposeFbo(W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, composeFbo_);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    // Titlebar background
    glEnable(GL_SCISSOR_TEST);
    glScissor(sh * S, H - (sh + TITLEBAR_H) * S, contentW, TITLEBAR_H * S);
    glClearColor(TITLEBAR_R, TITLEBAR_G, TITLEBAR_B, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    // Blit content into position
    glBindFramebuffer(GL_READ_FRAMEBUFFER, contentFbo_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, composeFbo_);
    glBlitFramebuffer(0, 0, contentW, contentH,
                      sh * S, sh * S, sh * S + contentW, sh * S + contentH,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Close button
    glBindFramebuffer(GL_FRAMEBUFFER, composeFbo_);
    glViewport(0, 0, W, H);
    DrawCloseButton(windowW, windowH, scale);

    // 2) Final compose to screen: shadow + rounded corners
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    int winW = windowW * S;
    int winH = windowH * S;
    int wx = sh * S;
    int wy = sh * S;

    glUseProgram(composeProg_);
    glUniform2f(cScreen_, float(W), float(H));
    glUniform4f(cWindow_, float(wx), float(wy), float(winW), float(winH));
    glUniform1f(cRadius_, float(CORNER_RADIUS * S));
    glUniform1f(cSigma_,  SHADOW_SIGMA * S);
    glUniform1f(cShadow_, 0.275f);
    glUniform1f(cOutline_, 0.18f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, composeTex_);
    glUniform1i(cTex_, 0);
    glBindVertexArray(quadVao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

bool CsdCompositor::InCloseButton(int x, int y, int windowW, int scale) const {
    (void)scale;
    int cx = windowW - CLOSE_MARGIN - CLOSE_SIZE;
    int cy = (TITLEBAR_H - CLOSE_SIZE) / 2;
    return x >= cx && x < cx + CLOSE_SIZE && y >= cy && y < cy + CLOSE_SIZE;
}

bool CsdCompositor::InTitlebar(int x, int y, int scale) const {
    (void)scale;
    return y < TITLEBAR_H;
}

uint32_t CsdCompositor::ResizeEdge(int x, int y, int windowW, int windowH) const {
    constexpr int BORDER_GRAB = 6;
    bool L = x < BORDER_GRAB, R = x > windowW - BORDER_GRAB;
    bool T = y < BORDER_GRAB, B = y > windowH - BORDER_GRAB;
    if (T && L) return 1;  // TOP_LEFT
    if (T && R) return 2;  // TOP_RIGHT
    if (B && L) return 3;  // BOTTOM_LEFT
    if (B && R) return 4;  // BOTTOM_RIGHT
    if (L)      return 5;  // LEFT
    if (R)      return 6;  // RIGHT
    if (T)      return 7;  // TOP
    if (B)      return 8;  // BOTTOM
    return 0;              // NONE
}
