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

#include "renderer.h"
#include <algorithm>
#include <cstring>

void Renderer::BeginFrame(uint32_t* pixels, int w, int h, const FontManager& fm) {
    px_ = pixels;
    w_ = w;
    h_ = h;
    fm_ = &fm;
}

void Renderer::DrawRect(float x, float y, float w, float h, const Color& c) {
    int x0 = std::max(0, (int)x);
    int y0 = std::max(0, (int)y);
    int x1 = std::min(w_, (int)(x + w));
    int y1 = std::min(h_, (int)(y + h));
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t argb = ColorToARGB(c);
    int span = x1 - x0;

    if (c.a >= 0.99f) {
        for (int row = y0; row < y1; row++) {
            uint32_t* dst = px_ + row * w_ + x0;
            // Fill with 32-bit writes
            for (int i = 0; i < span; i++) dst[i] = argb;
        }
    } else {
        // Alpha blend
        uint8_t sa = (uint8_t)(c.a * 255);
        uint8_t sr = (uint8_t)(c.r * 255);
        uint8_t sg = (uint8_t)(c.g * 255);
        uint8_t sb = (uint8_t)(c.b * 255);
        uint8_t inv = 255 - sa;
        for (int row = y0; row < y1; row++) {
            uint32_t* dst = px_ + row * w_ + x0;
            for (int i = 0; i < span; i++) {
                uint32_t d = dst[i];
                uint8_t dr = ((d >> 16) & 0xFF);
                uint8_t dg = ((d >> 8) & 0xFF);
                uint8_t db = (d & 0xFF);
                dr = (sr * sa + dr * inv) / 255;
                dg = (sg * sa + dg * inv) / 255;
                db = (sb * sa + db * inv) / 255;
                dst[i] = (255u << 24) | (dr << 16) | (dg << 8) | db;
            }
        }
    }
}

void Renderer::DrawGlyph(const GlyphInfo& gi, float x, float y,
                          const Color& c, float ascent) {
    if (gi.width == 0 || gi.height == 0 || !fm_) return;

    int gx = (int)(x + gi.bearingX);
    int gy = (int)(y + ascent - gi.bearingY);

    const uint8_t* atlas = fm_->AtlasData();
    int atlasW = fm_->AtlasWidth();
    uint8_t sr = (uint8_t)(c.r * 255);
    uint8_t sg = (uint8_t)(c.g * 255);
    uint8_t sb = (uint8_t)(c.b * 255);

    int srcX = gi.atlasX;
    int srcY = gi.atlasY;

    // Clip
    int startRow = std::max(0, -gy);
    int startCol = std::max(0, -gx);
    int endRow = std::min(gi.height, h_ - gy);
    int endCol = std::min(gi.width, w_ - gx);

    for (int row = startRow; row < endRow; row++) {
        const uint8_t* src = atlas + (srcY + row) * atlasW + srcX + startCol;
        uint32_t* dst = px_ + (gy + row) * w_ + gx + startCol;
        int cols = endCol - startCol;

        for (int col = 0; col < cols; col++) {
            uint8_t alpha = src[col];
            if (alpha == 0) continue;
            if (alpha == 255) {
                dst[col] = (255u << 24) | (sr << 16) | (sg << 8) | sb;
            } else {
                uint32_t d = dst[col];
                uint8_t inv = 255 - alpha;
                uint8_t dr = (sr * alpha + ((d >> 16) & 0xFF) * inv) / 255;
                uint8_t dg = (sg * alpha + ((d >> 8) & 0xFF) * inv) / 255;
                uint8_t db = (sb * alpha + (d & 0xFF) * inv) / 255;
                dst[col] = (255u << 24) | (dr << 16) | (dg << 8) | db;
            }
        }
    }
}

void Renderer::DrawShapedRun(const FontManager& fm, const ShapedRun& run,
                              float x, float y, float ascent, const Color& color) {
    for (auto& g : run.glyphs) {
        const GlyphInfo& gi = fm.EnsureGlyph(g.glyphId, g.faceIdx);
        DrawGlyph(gi, x + g.xPos, y, color, ascent);
    }
}

void Renderer::DrawTriRight(float cx, float cy, float size, const Color& c) {
    float half = size / 2;
    uint32_t argb = ColorToARGB(c);
    for (float row = -half; row <= half; row += 1.0f) {
        float span = half - std::abs(row);
        int x0 = std::max(0, (int)cx);
        int x1 = std::min(w_, (int)(cx + span));
        int y = (int)(cy + row);
        if (y < 0 || y >= h_ || x0 >= x1) continue;
        for (int x = x0; x < x1; x++) px_[y * w_ + x] = argb;
    }
}

void Renderer::DrawTriDown(float cx, float cy, float size, const Color& c) {
    float half = size / 2;
    uint32_t argb = ColorToARGB(c);
    for (float row = 0; row <= size; row += 1.0f) {
        float span = half * (1.0f - row / size);
        int x0 = std::max(0, (int)(cx - span));
        int x1 = std::min(w_, (int)(cx + span));
        int y = (int)(cy + row - half);
        if (y < 0 || y >= h_ || x0 >= x1) continue;
        for (int x = x0; x < x1; x++) px_[y * w_ + x] = argb;
    }
}
