// FastCode Native — GPU-rendered AI coding harness
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
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <ctime>

enum class BlockType { NORMAL, USER_PROMPT, THINKING, CODE, LOADING };

// Platform-independent key/modifier constants
namespace Key {
    constexpr int Escape = 1, Space = 2;
    constexpr int Up = 3, Down = 4, PageUp = 5, PageDown = 6;
    constexpr int Home = 7, End = 8;
    constexpr int A = 'A', C = 'C';
}
namespace Mod {
    constexpr int Ctrl = 1, Shift = 2;
}

inline double GetMonotonicTime() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

enum class FontStyle : uint8_t {
    Regular, Bold, Italic, BoldItalic, Code, ThinkingItalic,
    Heading1, Heading2, Heading3, Heading4, Heading5, Heading6,
    COUNT
};

struct Color {
    float r, g, b, a;
    Color() : r(0), g(0), b(0), a(1) {}
    Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    static Color RGB(int ri, int gi, int bi, int ai = 255) {
        return {ri / 255.f, gi / 255.f, bi / 255.f, ai / 255.f};
    }
};

struct StyledTextRun {
    std::string text;
    FontStyle style = FontStyle::Regular;
    Color color;
};

struct TextBlock {
    BlockType type;
    std::string text;
    std::vector<StyledTextRun> runs;
    bool isLoading = false;
    bool rightToLeft = false;
    int leftIndent = 0;
    int topSpacing = 0;
    int bottomSpacing = 0;
    bool isCollapsed = false;
    bool isExpandable = false;  // True if block wraps to more than 1 line

    TextBlock() : type(BlockType::NORMAL) {}
    TextBlock(BlockType t, const std::string& txt, bool rtl = false)
        : type(t), text(txt), rightToLeft(rtl), isCollapsed(t == BlockType::THINKING) {}

    bool HasStyledRuns() const { return !runs.empty(); }
    std::string GetFullText() const {
        if (runs.empty()) return text;
        std::string result;
        for (auto& run : runs) result += run.text;
        return result;
    }
};

struct TextPosition {
    int block = -1;
    int line = 0;
    int offset = 0;

    bool IsValid() const { return block >= 0; }
    bool operator==(const TextPosition& o) const {
        return block == o.block && line == o.line && offset == o.offset;
    }
    bool operator!=(const TextPosition& o) const { return !(*this == o); }
    bool operator<(const TextPosition& o) const {
        if (block != o.block) return block < o.block;
        if (line != o.line) return line < o.line;
        return offset < o.offset;
    }
    bool operator<=(const TextPosition& o) const { return !(o < *this); }
    bool operator>(const TextPosition& o) const { return o < *this; }
    bool operator>=(const TextPosition& o) const { return !(*this < o); }
};

struct WrappedLine {
    std::string text;
    bool rightToLeft = false;
    float x = 0, y = 0;
    float width = 0, height = 0;

    std::vector<StyledTextRun> styledRuns;
    std::vector<float> runXOffsets;

    mutable std::vector<float> caretX;
    mutable bool caretXValid = false;

    // Cached shaped runs - populated lazily, avoids re-shaping with HarfBuzz every frame.
    // For plain text: cachedShapeValid means the parent scroll_view has cached shapes for this line.
    // The actual ShapedRun data lives in scroll_view's per-line cache (avoids font.h dependency here).
    mutable bool shapedValid = false;
};

// --- UTF-8 helpers ---

inline int utf8_codepoint_count(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if (c < 0x80) i += 1;
        else if (c < 0xE0) i += 2;
        else if (c < 0xF0) i += 3;
        else i += 4;
        n++;
    }
    return n;
}

inline size_t utf8_char_to_byte(const std::string& s, int charIdx) {
    size_t pos = 0;
    for (int i = 0; i < charIdx && pos < s.size(); i++) {
        unsigned char c = s[pos];
        if (c < 0x80) pos += 1;
        else if (c < 0xE0) pos += 2;
        else if (c < 0xF0) pos += 3;
        else pos += 4;
    }
    return pos;
}

inline int utf8_byte_to_char(const std::string& s, size_t byteOff) {
    int n = 0;
    size_t pos = 0;
    while (pos < byteOff && pos < s.size()) {
        unsigned char c = s[pos];
        if (c < 0x80) pos += 1;
        else if (c < 0xE0) pos += 2;
        else if (c < 0xF0) pos += 3;
        else pos += 4;
        n++;
    }
    return n;
}

inline uint32_t utf8_decode_at(const std::string& s, size_t pos) {
    if (pos >= s.size()) return 0;
    unsigned char c = s[pos];
    if (c < 0x80) return c;
    if (c < 0xE0) return ((c & 0x1F) << 6) | (s[pos + 1] & 0x3F);
    if (c < 0xF0) return ((c & 0x0F) << 12) | ((s[pos + 1] & 0x3F) << 6) | (s[pos + 2] & 0x3F);
    return ((c & 0x07) << 18) | ((s[pos + 1] & 0x3F) << 12) | ((s[pos + 2] & 0x3F) << 6) | (s[pos + 3] & 0x3F);
}

inline bool DetectRTL(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        uint32_t cp = utf8_decode_at(s, i);
        unsigned char c = s[i];
        if (c < 0x80) i += 1;
        else if (c < 0xE0) i += 2;
        else if (c < 0xF0) i += 3;
        else i += 4;

        if ((cp >= 0x0590 && cp <= 0x05FF) || (cp >= 0x0600 && cp <= 0x06FF) ||
            (cp >= 0x0700 && cp <= 0x074F) || (cp >= 0x0750 && cp <= 0x077F) ||
            (cp >= 0x0780 && cp <= 0x07BF) || (cp >= 0x07C0 && cp <= 0x07FF) ||
            (cp >= 0x0800 && cp <= 0x083F) || (cp >= 0x0840 && cp <= 0x085F) ||
            (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
            (cp >= 0xFE70 && cp <= 0xFEFF))
            return true;
        if ((cp >= 0x0041 && cp <= 0x005A) || (cp >= 0x0061 && cp <= 0x007A) ||
            (cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x0370 && cp <= 0x03FF) ||
            (cp >= 0x0400 && cp <= 0x04FF) || (cp >= 0x0900 && cp <= 0x097F) ||
            (cp >= 0x3000 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF))
            return false;
    }
    return false;
}
