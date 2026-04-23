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
#include "gl_renderer.h"
#include <string>
#include <vector>
#include <functional>

// Simple flat widget system for the bottom control bar.
// All widgets are positioned absolutely (no layout engine).

struct WidgetRect { float x, y, w, h; };

inline bool PointInRect(float px, float py, const WidgetRect& r) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

// ============================================================================
// Label
// ============================================================================
class Label {
public:
    WidgetRect bounds{};
    std::string text;
    Color color{0.6f, 0.6f, 0.6f};
    FontStyle style = FontStyle::Regular;
    bool rightAlign = false;

    void Paint(GLRenderer& r, FontManager& fm) const;
};

// ============================================================================
// Button
// ============================================================================
class Button {
public:
    WidgetRect bounds{};
    std::string text;
    Color textColor{0.85f, 0.85f, 0.85f};
    Color bgColor{0.22f, 0.22f, 0.24f};
    Color hoverColor{0.28f, 0.28f, 0.30f};
    Color pressColor{0.18f, 0.18f, 0.20f};
    Color disabledText{0.4f, 0.4f, 0.4f};
    FontStyle style = FontStyle::Regular;
    bool hovered = false;
    bool pressed = false;
    bool enabled = true;
    bool visible = true;
    std::function<void()> onClick;

    void Paint(GLRenderer& r, FontManager& fm) const;
    bool OnMouseDown(float x, float y);
    bool OnMouseUp(float x, float y);
    void OnMouseMove(float x, float y);
};

// ============================================================================
// TextInput (single-line)
// ============================================================================
class TextInput {
public:
    WidgetRect bounds{};
    std::string text;
    std::string placeholder = "Type here...";
    Color textColor{0.85f, 0.85f, 0.85f};
    Color bgColor{0.15f, 0.15f, 0.16f};
    Color borderColor{0.3f, 0.3f, 0.32f};
    Color focusBorder{0.39f, 0.71f, 1.0f};
    Color placeholderColor{0.4f, 0.4f, 0.42f};
    Color cursorColor{0.85f, 0.85f, 0.85f};
    FontStyle style = FontStyle::Regular;
    bool focused = false;
    bool password = false;  // mask with dots
    int cursorPos = 0;      // byte position
    int selStart = 0;       // selection in codepoint offsets
    int selEnd = 0;
    float cursorBlink = 0;

    std::function<void(const std::string&)> onSubmit;
    std::function<std::string()> onPaste;

    void Paint(GLRenderer& r, FontManager& fm) const;
    bool OnMouseDown(float x, float y, FontManager& fm);
    void OnMouseDrag(float x, float y, FontManager& fm);
    std::string GetSelectedText() const;
    void OnChar(uint32_t codepoint, FontManager& fm);
    void OnKey(int key, int mods, FontManager& fm);
    void Update(float dt, FontManager& fm);
    void SetText(const std::string& t) { text = t; cursorPos = t.size(); selStart = selEnd = 0; scrollX = 0; }
    void Clear() { text.clear(); cursorPos = 0; selStart = selEnd = 0; scrollX = 0; }

private:
    std::string DisplayText() const;
    float scrollX = 0;
    int clickCount_ = 0;
    double lastClickTime_ = 0;
    void EnsureCursorVisible(FontManager& fm);
};

// ============================================================================
// Dropdown
// ============================================================================
struct DropdownItem {
    std::string id;
    std::string label;
};

class Dropdown {
public:
    WidgetRect bounds{};
    std::vector<DropdownItem> items;
    int selectedIndex = -1;
    Color textColor{0.85f, 0.85f, 0.85f};
    Color bgColor{0.22f, 0.22f, 0.24f};
    Color hoverColor{0.28f, 0.28f, 0.30f};
    Color popupBg{0.18f, 0.18f, 0.20f};
    Color popupHover{0.28f, 0.35f, 0.50f};
    FontStyle style = FontStyle::Regular;
    bool hovered = false;
    bool open = false;
    int hoveredItem = -1;
    bool enabled = true;
    float scrollOffset = 0;       // vertical scroll in pixels
    float autoScrollSpeed = 0;    // pixels/sec when hovering near edge
    float autoScrollAccum = 0;    // accumulated sub-pixel scroll

    static constexpr float kEdgeZone = 24.0f;  // px from popup edge to trigger auto-scroll

    std::function<void(int index, const std::string& id)> onSelect;

    void Paint(GLRenderer& r, FontManager& fm) const;
    void PaintPopup(GLRenderer& r, FontManager& fm) const;  // Draw on top of everything
    bool OnMouseDown(float x, float y);
    bool OnMouseUp(float x, float y);
    void OnMouseMove(float x, float y);
    void OnScroll(float dy);  // mouse wheel inside popup
    void Update(float dt);  // tick auto-scroll
    void Close() { open = false; hoveredItem = -1; scrollOffset = 0; autoScrollSpeed = 0; }

    std::string SelectedId() const {
        return (selectedIndex >= 0 && selectedIndex < (int)items.size())
            ? items[selectedIndex].id : "";
    }
    std::string SelectedLabel() const {
        return (selectedIndex >= 0 && selectedIndex < (int)items.size())
            ? items[selectedIndex].label : "";
    }

    float ItemHeight() const { return 32; }
    float MaxVisibleItems() const { return 12; }
    float VisiblePopupHeight() const;
    float TotalPopupHeight() const { return items.size() * ItemHeight(); }
    float MaxScroll() const;
    WidgetRect PopupRect() const;
};
