#include "widgets.h"
#include <cmath>
#include <algorithm>
#include <xkbcommon/xkbcommon-keysyms.h>

// ============================================================================
// Label
// ============================================================================

void Label::Paint(Renderer& r, FontManager& fm) const {
    if (text.empty()) return;
    auto run = fm.Shape(text, style);
    float y = bounds.y + (bounds.h - fm.LineHeight(style)) / 2;
    r.DrawShapedRun(fm, run, bounds.x, y, fm.Ascent(style), color);
}

// ============================================================================
// Button
// ============================================================================

void Button::Paint(Renderer& r, FontManager& fm) const {
    if (!visible) return;
    Color bg = !enabled ? bgColor : pressed ? pressColor : hovered ? hoverColor : bgColor;
    r.DrawRect(bounds.x, bounds.y, bounds.w, bounds.h, bg);

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
    if (PointInRect(x, y, bounds)) {
        pressed = true;
        return true;
    }
    return false;
}

bool Button::OnMouseUp(float x, float y) {
    if (!visible || !enabled) return false;
    if (pressed) {
        pressed = false;
        if (PointInRect(x, y, bounds) && onClick) {
            onClick();
            return true;
        }
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
        // Count UTF-8 chars and replace with bullets
        int count = 0;
        for (size_t i = 0; i < text.size(); ) {
            unsigned char c = text[i];
            if (c < 0x80) i += 1;
            else if (c < 0xE0) i += 2;
            else if (c < 0xF0) i += 3;
            else i += 4;
            count++;
        }
        // Use bullet character U+2022
        std::string result;
        for (int j = 0; j < count; j++) {
            result += "\xe2\x80\xa2";  // UTF-8 for •
        }
        return result;
    }
    return text;
}

void TextInput::Paint(Renderer& r, FontManager& fm, float time) const {
    // Background
    r.DrawRect(bounds.x, bounds.y, bounds.w, bounds.h, bgColor);

    // Border
    Color bc = focused ? focusBorder : borderColor;
    r.DrawRect(bounds.x, bounds.y, bounds.w, 1, bc);           // top
    r.DrawRect(bounds.x, bounds.y + bounds.h - 1, bounds.w, 1, bc); // bottom
    r.DrawRect(bounds.x, bounds.y, 1, bounds.h, bc);           // left
    r.DrawRect(bounds.x + bounds.w - 1, bounds.y, 1, bounds.h, bc); // right

    float pad = 8;
    float ty = bounds.y + (bounds.h - fm.LineHeight(style)) / 2;

    if (text.empty() && !focused) {
        // Placeholder
        auto run = fm.Shape(placeholder, style);
        r.DrawShapedRun(fm, run, bounds.x + pad, ty, fm.Ascent(style), placeholderColor);
    } else {
        // Text
        std::string display = DisplayText();
        if (!display.empty()) {
            auto run = fm.Shape(display, style);
            r.DrawShapedRun(fm, run, bounds.x + pad, ty, fm.Ascent(style), textColor);
        }

        // Cursor
        if (focused) {
            float blink = std::fmod(cursorBlink, 1.0f);
            if (blink < 0.5f) {
                // Measure text up to cursor position
                float cx = bounds.x + pad;
                if (cursorPos > 0 && !display.empty()) {
                    std::string before = display.substr(0, std::min(cursorPos, (int)display.size()));
                    auto beforeRun = fm.Shape(before, style);
                    cx += beforeRun.totalWidth;
                }
                r.DrawRect(cx, bounds.y + 4, 2, bounds.h - 8, cursorColor);
            }
        }
    }
}

bool TextInput::OnMouseDown(float x, float y) {
    bool wasFocused = focused;
    focused = PointInRect(x, y, bounds);
    if (focused && !wasFocused) {
        cursorPos = text.size();
        cursorBlink = 0;
    }
    return focused;
}

void TextInput::OnChar(uint32_t codepoint) {
    if (!focused) return;

    // Encode UTF-8
    char buf[5] = {};
    int len = 0;
    if (codepoint < 0x80) { buf[0] = codepoint; len = 1; }
    else if (codepoint < 0x800) {
        buf[0] = 0xC0 | (codepoint >> 6);
        buf[1] = 0x80 | (codepoint & 0x3F);
        len = 2;
    } else if (codepoint < 0x10000) {
        buf[0] = 0xE0 | (codepoint >> 12);
        buf[1] = 0x80 | ((codepoint >> 6) & 0x3F);
        buf[2] = 0x80 | (codepoint & 0x3F);
        len = 3;
    } else {
        buf[0] = 0xF0 | (codepoint >> 18);
        buf[1] = 0x80 | ((codepoint >> 12) & 0x3F);
        buf[2] = 0x80 | ((codepoint >> 6) & 0x3F);
        buf[3] = 0x80 | (codepoint & 0x3F);
        len = 4;
    }

    text.insert(cursorPos, buf, len);
    cursorPos += len;
    cursorBlink = 0;
}

void TextInput::OnKey(int key, int mods) {
    if (!focused) return;
    (void)mods;

    if (key == XKB_KEY_Return || key == XKB_KEY_KP_Enter) {
        if (onSubmit && !text.empty()) {
            onSubmit(text);
        }
        return;
    }

    if (key == XKB_KEY_BackSpace) {
        if (cursorPos > 0 && !text.empty()) {
            // Find previous UTF-8 char boundary
            int pos = cursorPos - 1;
            while (pos > 0 && (text[pos] & 0xC0) == 0x80) pos--;
            text.erase(pos, cursorPos - pos);
            cursorPos = pos;
            cursorBlink = 0;
        }
        return;
    }

    if (key == XKB_KEY_Delete) {
        if (cursorPos < (int)text.size()) {
            int end = cursorPos + 1;
            while (end < (int)text.size() && (text[end] & 0xC0) == 0x80) end++;
            text.erase(cursorPos, end - cursorPos);
            cursorBlink = 0;
        }
        return;
    }

    if (key == XKB_KEY_Left) {
        if (cursorPos > 0) {
            cursorPos--;
            while (cursorPos > 0 && (text[cursorPos] & 0xC0) == 0x80) cursorPos--;
            cursorBlink = 0;
        }
        return;
    }

    if (key == XKB_KEY_Right) {
        if (cursorPos < (int)text.size()) {
            cursorPos++;
            while (cursorPos < (int)text.size() && (text[cursorPos] & 0xC0) == 0x80) cursorPos++;
            cursorBlink = 0;
        }
        return;
    }

    if (key == XKB_KEY_Home) { cursorPos = 0; cursorBlink = 0; return; }
    if (key == XKB_KEY_End) { cursorPos = text.size(); cursorBlink = 0; return; }
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

void Dropdown::Paint(Renderer& r, FontManager& fm) const {
    if (!enabled) return;

    // Main button
    Color bg = open ? hoverColor : hovered ? hoverColor : bgColor;
    r.DrawRect(bounds.x, bounds.y, bounds.w, bounds.h, bg);

    // Selected text + dropdown arrow
    std::string display = SelectedLabel();
    if (display.empty()) display = "Select...";
    display += " \xe2\x96\xbe";  // ▾

    auto run = fm.Shape(display, style);
    float tx = bounds.x + 8;
    float ty = bounds.y + (bounds.h - fm.LineHeight(style)) / 2;
    r.DrawShapedRun(fm, run, tx, ty, fm.Ascent(style), textColor);

    // Popup
    if (open && !items.empty()) {
        WidgetRect pr = PopupRect();
        r.DrawRect(pr.x, pr.y, pr.w, pr.h, popupBg);

        float itemH = ItemHeight();
        for (int i = 0; i < (int)items.size(); i++) {
            float iy = pr.y + i * itemH;
            if (i == hoveredItem) {
                r.DrawRect(pr.x, iy, pr.w, itemH, popupHover);
            }
            if (i == selectedIndex) {
                // Subtle highlight for selected
                r.DrawRect(pr.x, iy, 3, itemH, {0.39f, 0.71f, 1.0f});
            }
            auto irun = fm.Shape(items[i].label, style);
            float ity = iy + (itemH - fm.LineHeight(style)) / 2;
            r.DrawShapedRun(fm, irun, pr.x + 10, ity, fm.Ascent(style), textColor);
        }
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

    if (PointInRect(x, y, bounds)) {
        open = true;
        return true;
    }
    return false;
}

bool Dropdown::OnMouseUp(float, float) {
    return false;
}

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
