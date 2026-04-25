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

#include "font.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <map>

#if defined(GRIT_LINUX) || defined(GRIT_FREEBSD)
#include <fontconfig/fontconfig.h>
#elif defined(GRIT_MACOS)
#include <CoreText/CoreText.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

FontManager::FontManager() = default;

FontManager::~FontManager() {
    for (auto& f : faces_) {
        if (f.hb) hb_font_destroy(f.hb);
        if (f.ft) FT_Done_Face(f.ft);
    }
    if (ft_) FT_Done_FreeType(ft_);
}

#if defined(GRIT_LINUX) || defined(GRIT_FREEBSD)

FontManager::FontMatch FontManager::FindFont(const char* family, bool bold, bool italic) const {
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
    FontMatch out;
    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
            out.path = (const char*)file;
        int idx = 0;
        if (FcPatternGetInteger(match, FC_INDEX, 0, &idx) == FcResultMatch)
            out.index = idx;
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pat);
    return out;
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
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
            std::string path((const char*)file);
            int fcIndex = 0;
            FcPatternGetInteger(match, FC_INDEX, 0, &fcIndex);
            std::string faceKey = path + ":" + std::to_string(sizePx) + ":" + std::to_string(fcIndex);
            auto fit = loadedFaces_.find(faceKey);
            if (fit != loadedFaces_.end()) {
                faceIdx = fit->second;
            } else {
                faceIdx = const_cast<FontManager*>(this)->LoadFace(path, sizePx, fcIndex);
                if (faceIdx >= 0)
                    loadedFaces_[faceKey] = faceIdx;
            }
        }
        FcPatternDestroy(match);
    }
    FcCharSetDestroy(cs);
    FcPatternDestroy(pat);
    fallbackCache_[key] = faceIdx;
    return faceIdx;
}

static void InitFontDiscovery() { FcInit(); }

#elif defined(GRIT_MACOS)

// Helper: get file path from a CTFont
static std::string CTFontGetFilePath(CTFontRef font) {
    CFURLRef url = (CFURLRef)CTFontCopyAttribute(font, kCTFontURLAttribute);
    if (!url) return {};
    char buf[1024];
    std::string path;
    if (CFURLGetFileSystemRepresentation(url, true, (UInt8*)buf, sizeof(buf)))
        path = buf;
    CFRelease(url);
    return path;
}

// Helper: get family name from a CTFont as a C++ string
static std::string CTFontGetFamilyName(CTFontRef font) {
    CFStringRef name = CTFontCopyFamilyName(font);
    if (!name) return {};
    char buf[256];
    CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(name);
    return buf;
}

// Try to find a font by family name + traits. Returns file path or empty string.
static std::string CTFindByFamily(const char* family, CTFontSymbolicTraits traits) {
    CFStringRef name = CFStringCreateWithCString(nullptr, family, kCFStringEncodingUTF8);

    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kCTFontFamilyNameAttribute, name);

    if (traits) {
        CFMutableDictionaryRef traitDict = CFDictionaryCreateMutable(
            nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        int32_t t = (int32_t)traits;
        CFNumberRef traitNum = CFNumberCreate(nullptr, kCFNumberSInt32Type, &t);
        CFDictionarySetValue(traitDict, kCTFontSymbolicTrait, traitNum);
        CFDictionarySetValue(attrs, kCTFontTraitsAttribute, traitDict);
        CFRelease(traitNum);
        CFRelease(traitDict);
    }

    CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
    CTFontRef font = CTFontCreateWithFontDescriptor(desc, 12.0, nullptr);

    std::string path;
    if (font) {
        // CoreText always returns something — verify the family actually matches
        std::string foundFamily = CTFontGetFamilyName(font);
        if (foundFamily == family)
            path = CTFontGetFilePath(font);
        CFRelease(font);
    }
    CFRelease(desc);
    CFRelease(attrs);
    CFRelease(name);
    return path;
}

// macOS font family equivalents for fonts the app requests
static const struct { const char* requested; const char* macos; } FONT_MAP[] = {
    {"Noto Sans",        "Helvetica Neue"},
    {"Noto Sans Mono",   "Menlo"},
    {"Noto Sans Arabic", "Geeza Pro"},
    {nullptr, nullptr}
};

FontManager::FontMatch FontManager::FindFont(const char* family, bool bold, bool italic) const {
    CTFontSymbolicTraits traits = 0;
    if (bold) traits |= kCTFontBoldTrait;
    if (italic) traits |= kCTFontItalicTrait;

    FontMatch out;
    out.path = CTFindByFamily(family, traits);
    if (!out.path.empty()) return out;

    for (auto* m = FONT_MAP; m->requested; m++) {
        if (strcmp(family, m->requested) == 0) {
            out.path = CTFindByFamily(m->macos, traits);
            if (!out.path.empty()) return out;
        }
    }
    return {};
}

int FontManager::FindFallbackFace(uint32_t codepoint, int sizePx) const {
    uint64_t key = ((uint64_t)codepoint << 32) | sizePx;
    auto it = fallbackCache_.find(key);
    if (it != fallbackCache_.end()) return it->second;

    // Create a string from the codepoint to ask CoreText for a fallback
    UniChar utf16[2];
    int utf16Len;
    if (codepoint <= 0xFFFF) {
        utf16[0] = (UniChar)codepoint;
        utf16Len = 1;
    } else {
        codepoint -= 0x10000;
        utf16[0] = (UniChar)(0xD800 + (codepoint >> 10));
        utf16[1] = (UniChar)(0xDC00 + (codepoint & 0x3FF));
        utf16Len = 2;
    }

    CFStringRef str = CFStringCreateWithCharacters(nullptr, utf16, utf16Len);

    // Use a base font to ask CoreText for a fallback
    CTFontRef baseFont = CTFontCreateWithName(CFSTR("Helvetica"), (CGFloat)sizePx, nullptr);
    CTFontRef fallback = CTFontCreateForString(baseFont, str, CFRangeMake(0, utf16Len));

    int faceIdx = -1;
    if (fallback) {
        std::string path = CTFontGetFilePath(fallback);
        if (!path.empty()) {
            std::string faceKey = path + ":" + std::to_string(sizePx);
            auto fit = loadedFaces_.find(faceKey);
            if (fit != loadedFaces_.end()) {
                faceIdx = fit->second;
            } else {
                faceIdx = const_cast<FontManager*>(this)->LoadFace(path, sizePx);
                if (faceIdx >= 0)
                    loadedFaces_[faceKey] = faceIdx;
            }
        }
        CFRelease(fallback);
    }
    CFRelease(baseFont);
    CFRelease(str);

    fallbackCache_[key] = faceIdx;
    return faceIdx;
}

static void InitFontDiscovery() { /* CoreText needs no init */ }

#endif

int FontManager::LoadFace(const std::string& path, int sizePx, long faceIndex) {
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

    // Fontconfig encodes variable-font named instances in the high 16 bits of
    // FC_INDEX (e.g. 0x00040000 = base face 0, instance 4). FreeType accepts
    // that layout directly in FT_New_Memory_Face, which is the only way to
    // get real bold weight out of a single-file variable font like
    // NotoSans[wght].ttf.
    Face f;
    if (FT_New_Memory_Face(ft_, buf->data(), (FT_Long)buf->size(),
                           (FT_Long)faceIndex, &f.ft)) {
        fprintf(stderr, "Failed to load font: %s (index %ld)\n", path.c_str(), faceIndex);
        return -1;
    }

    // For bitmap-only fonts (emoji), select the nearest available strike
    if (FT_HAS_FIXED_SIZES(f.ft) && !FT_IS_SCALABLE(f.ft)) {
        int bestIdx = 0;
        int bestDiff = 99999;
        for (int si = 0; si < f.ft->num_fixed_sizes; si++) {
            int diff = abs(f.ft->available_sizes[si].height - sizePx);
            if (diff < bestDiff) { bestDiff = diff; bestIdx = si; }
        }
        FT_Select_Size(f.ft, bestIdx);
        f.bitmapScale = (float)sizePx / f.ft->available_sizes[bestIdx].height;
    } else {
        FT_Set_Pixel_Sizes(f.ft, 0, sizePx);
        f.bitmapScale = 1.0f;
    }

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
    // Reset: Init may be called again when the display scale changes (HiDPI).
    // Without this reset, stale faces at the wrong pixel size accumulate and
    // the atlas keeps old glyph positions that no longer match the new sizes.
    for (auto& f : faces_) {
        if (f.hb) hb_font_destroy(f.hb);
        if (f.ft) FT_Done_Face(f.ft);
    }
    faces_.clear();
    styleFaces_.clear();
    loadedFaces_.clear();
    fallbackCache_.clear();
    cache_.assign(CACHE_SIZE, CacheSlot{});
    curX_ = 1; curY_ = 1; rowH_ = 0;
    std::fill(atlasData_.begin(), atlasData_.end(), 0);
    atlasGen_++;  // Force renderer to re-upload the (now-cleared) atlas

    if (!ft_ && FT_Init_FreeType(&ft_)) return false;
    InitFontDiscovery();

    int thinkingSizePx = std::max(10, baseSizePx - 3);

    auto loadStyle = [&](FontStyle style, const char* family, bool bold, bool italic, int sizePx) {
        FontMatch m = FindFont(family, bold, italic);
        if (m.path.empty()) {
            fprintf(stderr, "Font not found: %s bold=%d italic=%d\n", family, bold, italic);
            return;
        }
        int primaryIdx = LoadFace(m.path, sizePx, m.index);

        FontMatch am = FindFont("Noto Sans Arabic", bold, false);
        int arabicIdx = -1;
        if (!am.path.empty())
            arabicIdx = LoadFace(am.path, sizePx, am.index);

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

float FontManager::Descent(FontStyle style) const {
    int idx = FaceIndex(style);
    return (idx >= 0 && idx < (int)faces_.size()) ? faces_[idx].descent : 4;
}

float FontManager::VisibleHeight(FontStyle style) const {
    int idx = FaceIndex(style);
    if (idx < 0 || idx >= (int)faces_.size()) return 20;
    return faces_[idx].ascent + faces_[idx].descent;
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

ShapedRun FontManager::Shape(const std::string& text, FontStyle style, bool rtl) const {
    ShapedRun result;
    if (text.empty()) return result;

    int primaryIdx = FaceIndex(style, rtl);
    if (primaryIdx < 0 || primaryIdx >= (int)faces_.size()) return result;

    int sizePx = (int)(faces_[primaryIdx].lineHeight + 0.5f);

    // Step 1: Resolve font per codepoint, group consecutive same-font chars into runs
    struct FontRun { size_t byteStart, byteEnd; int faceIdx; };
    std::vector<FontRun> fontRuns;

    const char* p = text.c_str();
    const char* textEnd = p + text.size();
    int prevFallback = -1;
    bool prevWasZwj = false;

    while (p < textEnd) {
        size_t off = p - text.c_str();
        uint32_t cp;
        int len = DecodeUTF8(p, cp);

        int face = primaryIdx;
        if (FT_Get_Char_Index(faces_[primaryIdx].ft, cp) == 0) {
            int fb = FindFallbackFace(cp, sizePx);
            if (fb >= 0) face = fb;
        }

        // Emoji sequence continuations inherit previous fallback font:
        // - Character immediately after ZWJ (e.g., ♂ in 🤷‍♂️)
        // - ZWJ itself, variation selectors, skin tone modifiers, tag chars
        if (prevFallback >= 0 &&
            (prevWasZwj ||
             cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x20E3 ||
             (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
             (cp >= 0xE0020 && cp <= 0xE007F))) {
            face = prevFallback;
        }

        prevWasZwj = (cp == 0x200D);
        prevFallback = (face != primaryIdx) ? face : -1;

        p += len;
        size_t byteEnd = p - text.c_str();

        if (!fontRuns.empty() && fontRuns.back().faceIdx == face)
            fontRuns.back().byteEnd = byteEnd;
        else
            fontRuns.push_back({off, byteEnd, face});
    }

    // Step 2: Shape each font run independently
    // For RTL, process runs in reverse order for correct visual placement.
    // Uses hb_buffer_add_utf8 with full text context so Arabic joining
    // works correctly across run boundaries.
    float cursorX = 0;
    int rStart = rtl ? (int)fontRuns.size() - 1 : 0;
    int rEnd   = rtl ? -1 : (int)fontRuns.size();
    int rStep  = rtl ? -1 : 1;

    for (int ri = rStart; ri != rEnd; ri += rStep) {
        auto& fr = fontRuns[ri];

        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, text.c_str(), (int)text.size(),
                           (unsigned int)fr.byteStart, (int)(fr.byteEnd - fr.byteStart));
        hb_buffer_set_direction(buf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_guess_segment_properties(buf);
        hb_shape(faces_[fr.faceIdx].hb, buf, nullptr, 0);

        unsigned int count = 0;
        hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
        hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &count);

        float scale = faces_[fr.faceIdx].bitmapScale;
        for (unsigned int i = 0; i < count; i++) {
            ShapedGlyph g;
            g.glyphId = info[i].codepoint;
            g.cluster = info[i].cluster;  // Absolute byte offset (hb context)
            g.xPos = cursorX + pos[i].x_offset / 64.0f * scale;
            g.yPos = pos[i].y_offset / 64.0f * scale;
            g.xAdvance = pos[i].x_advance / 64.0f * scale;
            g.faceIdx = fr.faceIdx;
            g.cached = &EnsureGlyph(g.glyphId, fr.faceIdx);
            cursorX += g.xAdvance;
            result.glyphs.push_back(g);
        }

        hb_buffer_destroy(buf);
    }

    result.totalWidth = cursorX;
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
            int srcW = (int)slot->bitmap.width;
            int srcH = (int)slot->bitmap.rows;
            float scale = faces_[faceIdx].bitmapScale;

            // For bitmap fonts, scale down to target size
            int w = (scale < 0.99f) ? (int)(srcW * scale) : srcW;
            int h = (scale < 0.99f) ? (int)(srcH * scale) : srcH;
            if (w < 1) w = 1;
            if (h < 1) h = 1;

            if (w > 0 && h > 0) {
                if (curX_ + w + 1 >= atlasW_) {
                    curX_ = 1;
                    curY_ += rowH_ + 1;
                    rowH_ = 0;
                }
                if (curY_ + h + 1 < atlasH_) {
                    if (isColor) {
                        // Color bitmap → grayscale with nearest-neighbor scaling
                        for (int r = 0; r < h; r++) {
                            int sr = (scale < 0.99f) ? (int)(r / scale) : r;
                            if (sr >= srcH) sr = srcH - 1;
                            const uint8_t* src = slot->bitmap.buffer + sr * slot->bitmap.pitch;
                            uint8_t* dst = &atlasData_[(curY_ + r) * atlasW_ + curX_];
                            for (int c = 0; c < w; c++) {
                                int sc = (scale < 0.99f) ? (int)(c / scale) : c;
                                if (sc >= srcW) sc = srcW - 1;
                                uint8_t b=src[sc*4], g=src[sc*4+1], rv=src[sc*4+2], a=src[sc*4+3];
                                dst[c] = a > 0 ? (uint8_t)((rv*77 + g*150 + b*29) >> 8) : 0;
                            }
                        }
                    } else if (scale < 0.99f) {
                        // Grayscale bitmap with scaling
                        for (int r = 0; r < h; r++) {
                            int sr = (int)(r / scale);
                            if (sr >= srcH) sr = srcH - 1;
                            uint8_t* dst = &atlasData_[(curY_ + r) * atlasW_ + curX_];
                            for (int c = 0; c < w; c++) {
                                int sc = (int)(c / scale);
                                if (sc >= srcW) sc = srcW - 1;
                                dst[c] = slot->bitmap.buffer[sr * slot->bitmap.pitch + sc];
                            }
                        }
                    } else {
                        for (int r = 0; r < h; r++)
                            memcpy(&atlasData_[(curY_ + r) * atlasW_ + curX_],
                                   &slot->bitmap.buffer[r * slot->bitmap.pitch], w);
                    }

                    gi.atlasX = curX_;
                    gi.atlasY = curY_;
                    gi.bearingX = (int)(slot->bitmap_left * scale);
                    gi.bearingY = (int)(slot->bitmap_top * scale);
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
