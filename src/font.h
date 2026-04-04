#pragma once
#include "types.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <vector>
#include <unordered_map>
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
    };

    FT_Library ft_ = nullptr;
    std::vector<Face> faces_;
    std::map<FontStyle, std::pair<int, int>> styleFaces_;

    int atlasW_ = 2048, atlasH_ = 2048;
    mutable std::vector<uint8_t> atlasData_;
    mutable int curX_ = 1, curY_ = 1, rowH_ = 0;
    mutable std::unordered_map<uint64_t, GlyphInfo> cache_;
    mutable size_t atlasGen_ = 0;

    uint64_t Key(uint32_t glyph, int face) const { return ((uint64_t)face << 32) | glyph; }

    int LoadFace(const std::string& path, int sizePx);
    std::string FindFont(const char* family, bool bold, bool italic) const;
    std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> fileCache_;
};
