#pragma once
#include <wx/wx.h>
#include <wx/scrolwin.h>
#include "block.h"
#include <vector>

struct Palette {
    wxColour bg;
    wxColour text;
    wxColour codeBg;
    wxColour codeFg;
    wxColour userBubbleBg;
    wxColour selectionBg;
    wxColour thinkingDot;
    wxColour tableBorder;
    wxColour tableHeaderBg;
    wxColour toolHeaderBg;
    wxColour toolBodyBg;
    wxColour toolAccent;     // tool name + chevron color
    wxColour toolDim;        // hint text ("· N lines", separator)
};

// Custom-painted scrolling canvas that owns a vector<Block> and renders it
// markdown-style. Blocks are immutable once added; selection state is tracked
// as (blockIdx, charOffset-in-visibleText) anchor + caret pairs.
class ChatCanvas : public wxScrolledCanvas {
public:
    ChatCanvas(wxWindow* parent);

    // Append a finalized block. Triggers reflow + redraw.
    void AddBlock(Block b);

    // Toggle a ToolCall block's collapsed/expanded state. No-op for other types.
    void ToggleToolCall(int blockIdx);

    // Show/hide an animated "thinking dots" indicator below the last block.
    // Driven by web request streaming start/end. Mutates only the indicator —
    // never touches committed blocks.
    void SetThinking(bool on);

    // Clear all blocks (e.g. for a "New chat" button — not exposed in this
    // prototype but useful for future extensions).
    void Clear();

    // Returns concatenated visible text of the current selection, or empty
    // string if no selection. Used for clipboard copy.
    wxString GetSelectedText() const;

    // Select all blocks (Ctrl+A handler).
    void SelectAll();

    // Read-only access to the block list. Used by the MCP server to introspect
    // rendering state from outside. Returned reference is only valid on the
    // GUI thread; callers must marshal via CallAfter.
    const std::vector<Block>& Blocks() const { return blocks_; }

private:
    std::vector<Block> blocks_;
    bool thinking_ = false;

    // Selection state. Positions are content-relative so they survive the
    // streaming append of new blocks (existing block indices don't shift).
    BlockPos selAnchor_;
    BlockPos selCaret_;
    bool selecting_ = false;

    // Layout cache: total content height after most recent layout.
    int contentHeight_ = 0;
    int layoutWidth_ = -1;
    // Single dirty bit — fast path through Relayout when nothing changed.
    // Mutators flip it; Relayout clears it after a successful pass.
    bool layoutDirty_ = true;
    // Cumulative top Y of each block in canvas (unscrolled) coords.
    // Size = blocks_.size() + 1. blockTops_[i] = top of blocks_[i],
    // blockTops_[N] = bottom of last block (before thinking dots/margin).
    // Lets OnPaint binary-search the first visible block instead of walking.
    std::vector<int> blockTops_;

    // Animation tick for thinking dots.
    wxTimer animTimer_;
    double animPhase_ = 0;

    // Cached bounding rect of the thinking dots in canvas (unscrolled) coords,
    // updated each paint while thinking_ is on. Lets the anim tick invalidate
    // only the dots area instead of the whole canvas.
    wxRect dotsRect_;
    bool dotsRectValid_ = false;

    void OnPaint(wxPaintEvent& e);
    void OnSize(wxSizeEvent& e);
    void OnLeftDown(wxMouseEvent& e);
    void OnLeftUp(wxMouseEvent& e);
    void OnMotion(wxMouseEvent& e);
    void OnKeyDown(wxKeyEvent& e);
    void OnAnimTick(wxTimerEvent& e);
    void OnSysColourChanged(wxSysColourChangedEvent& e);
    void OnScrollWin(wxScrollWinEvent& e);
    void OnMouseWheel(wxMouseEvent& e);

    // Standard chat-app autoscroll: only pin to bottom on AddBlock when the
    // user is already near the bottom. If they scrolled up to read, leave the
    // viewport alone; re-engage when they scroll back near the bottom.
    bool stickToBottom_ = true;
    void UpdateStickToBottom();

    // Layout (or re-layout) all blocks to the given content width.
    void Relayout(int width);
    void LayoutBlock(wxDC& dc, Block& b, int contentWidth, int topSpacing) const;
    // Wrap a styled run sequence to maxW and produce visual lines. Used for
    // paragraphs, headings, user prompts, and individual table cells.
    void WrapRuns(wxDC& dc, const std::vector<InlineRun>& runs,
                  BlockType bt, int hLvl, int maxW,
                  std::vector<WrappedLine>& outLines) const;

    // Char-level wrap for monospace tool / code body text. Splits on existing
    // newlines, then within each line breaks at character boundaries when
    // content would otherwise overflow maxW. Prefers spaces for soft breaks
    // when one is available within the candidate range.
    void WrapMonospace(wxDC& dc, const wxString& text, int maxW,
                       std::vector<WrappedLine>& outLines,
                       int textOffBase = 0) const;

    // Hit-test: canvas-coords (after scrolling unscale) -> BlockPos.
    BlockPos HitTest(const wxPoint& canvasPt) const;

    // Paint helpers.
    void PaintBlock(wxDC& dc, const Block& b, int yTop, BlockPos selStart, BlockPos selEnd, int blockIdx) const;
    void PaintThinkingDots(wxDC& dc, int xLeft, int yTop) const;
    // Render the visible (post-Relayout) region into a memory DC. Used both
    // for the on-screen paint and for refreshing the cache bitmap.
    void RenderViewport(wxDC& dc, int viewY, int width, int height,
                        BlockPos selStart, BlockPos selEnd) const;

    // Cached palette. Rebuilt in the constructor and on wxEVT_SYS_COLOUR_CHANGED.
    Palette palette_;
    void RebuildPalette();

    // Font lookups — built once, cached.
    void EnsureFonts();
    wxFont fontBody_;
    wxFont fontBodyBold_;
    wxFont fontBodyItalic_;
    wxFont fontBodyBoldItalic_;
    wxFont fontCode_;
    wxFont fontH_[6];
    wxFont fontHB_[6];   // bold variant for inline bold inside heading
    bool fontsReady_ = false;

    // Returns the right wxFont for a run given its block context.
    const wxFont& FontFor(const InlineRun& r, BlockType bt, int hLvl) const;

    wxDECLARE_EVENT_TABLE();
};
