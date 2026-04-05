#include "widgets.h"
#include <cmath>
#include <algorithm>
#include <xkbcommon/xkbcommon-keysyms.h>

// ============================================================================
// Label
// ============================================================================

void Label::Paint(GLRenderer& r, FontManager& fm) const {
    if (text.empty()) return;
    auto run = fm.Shape(text, style);
    float y = bounds.y + (bounds.h - fm.LineHeight(style)) / 2;
    r.DrawShapedRun(fm, run, bounds.x, y, fm.Ascent(style), color);
}

// ============================================================================
// Button
// ============================================================================

void Button::Paint(GLRenderer& r, FontManager& fm) const {
    if (!visible) return;
    float radius = 6;
    float padX = 12;  // Horizontal padding
    Color bg = !enabled ? bgColor : pressed ? pressColor : hovered ? hoverColor : bgColor;
    r.DrawRoundedRect(bounds.x, bounds.y, bounds.w, bounds.h, radius, bg);

    if (!text.empty()) {
        Color tc = enabled ? textColor : disabledText;
        auto run = fm.Shape(text, style);
        float tx = bounds.x + (bounds.w - run.totalWidth) / 2;
        float ty = bounds.y + (bounds.h - fm.LineHeight(style)) / 2;
        r.DrawShapedRun(fm, run, tx, ty, fm.Ascent(style), tc);
    }
}

bool Button::OnMouseDown(float x, float y) {
    if (!visible || !enabled) return false;
    if (PointInRect(x, y, bounds)) { pressed = true; return true; }
    return false;
}

bool Button::OnMouseUp(float x, float y) {
    if (!visible || !enabled) return false;
    if (pressed) {
        pressed = false;
        if (PointInRect(x, y, bounds) && onClick) { onClick(); return true; }
    }
    return false;
}

void Button::OnMouseMove(float x, float y) {
    if (!visible || !enabled) return;
    hovered = PointInRect(x, y, bounds);
}

// ============================================================================
// TextInput
// ============================================================================

std::string TextInput::DisplayText() const {
    if (password) {
        int count = 0;
        for (size_t i = 0; i < text.size(); ) {
            unsigned char c = text[i];
            if (c < 0x80) i++; else if (c < 0xE0) i+=2; else if (c < 0xF0) i+=3; else i+=4;
            count++;
        }
        std::string result;
        for (int j = 0; j < count; j++) result += "\xe2\x80\xa2";
        return result;
    }
    return text;
}

// Get byte offset for the Nth codepoint
static int ByteOffsetForCodepoint(const std::string& s, int cp) {
    int idx = 0;
    for (int i = 0; i < cp && idx < (int)s.size(); i++) {
        unsigned char c = s[idx];
        if (c < 0x80) idx++; else if (c < 0xE0) idx+=2; else if (c < 0xF0) idx+=3; else idx+=4;
    }
    return std::min(idx, (int)s.size());
}

// Count codepoints in byte range [0, bytePos)
static int CodepointCount(const std::string& s, int bytePos) {
    int count = 0;
    int idx = 0;
    while (idx < bytePos && idx < (int)s.size()) {
        unsigned char c = s[idx];
        if (c < 0x80) idx++; else if (c < 0xE0) idx+=2; else if (c < 0xF0) idx+=3; else idx+=4;
        count++;
    }
    return count;
}

// Move cursor one codepoint left
static int PrevCP(const std::string& s, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && (s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

// Move cursor one codepoint right
static int NextCP(const std::string& s, int pos) {
    if (pos >= (int)s.size()) return s.size();
    pos++;
    while (pos < (int)s.size() && (s[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

void TextInput::Paint(GLRenderer& r, FontManager& fm, float time) const {
    float radius = 6;
    float pad = 8;

    // Border (rounded rect outline: larger rect behind, then fill)
    Color bc = focused ? focusBorder : borderColor;
    r.DrawRoundedRect(bounds.x - 1, bounds.y - 1, bounds.w + 2, bounds.h + 2, radius + 1, bc);
    r.DrawRoundedRect(bounds.x, bounds.y, bounds.w, bounds.h, radius, bgColor);

    float ty = bounds.y + (bounds.h - fm.LineHeight(style)) / 2;

    if (text.empty() && !focused) {
        auto run = fm.Shape(placeholder, style);
        r.DrawShapedRun(fm, run, bounds.x + pad, ty, fm.Ascent(style), placeholderColor);
    } else {
        std::string display = DisplayText();

        // Draw selection highlight
        if (selStart != selEnd && !display.empty()) {
            int s0 = std::min(selStart, selEnd);
            int s1 = std::max(selStart, selEnd);
            std::string before = display.substr(0, ByteOffsetForCodepoint(display, s0));
            std::string selected = display.substr(ByteOffsetForCodepoint(display, s0),
                ByteOffsetForCodepoint(display, s1) - ByteOffsetForCodepoint(display, s0));
            float x0 = bounds.x + pad + fm.MeasureWidth(before, style);
            float x1 = x0 + fm.MeasureWidth(selected, style);
            r.DrawRect(x0, ty, x1 - x0, fm.LineHeight(style), {0.2f, 0.4f, 0.8f, 0.5f});
        }

        if (!display.empty()) {
            auto run = fm.Shape(display, style);
            r.DrawShapedRun(fm, run, bounds.x + pad, ty, fm.Ascent(style), textColor);
        }

        // Cursor
        if (focused) {
            float blink = std::fmod(cursorBlink, 1.0f);
            if (blink < 0.5f) {
                float cx = bounds.x + pad;
                if (cursorPos > 0 && !display.empty()) {
                    int cpIdx = CodepointCount(display, std::min(cursorPos, (int)display.size()));
                    std::string before = display.substr(0, ByteOffsetForCodepoint(display, cpIdx));
                    cx += fm.MeasureWidth(before, style);
                }
                float cursorH = fm.LineHeight(style) * 0.8f;
                float cursorY = ty + fm.LineHeight(style) * 0.1f;
                r.DrawRect(cx, cursorY, 1.5f, cursorH, cursorColor);
            }
        }
    }
}

// Hit-test: find codepoint index at pixel X within text
static int HitTestText(const std::string& text, FontStyle style, FontManager& fm, float localX) {
    if (text.empty() || localX <= 0) return 0;
    float total = fm.MeasureWidth(text, style);
    if (localX >= total) return CodepointCount(text, text.size());

    // Binary search by codepoint
    int cpCount = CodepointCount(text, text.size());
    int lo = 0, hi = cpCount;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int byteOff = ByteOffsetForCodepoint(text, mid);
        float w = fm.MeasureWidth(text.substr(0, byteOff), style);
        if (w < localX) lo = mid + 1; else hi = mid;
    }
    // Snap to nearest side
    if (lo > 0) {
        int bPrev = ByteOffsetForCodepoint(text, lo - 1);
        int bCurr = ByteOffsetForCodepoint(text, lo);
        float wPrev = fm.MeasureWidth(text.substr(0, bPrev), style);
        float wCurr = fm.MeasureWidth(text.substr(0, bCurr), style);
        if (localX - wPrev < wCurr - localX) lo--;
    }
    return lo;
}

bool TextInput::OnMouseDown(float x, float y) {
    focused = PointInRect(x, y, bounds);
    if (!focused) return false;

    float pad = 8;
    float localX = x - bounds.x - pad;
    // Needs FontManager — caller should call OnMouseDrag after for proper placement
    // For now, set cursor to end; OnMouseDrag with fm will refine
    cursorPos = text.size();
    selStart = selEnd = CodepointCount(text, cursorPos);
    cursorBlink = 0;
    return true;
}

void TextInput::OnMouseDrag(float x, float y, FontManager& fm) {
    if (!focused) return;
    float pad = 8;
    float localX = x - bounds.x - pad;
    std::string display = DisplayText();
    int cp = HitTestText(display, style, fm, localX);
    cursorPos = ByteOffsetForCodepoint(text, cp);
    selEnd = cp;
    cursorBlink = 0;
}

void TextInput::OnChar(uint32_t codepoint) {
    if (!focused) return;

    // Delete selected text first
    if (selStart != selEnd) {
        int s0 = std::min(selStart, selEnd);
        int s1 = std::max(selStart, selEnd);
        int b0 = ByteOffsetForCodepoint(text, s0);
        int b1 = ByteOffsetForCodepoint(text, s1);
        text.erase(b0, b1 - b0);
        cursorPos = b0;
        selStart = selEnd = s0;
    }

    // Encode UTF-8
    char buf[5] = {};
    int len = 0;
    if (codepoint < 0x80) { buf[0] = codepoint; len = 1; }
    else if (codepoint < 0x800) {
        buf[0] = 0xC0 | (codepoint >> 6); buf[1] = 0x80 | (codepoint & 0x3F); len = 2;
    } else if (codepoint < 0x10000) {
        buf[0] = 0xE0 | (codepoint >> 12); buf[1] = 0x80 | ((codepoint >> 6) & 0x3F);
        buf[2] = 0x80 | (codepoint & 0x3F); len = 3;
    } else {
        buf[0] = 0xF0 | (codepoint >> 18); buf[1] = 0x80 | ((codepoint >> 12) & 0x3F);
        buf[2] = 0x80 | ((codepoint >> 6) & 0x3F); buf[3] = 0x80 | (codepoint & 0x3F); len = 4;
    }

    text.insert(cursorPos, buf, len);
    cursorPos += len;
    selStart = selEnd = CodepointCount(text, cursorPos);
    cursorBlink = 0;
}

void TextInput::OnKey(int key, int mods) {
    if (!focused) return;
    bool ctrl = (mods & 1);  // Mod::Ctrl
    bool shift = (mods & 2); // Mod::Shift

    // Ctrl+V paste
    if (ctrl && key == 'V') {
        if (onPaste) {
            // Delete selection first
            if (selStart != selEnd) {
                int s0 = std::min(selStart, selEnd);
                int s1 = std::max(selStart, selEnd);
                int b0 = ByteOffsetForCodepoint(text, s0);
                int b1 = ByteOffsetForCodepoint(text, s1);
                text.erase(b0, b1 - b0);
                cursorPos = b0;
                selStart = selEnd = s0;
            }
            std::string clip = onPaste();
            if (!clip.empty()) {
                for (auto& c : clip) if (c == '\n' || c == '\r') c = ' ';
                text.insert(cursorPos, clip);
                cursorPos += clip.size();
                selStart = selEnd = CodepointCount(text, cursorPos);
            }
        }
        return;
    }

    // Ctrl+A select all
    if (ctrl && key == 'A') {
        selStart = 0;
        selEnd = CodepointCount(text, text.size());
        cursorPos = text.size();
        cursorBlink = 0;
        return;
    }

    if (key == XKB_KEY_Return || key == XKB_KEY_KP_Enter) {
        if (onSubmit && !text.empty()) onSubmit(text);
        return;
    }

    if (key == XKB_KEY_BackSpace) {
        if (selStart != selEnd) {
            // Delete selection
            int s0 = std::min(selStart, selEnd);
            int s1 = std::max(selStart, selEnd);
            int b0 = ByteOffsetForCodepoint(text, s0);
            int b1 = ByteOffsetForCodepoint(text, s1);
            text.erase(b0, b1 - b0);
            cursorPos = b0;
            selStart = selEnd = s0;
        } else if (cursorPos > 0) {
            int prev = PrevCP(text, cursorPos);
            text.erase(prev, cursorPos - prev);
            cursorPos = prev;
            selStart = selEnd = CodepointCount(text, cursorPos);
        }
        cursorBlink = 0;
        return;
    }

    if (key == XKB_KEY_Delete) {
        if (selStart != selEnd) {
            int s0 = std::min(selStart, selEnd);
            int s1 = std::max(selStart, selEnd);
            int b0 = ByteOffsetForCodepoint(text, s0);
            int b1 = ByteOffsetForCodepoint(text, s1);
            text.erase(b0, b1 - b0);
            cursorPos = b0;
            selStart = selEnd = s0;
        } else if (cursorPos < (int)text.size()) {
            int next = NextCP(text, cursorPos);
            text.erase(cursorPos, next - cursorPos);
            selStart = selEnd = CodepointCount(text, cursorPos);
        }
        cursorBlink = 0;
        return;
    }

    if (key == XKB_KEY_Left) {
        if (cursorPos > 0) {
            cursorPos = PrevCP(text, cursorPos);
            int cp = CodepointCount(text, cursorPos);
            if (shift) { selEnd = cp; }
            else { selStart = selEnd = cp; }
        }
        cursorBlink = 0;
        return;
    }

    if (key == XKB_KEY_Right) {
        if (cursorPos < (int)text.size()) {
            cursorPos = NextCP(text, cursorPos);
            int cp = CodepointCount(text, cursorPos);
            if (shift) { selEnd = cp; }
            else { selStart = selEnd = cp; }
        }
        cursorBlink = 0;
        return;
    }

    if (key == XKB_KEY_Home) {
        cursorPos = 0;
        int cp = 0;
        if (shift) { selEnd = cp; }
        else { selStart = selEnd = cp; }
        cursorBlink = 0;
        return;
    }

    if (key == XKB_KEY_End) {
        cursorPos = text.size();
        int cp = CodepointCount(text, cursorPos);
        if (shift) { selEnd = cp; }
        else { selStart = selEnd = cp; }
        cursorBlink = 0;
        return;
    }
}

void TextInput::Update(float dt) {
    if (focused) cursorBlink += dt;
}

// ============================================================================
// Dropdown
// ============================================================================

WidgetRect Dropdown::PopupRect() const {
    return {bounds.x, bounds.y - PopupHeight(), bounds.w, PopupHeight()};
}

void Dropdown::Paint(GLRenderer& r, FontManager& fm) const {
    if (!enabled) return;
    float radius = 6;

    Color bg = open ? hoverColor : hovered ? hoverColor : bgColor;
    r.DrawRoundedRect(bounds.x, bounds.y, bounds.w, bounds.h, radius, bg);

    // Selected text
    std::string display = SelectedLabel();
    if (display.empty()) display = "Select...";

    auto run = fm.Shape(display, style);
    float tx = bounds.x + 8;
    float ty = bounds.y + (bounds.h - fm.LineHeight(style)) / 2;
    r.DrawShapedRun(fm, run, tx, ty, fm.Ascent(style), textColor);

    // Dropdown arrow
    float triSize = fm.LineHeight(style) * 0.3f;
    float triX = bounds.x + bounds.w - 12;
    float triY = bounds.y + bounds.h / 2;
    r.DrawTriDown(triX, triY, triSize, textColor);
}

void Dropdown::PaintPopup(GLRenderer& r, FontManager& fm) const {
    if (!open || items.empty()) return;
    float radius = 6;

    WidgetRect pr = PopupRect();
    r.DrawRoundedRect(pr.x, pr.y, pr.w, pr.h, radius, popupBg);

    float itemH = ItemHeight();
    for (int i = 0; i < (int)items.size(); i++) {
        float iy = pr.y + i * itemH;
        if (i == hoveredItem) {
            r.DrawRect(pr.x + 2, iy, pr.w - 4, itemH, popupHover);
        }
        if (i == selectedIndex) {
            r.DrawRect(pr.x, iy, 3, itemH, {0.39f, 0.71f, 1.0f});
        }
        auto irun = fm.Shape(items[i].label, style);
        float ity = iy + (itemH - fm.LineHeight(style)) / 2;
        r.DrawShapedRun(fm, irun, pr.x + 10, ity, fm.Ascent(style), textColor);
    }
}

bool Dropdown::OnMouseDown(float x, float y) {
    if (!enabled) return false;

    if (open) {
        WidgetRect pr = PopupRect();
        if (PointInRect(x, y, pr)) {
            int idx = (int)((y - pr.y) / ItemHeight());
            if (idx >= 0 && idx < (int)items.size()) {
                selectedIndex = idx;
                if (onSelect) onSelect(idx, items[idx].id);
            }
            Close();
            return true;
        }
        Close();
        return true;
    }

    if (PointInRect(x, y, bounds)) { open = true; return true; }
    return false;
}

bool Dropdown::OnMouseUp(float, float) { return false; }

void Dropdown::OnMouseMove(float x, float y) {
    hovered = PointInRect(x, y, bounds);
    if (open) {
        WidgetRect pr = PopupRect();
        if (PointInRect(x, y, pr)) {
            hoveredItem = (int)((y - pr.y) / ItemHeight());
        } else {
            hoveredItem = -1;
        }
    }
}
