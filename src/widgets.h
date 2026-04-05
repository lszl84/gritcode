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

    void Paint(GLRenderer& r, FontManager& fm, float time) const;
    bool OnMouseDown(float x, float y);
    void OnMouseDrag(float x, float y, FontManager& fm);
    void OnChar(uint32_t codepoint);
    void OnKey(int key, int mods);
    void Update(float dt);
    void SetText(const std::string& t) { text = t; cursorPos = t.size(); selStart = selEnd = 0; }
    void Clear() { text.clear(); cursorPos = 0; selStart = selEnd = 0; }

private:
    std::string DisplayText() const;
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

    std::function<void(int index, const std::string& id)> onSelect;

    void Paint(GLRenderer& r, FontManager& fm) const;
    void PaintPopup(GLRenderer& r, FontManager& fm) const;  // Draw on top of everything
    bool OnMouseDown(float x, float y);
    bool OnMouseUp(float x, float y);
    void OnMouseMove(float x, float y);
    void Close() { open = false; hoveredItem = -1; }

    std::string SelectedId() const {
        return (selectedIndex >= 0 && selectedIndex < (int)items.size())
            ? items[selectedIndex].id : "";
    }
    std::string SelectedLabel() const {
        return (selectedIndex >= 0 && selectedIndex < (int)items.size())
            ? items[selectedIndex].label : "";
    }

    float PopupHeight() const { return items.size() * ItemHeight(); }
    float ItemHeight() const { return 28; }

private:
    WidgetRect PopupRect() const;
};
