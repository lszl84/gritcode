#pragma once
#include "types.h"
#include "font.h"

class Renderer {
public:
    void BeginFrame(uint32_t* pixels, int w, int h, const FontManager& fm);
    void DrawRect(float x, float y, float w, float h, const Color& c);
    void DrawGlyph(const GlyphInfo& gi, float x, float y, const Color& c, float ascent);
    void DrawShapedRun(const FontManager& fm, const ShapedRun& run,
                       float x, float y, float ascent, const Color& c);
    // Filled triangles for collapse/expand indicators and dropdown arrows
    void DrawTriRight(float cx, float cy, float size, const Color& c);  // ▶
    void DrawTriDown(float cx, float cy, float size, const Color& c);   // ▼

private:
    uint32_t* px_ = nullptr;
    int w_ = 0, h_ = 0;
    const FontManager* fm_ = nullptr;

    static uint32_t ColorToARGB(const Color& c) {
        return (255u << 24) |
               ((uint32_t)(c.r * 255) << 16) |
               ((uint32_t)(c.g * 255) << 8) |
               (uint32_t)(c.b * 255);
    }
};
