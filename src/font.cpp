#include "font.h"
#include <fontconfig/fontconfig.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <map>

FontManager::FontManager() = default;

FontManager::~FontManager() {
    for (auto& f : faces_) {
        if (f.hb) hb_font_destroy(f.hb);
        if (f.ft) FT_Done_Face(f.ft);
    }
    if (ft_) FT_Done_FreeType(ft_);
}

std::string FontManager::FindFont(const char* family, bool bold, bool italic) const {
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8*)family);
    if (bold) FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
    else FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_REGULAR);
    if (italic) FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ITALIC);
    else FcPatternAddInteger(pat, FC_SLANT, FC_SLANT_ROMAN);

    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, pat, &result);
    std::string path;
    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
            path = (const char*)file;
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return path;
}

int FontManager::LoadFace(const std::string& path, int sizePx) {
    auto& buf = fileCache_[path];
    if (!buf) {
        buf = std::make_shared<std::vector<uint8_t>>();
        FILE* fp = fopen(path.c_str(), "rb");
        if (!fp) { fprintf(stderr, "Failed to open font: %s\n", path.c_str()); return -1; }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        buf->resize(sz);
        if (fread(buf->data(), 1, sz, fp) != (size_t)sz) {
            fclose(fp);
            return -1;
        }
        fclose(fp);
    }

    Face f;
    if (FT_New_Memory_Face(ft_, buf->data(), (FT_Long)buf->size(), 0, &f.ft)) {
        fprintf(stderr, "Failed to load font: %s\n", path.c_str());
        return -1;
    }
    FT_Set_Pixel_Sizes(f.ft, 0, sizePx);

    f.hb = hb_ft_font_create_referenced(f.ft);

    f.ascent = f.ft->size->metrics.ascender / 64.0f;
    f.descent = -(f.ft->size->metrics.descender / 64.0f);
    f.lineHeight = f.ft->size->metrics.height / 64.0f;

    if (FT_Load_Char(f.ft, ' ', FT_LOAD_DEFAULT) == 0)
        f.spaceWidth = f.ft->glyph->advance.x / 64.0f;
    else
        f.spaceWidth = sizePx * 0.3f;

    int idx = (int)faces_.size();
    faces_.push_back(std::move(f));
    return idx;
}

bool FontManager::Init(int baseSizePx) {
    if (FT_Init_FreeType(&ft_)) return false;
    FcInit();

    int thinkingSizePx = std::max(10, baseSizePx - 3);

    auto loadStyle = [&](FontStyle style, const char* family, bool bold, bool italic, int sizePx) {
        std::string path = FindFont(family, bold, italic);
        if (path.empty()) {
            fprintf(stderr, "Font not found: %s bold=%d italic=%d\n", family, bold, italic);
            return;
        }
        int primaryIdx = LoadFace(path, sizePx);

        std::string arabicPath = FindFont("Noto Sans Arabic", bold, false);
        int arabicIdx = -1;
        if (!arabicPath.empty())
            arabicIdx = LoadFace(arabicPath, sizePx);

        styleFaces_[style] = {primaryIdx, arabicIdx >= 0 ? arabicIdx : primaryIdx};
    };

    loadStyle(FontStyle::Regular, "Noto Sans", false, false, baseSizePx);
    loadStyle(FontStyle::Bold, "Noto Sans", true, false, baseSizePx);
    loadStyle(FontStyle::Italic, "Noto Sans", false, true, baseSizePx);
    loadStyle(FontStyle::BoldItalic, "Noto Sans", true, true, baseSizePx);
    loadStyle(FontStyle::Code, "Noto Sans Mono", false, false, baseSizePx);
    loadStyle(FontStyle::ThinkingItalic, "Noto Sans", false, true, thinkingSizePx);

    int headingSizes[] = {
        baseSizePx + 12, baseSizePx + 10, baseSizePx + 8,
        baseSizePx + 6, baseSizePx + 4, baseSizePx + 2};
    loadStyle(FontStyle::Heading1, "Noto Sans", true, false, headingSizes[0]);
    loadStyle(FontStyle::Heading2, "Noto Sans", true, false, headingSizes[1]);
    loadStyle(FontStyle::Heading3, "Noto Sans", true, false, headingSizes[2]);
    loadStyle(FontStyle::Heading4, "Noto Sans", true, false, headingSizes[3]);
    loadStyle(FontStyle::Heading5, "Noto Sans", true, false, headingSizes[4]);
    loadStyle(FontStyle::Heading6, "Noto Sans", true, false, headingSizes[5]);

    atlasData_.resize(atlasW_ * atlasH_, 0);

    return !faces_.empty();
}

int FontManager::FaceIndex(FontStyle style, bool rtl) const {
    auto it = styleFaces_.find(style);
    if (it == styleFaces_.end()) {
        it = styleFaces_.find(FontStyle::Regular);
        if (it == styleFaces_.end()) return 0;
    }
    return rtl ? it->second.second : it->second.first;
}

float FontManager::LineHeight(FontStyle style) const {
    int idx = FaceIndex(style);
    return (idx >= 0 && idx < (int)faces_.size()) ? faces_[idx].lineHeight : 20;
}

float FontManager::Ascent(FontStyle style) const {
    int idx = FaceIndex(style);
    return (idx >= 0 && idx < (int)faces_.size()) ? faces_[idx].ascent : 16;
}

float FontManager::SpaceWidth(FontStyle style) const {
    int idx = FaceIndex(style);
    return (idx >= 0 && idx < (int)faces_.size()) ? faces_[idx].spaceWidth : 6;
}

static int DecodeUTF8(const char* p, uint32_t& cp) {
    uint8_t c = *p;
    if (c < 0x80) { cp = c; return 1; }
    if (c < 0xE0) { cp = ((c & 0x1F) << 6) | (p[1] & 0x3F); return 2; }
    if (c < 0xF0) { cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); return 3; }
    cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    return 4;
}

int FontManager::FindFallbackFace(uint32_t codepoint, int sizePx) const {
    uint64_t key = ((uint64_t)codepoint << 32) | sizePx;
    auto it = fallbackCache_.find(key);
    if (it != fallbackCache_.end()) return it->second;

    FcPattern* pat = FcPatternCreate();
    FcCharSet* cs = FcCharSetCreate();
    FcCharSetAddChar(cs, codepoint);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult res;
    FcPattern* match = FcFontMatch(nullptr, pat, &res);
    int faceIdx = -1;
    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
            faceIdx = const_cast<FontManager*>(this)->LoadFace((const char*)file, sizePx);
        FcPatternDestroy(match);
    }
    FcCharSetDestroy(cs);
    FcPatternDestroy(pat);
    fallbackCache_[key] = faceIdx;
    return faceIdx;
}

void FontManager::FillMissingGlyphs(ShapedRun& run, const std::string& text,
                                     int primaryFace, bool rtl) const {
    int sizePx = (int)(faces_[primaryFace].lineHeight + 0.5f);

    for (auto& g : run.glyphs) {
        if (g.glyphId != 0) continue;  // Has glyph — skip
        if (g.cluster >= text.size()) continue;

        // Decode codepoint at this cluster position
        uint32_t cp;
        DecodeUTF8(text.c_str() + g.cluster, cp);
        if (cp == 0) continue;

        // Find a fallback font that has this codepoint
        int fbFace = FindFallbackFace(cp, sizePx);
        if (fbFace < 0 || fbFace >= (int)faces_.size()) continue;

        // Get glyph index from fallback font
        FT_UInt glyphIndex = FT_Get_Char_Index(faces_[fbFace].ft, cp);
        if (glyphIndex == 0) continue;  // Fallback font doesn't have it either

        // Replace glyph with fallback version
        g.glyphId = glyphIndex;
        g.faceIdx = fbFace;

        // Get correct advance from fallback font
        if (FT_Load_Glyph(faces_[fbFace].ft, glyphIndex, FT_LOAD_DEFAULT) == 0) {
            float newAdvance = faces_[fbFace].ft->glyph->advance.x / 64.0f;
            float diff = newAdvance - g.xAdvance;
            g.xAdvance = newAdvance;
            // Shift all subsequent glyphs
            for (auto& g2 : run.glyphs) {
                if (&g2 > &g) g2.xPos += diff;
            }
            run.totalWidth += diff;
        }

        // Update cached pointer
        g.cached = &EnsureGlyph(g.glyphId, g.faceIdx);
    }
}

ShapedRun FontManager::Shape(const std::string& text, FontStyle style, bool rtl) const {
    ShapedRun result;
    if (text.empty()) return result;

    int fIdx = FaceIndex(style, rtl);
    if (fIdx < 0 || fIdx >= (int)faces_.size()) return result;

    const Face& face = faces_[fIdx];

    // Step 1: Shape entire text with primary font (preserves ligatures, Arabic forms)
    hb_buffer_t* buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text.c_str(), (int)text.size(), 0, (int)text.size());
    hb_buffer_set_direction(buf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(face.hb, buf, nullptr, 0);

    unsigned int glyphCount = 0;
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &glyphCount);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &glyphCount);

    float cursorX = 0;
    result.glyphs.resize(glyphCount);
    bool hasMissing = false;

    for (unsigned int i = 0; i < glyphCount; i++) {
        auto& g = result.glyphs[i];
        g.glyphId = info[i].codepoint;
        g.cluster = info[i].cluster;
        g.xPos = cursorX + pos[i].x_offset / 64.0f;
        g.yPos = pos[i].y_offset / 64.0f;
        g.xAdvance = pos[i].x_advance / 64.0f;
        g.faceIdx = fIdx;
        g.cached = &EnsureGlyph(g.glyphId, fIdx);
        cursorX += pos[i].x_advance / 64.0f;
        if (g.glyphId == 0) hasMissing = true;
    }
    result.totalWidth = cursorX;
    hb_buffer_destroy(buf);

    // Step 2: Fill missing glyphs with fallback fonts (per-glyph, not per-run)
    if (hasMissing) FillMissingGlyphs(result, text, fIdx, rtl);

    return result;
}

float FontManager::MeasureWidth(const std::string& text, FontStyle style, bool rtl) const {
    return Shape(text, style, rtl).totalWidth;
}

const GlyphInfo& FontManager::EnsureGlyph(uint32_t glyphId, int faceIdx) const {
    uint64_t key = Key(glyphId, faceIdx);

    // Flat open-addressing lookup (cache-friendly linear probing)
    size_t idx = (size_t)(key * 0x9E3779B97F4A7C15ULL) & CACHE_MASK;
    for (size_t i = 0; i < 64; i++) {  // Max 64 probes
        auto& slot = cache_[idx];
        if (slot.key == key) return slot.info;      // Hit
        if (slot.key == ~0ULL) break;               // Empty = miss
        idx = (idx + 1) & CACHE_MASK;
    }

    // Cache miss: rasterize glyph
    GlyphInfo gi{};
    if (faceIdx >= 0 && faceIdx < (int)faces_.size()) {
        FT_Face ft = faces_[faceIdx].ft;
        // Check if this face has color bitmaps (emoji font)
        bool hasColorBitmaps = FT_HAS_COLOR(ft);
        FT_Int32 loadFlags = FT_LOAD_RENDER;
        if (hasColorBitmaps) loadFlags |= FT_LOAD_COLOR;

        if (FT_Load_Glyph(ft, glyphId, loadFlags) == 0 && ft->glyph->bitmap.buffer) {
            bool isColor = (ft->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA);
            FT_GlyphSlot slot = ft->glyph;
            int w = (int)slot->bitmap.width;
            int h = (int)slot->bitmap.rows;

            if (w > 0 && h > 0) {
                if (curX_ + w + 1 >= atlasW_) {
                    curX_ = 1;
                    curY_ += rowH_ + 1;
                    rowH_ = 0;
                }
                if (curY_ + h + 1 < atlasH_) {
                    if (isColor) {
                        for (int r = 0; r < h; r++) {
                            const uint8_t* src = slot->bitmap.buffer + r * slot->bitmap.pitch;
                            uint8_t* dst = &atlasData_[(curY_ + r) * atlasW_ + curX_];
                            for (int c = 0; c < w; c++) {
                                uint8_t b=src[c*4], g=src[c*4+1], rv=src[c*4+2], a=src[c*4+3];
                                dst[c] = a > 0 ? (uint8_t)((rv*77 + g*150 + b*29) >> 8) : 0;
                            }
                        }
                    } else {
                        for (int r = 0; r < h; r++)
                            memcpy(&atlasData_[(curY_ + r) * atlasW_ + curX_],
                                   &slot->bitmap.buffer[r * slot->bitmap.pitch], w);
                    }

                    gi.atlasX = curX_;
                    gi.atlasY = curY_;
                    gi.bearingX = slot->bitmap_left;
                    gi.bearingY = slot->bitmap_top;
                    gi.width = w;
                    gi.height = h;

                    curX_ += w + 1;
                    if (h > rowH_) rowH_ = h;
                }
            }
        }
    }

    // Insert into flat cache
    idx = (size_t)(key * 0x9E3779B97F4A7C15ULL) & CACHE_MASK;
    for (size_t i = 0; i < 64; i++) {
        auto& s = cache_[idx];
        if (s.key == ~0ULL || s.key == key) {
            s.key = key;
            s.info = gi;
            atlasGen_++;
            return s.info;
        }
        idx = (idx + 1) & CACHE_MASK;
    }

    // Fallback: overwrite first slot (shouldn't happen with 8192 slots)
    auto& s = cache_[(size_t)(key * 0x9E3779B97F4A7C15ULL) & CACHE_MASK];
    s.key = key;
    s.info = gi;
    atlasGen_++;
    return s.info;
}

std::vector<float> FontManager::CaretPositionsFromRun(const ShapedRun& run, const std::string& text, bool rtl) const {
    int charCount = utf8_codepoint_count(text);
    std::vector<float> caretX(charCount + 1, 0.0f);
    if (text.empty() || run.glyphs.empty()) return caretX;

    if (!rtl) {
        std::vector<float> byteToX(text.size() + 1, -1.0f);
        float x = 0;
        for (auto& g : run.glyphs) {
            if (g.cluster <= text.size() && byteToX[g.cluster] < 0)
                byteToX[g.cluster] = x;
            x += g.xAdvance;
        }
        byteToX[text.size()] = run.totalWidth;
        float last = 0;
        for (size_t i = 0; i <= text.size(); i++) {
            if (byteToX[i] >= 0) last = byteToX[i];
            else byteToX[i] = last;
        }
        caretX[0] = 0;
        for (int i = 1; i <= charCount; i++) {
            size_t byteOff = utf8_char_to_byte(text, i);
            caretX[i] = (byteOff <= text.size()) ? byteToX[byteOff] : run.totalWidth;
        }
    } else {
        std::vector<float> byteAdv(text.size() + 1, 0.0f);
        for (auto& g : run.glyphs)
            if (g.cluster <= text.size()) byteAdv[g.cluster] += g.xAdvance;
        caretX[0] = run.totalWidth;
        for (int i = 0; i < charCount; i++) {
            size_t byteOff = utf8_char_to_byte(text, i);
            float adv = (byteOff <= text.size()) ? byteAdv[byteOff] : 0;
            caretX[i + 1] = caretX[i] - adv;
        }
    }
    return caretX;
}

std::vector<float> FontManager::CaretPositions(const std::string& text, FontStyle style, bool rtl) const {
    if (text.empty()) return std::vector<float>(1, 0.0f);
    auto run = Shape(text, style, rtl);
    return CaretPositionsFromRun(run, text, rtl);
}
