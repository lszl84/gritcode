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
