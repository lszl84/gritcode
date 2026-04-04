#pragma once
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
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
    void DrawGlyph(const GlyphInfo& gi, float x, float y, const Color& c, float ascent);
    void DrawShapedRun(const FontManager& fm, const ShapedRun& run,
                       float x, float y, float ascent, const Color& c);
    void DrawTriRight(float cx, float cy, float size, const Color& c);
    void DrawTriDown(float cx, float cy, float size, const Color& c);

private:
    GLuint program_ = 0;
    GLuint vao_ = 0, vbo_ = 0;
    GLuint atlasTex_ = 0;
    GLint locProj_ = -1, locUseTex_ = -1;

    int vpW_ = 0, vpH_ = 0;
    const FontManager* fm_ = nullptr;
    size_t lastAtlasGen_ = 0;  // Track atlas changes

    struct Vertex {
        float x, y;       // position
        float u, v;       // texcoord
        float r, g, b, a; // color
        float useTex;     // 1.0 = textured, 0.0 = solid color
    };
    std::vector<Vertex> batch_;

    void PushQuad(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1,
                  const Color& c, float useTex);
    void UpdateAtlasTexture();
    void FlushBatch();
};
