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
#include <vector>
#include <memory>
#include <set>
#include <functional>

class ScrollView {
public:
    ScrollView();

    bool Init(int windowW, int windowH, float scale = 1.0f);

    void AppendStream(BlockType type, const std::string& text, bool rtl = false);
    void ContinueStream(const std::string& text);
    void AddBlocks(std::vector<std::unique_ptr<TextBlock>> blocks);
    void RemoveBlocksFrom(size_t fromIndex);
    void Clear();

    void SetAutoScroll(bool e) { autoScroll_ = e; }
    void ScrollToBottom();
    void ScrollBy(float delta);

    bool HasSelection() const;
    std::string GetSelectedText() const;
    void SelectAll();
    void ClearSelection();

    void OnMouseDown(float x, float y, bool shift);
    void OnMouseUp(float x, float y);
    void OnMouseMove(float x, float y, bool leftDown);
    void OnScroll(float yOffset);
    void OnKey(int key, int mods);
    void OnResize(int w, int h);
    float ContentBottom() const {
        // Last block's actual bottom edge (no margin padding)
        if (blocks_.empty() || blockTopCache_.empty()) return topMargin_;
        size_t last = blocks_.size() - 1;
        return blockTopCache_[last] + blockHeightCache_[last] - scrollPos_;
    }
    float ColumnLeft() const { return leftMargin_; }
    float ColumnWidth() const { return contentW_; }

    void Paint(GLRenderer& renderer);
    bool NeedsRedraw() const { return needsRedraw_; }
    void MarkDirty() { needsRedraw_ = true; }
    void ClearDirty() { needsRedraw_ = false; }

    void StartThinking(size_t blockIdx);
    void StopThinking(size_t blockIdx);
    void StopAllAnimations();
    bool HasActiveThinking() const { return !animatedBlocks_.empty(); }
    void ToggleCollapse(size_t blockIdx);
    void Update(float dt);

    size_t BlockCount() const { return blocks_.size(); }
    std::vector<std::unique_ptr<TextBlock>>& Blocks() { return blocks_; }
    void RequestRebuild() { needsFullRebuild_ = true; }
    FontManager& Fonts() { return fonts_; }
    Color BgColor() const { return bgColor_; }

    void BeginBatch();
    void EndBatch();

    using ClipboardFunc = std::function<void(const std::string&)>;
    void SetClipboardFunc(ClipboardFunc fn) { clipboardFn_ = fn; }

private:
    FontManager fonts_;
    ClipboardFunc clipboardFn_;
    std::vector<std::unique_ptr<TextBlock>> blocks_;

    int windowW_ = 900, windowH_ = 700;
    float scrollPos_ = 0;
    bool autoScroll_ = true;
    bool inBatch_ = false;

    float leftMargin_ = 10;
    float topMargin_ = 0;
    float blockSpacing_ = 0;

    // Centered chat column. leftMargin_ is recomputed from these so it always
    // points at the column's left edge. When the window is wider than
    // maxContentW_ + 2*sideMargin_, the column hugs the center with extra
    // whitespace on either side; below that the column shrinks down to the
    // sideMargin floor.
    float sideMargin_ = 0;
    float maxContentW_ = 0;
    float contentW_ = 0;

    // Per-block-type chrome paddings/radii (scaled at Init time).
    float userBubblePad_ = 0;
    float userBubbleRadius_ = 0;
    float codeHPad_ = 0;
    float codeVPad_ = 0;
    float codeRadius_ = 0;
    float toolPadX_ = 0;
    float toolPadY_ = 0;
    float toolGap_ = 0;
    float toolRadius_ = 0;
    float tableHPad_ = 0;
    float tableVPad_ = 0;
    float tableBorderW_ = 0;
    // Extra leading added per visual line for regular paragraph text — wx's
    // GetTextExtent reports a slightly taller line box than FreeType's
    // metrics.height/64, so we add ~1 scaled px to match the wx-branch feel.
    float paraLineGap_ = 0;

    Color bgColor_, normalColor_, userPromptColor_, thinkingColor_, codeColor_;
    Color codeBg_, thinkingBg_, userPromptBg_;
    Color toolHeaderBg_, toolBodyBg_, toolAccent_, toolDim_;
    Color tableHeaderBg_, tableBorderColor_;
    Color selBgColor_, selTextColor_;

    mutable std::vector<std::vector<std::vector<ShapedRun>>> shapedCache_;
    std::vector<std::vector<WrappedLine>> wrappedCache_;
    std::vector<float> blockHeightCache_;
    std::vector<float> charHeightCache_;
    std::vector<float> blockTopCache_;

    // Per-block table chrome info — populated only for BlockType::TABLE blocks
    // so Paint can draw the background, header rule, and column dividers
    // without re-deriving column geometry from scratch.
    struct TableLayoutInfo {
        std::vector<float> colX;   // left edge of each column, relative to block's text area
        std::vector<float> colW;   // per-column content widths
        std::vector<float> rowBottomY;  // bottom y of each row (relative to block top)
        float hPad = 0;            // horizontal padding inside each cell
        float vPad = 0;            // vertical padding inside each cell
        float headerBottomY = 0;   // y of the rule under the header row, relative to block top
        float totalW = 0;          // full table width (from colX[0] to right edge of last col)
    };
    std::vector<TableLayoutInfo> tableLayoutCache_;
    float cachedTotalH_ = 0;
    int cachedW_ = -1;
    bool needsFullRebuild_ = true;
    bool streamDirty_ = false;
    bool needsRedraw_ = true;

    struct TextSegment {
        std::string text;
        float width = 0, height = 0;
        bool isNewline = false, isSpace = false;
        FontStyle style = FontStyle::Regular;
        Color color;
        bool hasStyle = false;
    };
    std::vector<std::vector<TextSegment>> segCache_;
    std::vector<bool> segValid_;
    std::vector<size_t> segTextLen_;

    TextPosition selAnchor_, selCaret_;
    bool selecting_ = false;
    int clickCount_ = 0;
    double lastClickTime_ = 0;
    TextPosition lastClickPos_;
    TextPosition wordAnchorStart_, wordAnchorEnd_;
    float autoScrollDelta_ = 0;

    std::set<size_t> animatedBlocks_;
    float animTime_ = 0;
    int loadingFrame_ = 0;

    void MeasureSegments(size_t idx);
    void MeasureStyledSegments(size_t idx);
    void LayoutFromSegments(size_t idx, float textAreaW, float clientW,
                            std::vector<WrappedLine>& out, float& outH);
    void LayoutTable(size_t idx, float textAreaW, float clientW,
                     std::vector<WrappedLine>& out, float& outH);
    WrappedLine MakeLine(const std::string& text, bool rtl, float textW,
                         float textH, float margin, float clientW, int indent = 0);

    void RebuildSingleBlock(size_t idx, float textAreaW, float clientW);
    void RebuildBlockTopCache();
    void UpdateBlockTopCacheTail(size_t from);
    size_t FindFirstVisible(float scrollPos) const;

    void EnsureShapedCache(size_t bi, size_t li) const;

    TextPosition HitTest(float px, float py) const;
    void EnsureCaretX(WrappedLine& wl, FontStyle style, bool rtl) const;
    float CaretXForOffset(const WrappedLine& wl, int off) const;
    void GetOrderedSel(TextPosition& s, TextPosition& e) const;
    std::string TextBetween(const TextPosition& s, const TextPosition& e) const;
    void FindWordBoundary(const TextPosition& pos, TextPosition& ws, TextPosition& we) const;

    FontStyle StyleForType(BlockType t) const;
    Color ColorForType(BlockType t) const;
    Color BgForType(BlockType t) const;
    void InitColors();

    // Recomputes leftMargin_/contentW_ from windowW_, sideMargin_, maxContentW_.
    // Call after windowW_ changes.
    void RecomputeColumn();

    // Per-block paint offsets. xOff/yOff shift drawn text relative to wl.x and
    // wl.y (bubbles inset, code padded, tool body sits below the header).
    // bubbleX/bubbleW describe the right-aligned bubble for USER_PROMPT, the
    // rounded box for CODE/TOOL_CALL.
    struct BlockChrome {
        float xOff = 0;
        float yOff = 0;
        float bubbleX = 0;
        float bubbleW = 0;
        float headerH = 0;  // TOOL_CALL only: header strip height
        float bodyH = 0;    // TOOL_CALL only: body box height (0 when collapsed)
    };
    BlockChrome ChromeFor(size_t i) const;

    // Build wrapped body lines for an expanded TOOL_CALL block. Mutates the
    // block's wrappedCache slot to hold one WrappedLine per visual body line.
    void LayoutToolCallBody(size_t idx, float bodyTextW);
};
