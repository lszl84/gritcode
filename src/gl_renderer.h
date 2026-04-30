// Gritcode — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#endif
#include "types.h"
#include "font.h"
#include <vector>

// GPU-accelerated renderer using OpenGL 3.3 core.
// Glyph atlas uploaded as a GL texture. Text drawn as textured quads.
// All quads batched into a single draw call per frame.
class GLRenderer {
public:
    bool Init();
    void Shutdown();

    void BeginFrame(int viewportW, int viewportH, const FontManager& fm);
    void EndFrame();

    void DrawRect(float x, float y, float w, float h, const Color& c);
    void DrawRoundedRect(float x, float y, float w, float h, float r, const Color& c);
    // Horizontal gradient: per-vertex colors, smoothly interpolated by GL.
    void DrawGradientRectH(float x, float y, float w, float h,
                           const Color& left, const Color& right);
    void DrawGlyph(const GlyphInfo& gi, float x, float y, const Color& c, float ascent);
    void DrawShapedRun(const FontManager& fm, const ShapedRun& run,
                       float x, float y, float ascent, const Color& c);
    void DrawTriRight(float cx, float cy, float size, const Color& c);
    void DrawTriDown(float cx, float cy, float size, const Color& c);
    // PushClip/PopClip form a proper stack: nested pushes intersect with the
    // current clip, and PopClip restores the previous one (rather than just
    // disabling scissor, which would leak content past the outer clip).
    void PushClip(float x, float y, float w, float h);
    void PopClip();

private:
    GLuint program_ = 0;
    GLuint vao_ = 0, vbo_ = 0;
    GLuint atlasTex_ = 0;
    GLint locProj_ = -1, locUseTex_ = -1;

    int vpW_ = 0, vpH_ = 0;
    const FontManager* fm_ = nullptr;
    size_t lastAtlasGen_ = 0;

    struct Vertex {
        float x, y;       // position
        float u, v;       // texcoord / local coords for SDF
        float r, g, b, a; // color
        float useTex;     // 0=solid, 1=textured, 2+=rounded rect (value-2 = radius)
        float rectW, rectH; // rect size for SDF rounded rect mode
    };
    std::vector<Vertex> batch_;

    struct ClipRect { int x, y, w, h; };  // GL-space (bottom-left origin)
    std::vector<ClipRect> clipStack_;

    void PushQuad(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1,
                  const Color& c, float useTex);
    void UpdateAtlasTexture();
    void FlushBatch();
};
