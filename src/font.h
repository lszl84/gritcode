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
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <vector>
#include <map>
#include <memory>
#include <string>

struct GlyphInfo {
    int atlasX, atlasY;
    int bearingX, bearingY;
    int width, height;
};

struct ShapedGlyph {
    uint32_t glyphId;
    float xPos, yPos;
    float xAdvance;
    uint32_t cluster;
    int faceIdx;
    const GlyphInfo* cached = nullptr;  // Direct pointer, avoids hash lookup during draw
};

struct ShapedRun {
    std::vector<ShapedGlyph> glyphs;
    float totalWidth = 0;
};

class FontManager {
public:
    FontManager();
    ~FontManager();

    bool Init(int baseSizePx = 20);

    ShapedRun Shape(const std::string& text, FontStyle style, bool rtl = false) const;

    float LineHeight(FontStyle style) const;
    float Ascent(FontStyle style) const;
    float SpaceWidth(FontStyle style) const;
    float MeasureWidth(const std::string& text, FontStyle style, bool rtl = false) const;

    std::vector<float> CaretPositions(const std::string& text, FontStyle style, bool rtl) const;
    std::vector<float> CaretPositionsFromRun(const ShapedRun& run, const std::string& text, bool rtl) const;

    const GlyphInfo& EnsureGlyph(uint32_t glyphId, int faceIdx) const;

    const uint8_t* AtlasData() const { return atlasData_.data(); }
    int AtlasWidth() const { return atlasW_; }
    int AtlasHeight() const { return atlasH_; }
    size_t AtlasGeneration() const { return atlasGen_; }

    int FaceIndex(FontStyle style, bool rtl = false) const;

private:
    struct Face {
        FT_Face ft = nullptr;
        hb_font_t* hb = nullptr;
        float lineHeight = 0, ascent = 0, descent = 0, spaceWidth = 0;
        float bitmapScale = 1.0f;  // For bitmap fonts: scale factor to desired size
    };

    FT_Library ft_ = nullptr;
    std::vector<Face> faces_;
    std::map<FontStyle, std::pair<int, int>> styleFaces_;

    int atlasW_ = 2048, atlasH_ = 2048;
    mutable std::vector<uint8_t> atlasData_;
    mutable int curX_ = 1, curY_ = 1, rowH_ = 0;
    mutable size_t atlasGen_ = 0;

    // Flat open-addressing glyph cache (replaces unordered_map)
    // Key = (faceIdx << 32) | glyphId. Robin-hood-style linear probing.
    static constexpr size_t CACHE_SIZE = 8192;  // Must be power of 2
    static constexpr size_t CACHE_MASK = CACHE_SIZE - 1;
    struct CacheSlot {
        uint64_t key = ~0ULL;  // ~0 = empty
        GlyphInfo info;
    };
    mutable std::vector<CacheSlot> cache_{CACHE_SIZE};

    uint64_t Key(uint32_t glyph, int face) const { return ((uint64_t)face << 32) | glyph; }

    int LoadFace(const std::string& path, int sizePx);
    std::string FindFont(const char* family, bool bold, bool italic) const;
    int FindFallbackFace(uint32_t codepoint, int sizePx) const;

    mutable std::map<uint64_t, int> fallbackCache_;
    mutable std::map<std::string, int> loadedFaces_;  // "path:sizePx" → face index
    std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> fileCache_;
};
