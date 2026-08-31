#pragma once
#include <wx/wx.h>
#include <wx/scrolwin.h>
#include "block.h"
#include <array>
#include <functional>
#include <unordered_map>
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
    wxColour thinkingBg;     // uniform background for thinking blocks
    wxColour thinkingText;   // body text color for thinking blocks (slightly dim)
    wxColour thinkingAccent; // chevron + "Thinking" label color
    wxColour linkColour;       // hyperlink text colour (blue, underlined)
};

// Custom-painted scrolling canvas that owns a vector<Block> and renders it
// markdown-style. Blocks are immutable once added; selection state is tracked
// as (blockIdx, charOffset-in-visibleText) anchor + caret pairs.
class ChatCanvas : public wxScrolledCanvas {
public:
    ChatCanvas(wxWindow* parent);

    // Override the background colour computed from system theme.
    // Pass wxNullColour to revert to the default palette bg.
    void SetBgColour(const wxColour& c) { bgOverride_ = c; Refresh(); }

    // Append a finalized block. Triggers reflow + redraw.
    void AddBlock(Block b);

    // Bulk-add mode for session restore: BeginBatch(), then any number of
    // AddBlock() calls accumulate with no reflow/redraw, then EndBatch()
    // reflows and redraws once. If the canvas isn't shown yet, the reflow
    // is deferred to the first paint anyway.
    void BeginBatch();
    void EndBatch();

    // Toggle a ToolCall block's collapsed/expanded state. No-op for other types.
    void ToggleToolCall(int blockIdx);

    // Show/hide an animated "thinking dots" indicator below the last block.
    // Driven by web request streaming start/end. Mutates only the indicator —
    // never touches committed blocks.
    void SetThinking(bool on);

    // Clear all blocks (e.g. for a "New chat" button — not exposed in this
    // prototype but useful for future extensions).
    void Clear();

    // Show a centered "Loading Session..." placeholder instead of blocks.
    // Used while a large session is being restored on the main thread.
    void SetLoading(bool on) { loading_ = on; loadingPaintFired_ = false; Refresh(); }

    // Called from the first paint of the loading placeholder (deferred via
    // CallAfter so the paint has been committed to the window first). The
    // frame uses it to run the heavy session restore after the placeholder
    // is actually on screen — a timer would race GTK's async first expose.
    void SetLoadingPaintCallback(std::function<void()> cb) { onLoadingPainted_ = std::move(cb); }

    // Returns concatenated visible text of the current selection, or empty
    // string if no selection. Used for clipboard copy.
    wxString GetSelectedText() const;

    // Select all blocks (Ctrl+A handler).
    void SelectAll();

    // Read-only access to the block list. Used by the MCP server to introspect
    // rendering state from outside. Returned reference is only valid on the
    // GUI thread; callers must marshal via CallAfter.
    const std::vector<Block>& Blocks() const { return blocks_; }

    // ---- Selection introspection / programmatic drive (used by MCP). All
    //      callers must already be on the GUI thread.
    BlockPos HitTestPublic(int canvasX, int canvasY) const {
        return HitTest({canvasX, canvasY});
    }
    // Returns the caret position a drag motion to (canvasX, canvasY) would
    // produce given the supplied anchor. This is the same resolution
    // OnMotion uses on real mouse events — including the through-drag snap
    // that treats collapsed tool blocks as atomic units when crossed from
    // outside. Lets MCP drag-simulation reproduce real interactive behavior.
    BlockPos ResolveDragCaret(int canvasX, int canvasY, BlockPos anchor) const {
        return ApplyDragSnap(HitTest({canvasX, canvasY}), {canvasX, canvasY}, anchor);
    }
    void GetSelection(BlockPos& anchor, BlockPos& caret) const {
        anchor = selAnchor_;
        caret = selCaret_;
    }
    void SetSelectionExplicit(BlockPos anchor, BlockPos caret) {
        selAnchor_ = anchor;
        selCaret_ = caret;
        selecting_ = false;
        Refresh();
    }
    // Per-block geometry in canvas (unscrolled) coords. Returns -1/-1 if idx
    // is out of range. Lets MCP-driven tests aim at known Y positions inside
    // a tool block without re-deriving layout from the outside.
    void GetBlockGeometry(int idx, int& yTop, int& height) const {
        if (idx < 0 || idx >= (int)blocks_.size()
            || idx >= (int)blockTops_.size()) {
            yTop = -1;
            height = -1;
            return;
        }
        yTop = blockTops_[idx];
        height = blocks_[idx].cachedHeight;
    }

private:
    std::vector<Block> blocks_;
    bool thinking_ = false;

    // Selection state. Positions are content-relative so they survive the
    // streaming append of new blocks (existing block indices don't shift).
    BlockPos selAnchor_;
    BlockPos selCaret_;
    bool selecting_ = false;

    // Click-vs-drag gesture for Image blocks.
    bool imageDragCandidate_ = false;
    wxPoint imageDragStart_;
    int imageDragBlock_ = -1;

    // Layout cache: total content height after most recent layout.
    int contentHeight_ = 0;
    int layoutWidth_ = -1;
    // Single dirty bit — fast path through Relayout when nothing changed.
    // Mutators flip it; Relayout clears it after a successful pass.
    bool layoutDirty_ = true;
    bool batchAdd_ = false;   // set between BeginBatch() and EndBatch()
    bool loading_ = false;    // draw loading placeholder instead of blocks
    bool loadingPaintFired_ = false;  // first paint of the placeholder has happened
    std::function<void()> onLoadingPainted_;  // invokes the deferred restore
    // Cumulative top Y of each block in canvas (unscrolled) coords.
    // Size = blocks_.size() + 1. blockTops_[i] = top of blocks_[i],
    // blockTops_[N] = bottom of last block (before thinking dots/margin).
    // Lets OnPaint binary-search the first visible block instead of walking.
    std::vector<int> blockTops_;

    // Animation tick for thinking dots.
    wxTimer animTimer_;
    double animPhase_ = 0;

    // Debounce timer for resize. EVT_SIZE only restarts this timer; the actual
    // (potentially expensive) Relayout runs once it fires, after the user
    // stops dragging. In the meantime OnPaint reuses the cached layout — only
    // re-centering it at the new client width — so window edges track the
    // cursor without re-wrapping every block per frame.
    wxTimer resizeTimer_;

    // Cached bounding rect of the thinking dots in canvas (unscrolled) coords,
    // updated each paint while thinking_ is on. Lets the anim tick invalidate
    // only the dots area instead of the whole canvas.
    wxRect dotsRect_;
    bool dotsRectValid_ = false;

    void OnPaint(wxPaintEvent& e);
    void OnSize(wxSizeEvent& e);
    void OnLeftDown(wxMouseEvent& e);
    void OnLeftUp(wxMouseEvent& e);
    void OnLeftDClick(wxMouseEvent& e);
    void OnMotion(wxMouseEvent& e);
    void OnKeyDown(wxKeyEvent& e);
    void OnAnimTick(wxTimerEvent& e);
    void OnResizeSettle(wxTimerEvent& e);
    void OnSysColourChanged(wxSysColourChangedEvent& e);
    void OnScrollWin(wxScrollWinEvent& e);
    void OnMouseWheel(wxMouseEvent& e);

    // Standard chat-app autoscroll: only pin to bottom on AddBlock when the
    // user is already near the bottom. If they scrolled up to read, leave the
    // viewport alone; re-engage when they scroll back near the bottom.
    bool stickToBottom_ = true;
    void UpdateStickToBottom();
    // If pinned to the bottom, scroll to the current contentHeight_ bottom.
    void ScrollToBottomIfPinned();

    // Measurement accounting for profiling (WX_GRITCODE_PROF=1). Filled by
    // MeasExtent/MeasPartial/MeasSetFont wrappers and dumped at the end of
    // Relayout so we can see where layout time actually goes.
    mutable long long extentUs_ = 0, partialUs_ = 0, setfontUs_ = 0;
    mutable long long extentCalls_ = 0, partialCalls_ = 0, setfontCalls_ = 0;

    void MeasExtent(wxDC& dc, const wxString& s, wxCoord* w, wxCoord* h) const;
    void MeasPartial(wxDC& dc, const wxString& s, wxArrayInt& parts) const;
    void MeasSetFont(wxDC& dc, const wxFont& f) const;

    // Memoized text-measurement state. Token widths repeat enormously in chat
    // text (spaces, punctuation, common words), so cache by (font, text).
    // Heights are constant per font, cached separately in fontHeightCache_.
    mutable std::unordered_map<std::wstring, int> widthCache_;
    mutable std::array<int, 12> fontHeightCache_{};  // 0 = not cached

    int FontIndex(const InlineRun& r, BlockType bt, int hLvl) const;
    int FontHeight(wxDC& dc, int fi, const wxFont& f) const;

    // Lazily compute and return the per-char x positions for a wrapped line.
    // Only used for partial-line selection and hit-testing; full-line work
    // uses WrappedLine::lineWidth instead.
    const std::vector<int>& LineGlyphs(wxDC& dc, const Block& b,
                                       const WrappedLine& wl) const;

    // Layout (or re-layout) all blocks to the given content width.
    void Relayout(int width);
    // Client width -> content column width (clamped to [100, kMaxContentW]).
    int ContentWidthFor(int clientW) const;
    // Cheap O(text) height estimate for an unmeasured block — no DC calls, so
    // restoring a huge session doesn't measure every block up front.
    int EstimateBlockHeight(const Block& b, int contentW) const;
    // Recompute blockTops_ + contentHeight_ from current cachedHeight values
    // (measured or estimated) and update the virtual size.
    void RecomputeTops();
    // Measure (LayoutBlock) any unmeasured blocks near the viewport so paint
    // has real line geometry; off-screen blocks stay estimated until scrolled
    // into view.
    void EnsureVisibleLaidOut(int viewY, int viewH, int contentW);
    void EnsureBlockLaidOut(size_t idx, int contentW);
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

    // Through-drag snap: when the cursor enters a *collapsed* tool block
    // during a drag whose anchor is in a different block, snap the caret
    // offset to 0 (upper half of the block) or end-of-visibleText (lower
    // half). See OnMotion comment for rationale.
    BlockPos ApplyDragSnap(BlockPos hp, const wxPoint& canvasPt,
                           BlockPos anchor) const;

    // Map a table block's visibleText offset to a (row, col, cellCharOffset).
    // Returns true if the offset falls within a cell (not on a delimiter).
    bool TableOffsetToCell(const Block& b, int offset,
                           int& row, int& col, int& cellOff) const;
    // Inverse: (row, col, cellCharOffset) -> visibleText offset.
    int TableCellToOffset(const Block& b, int row, int col, int cellOff) const;

    // Find word boundaries around a given BlockPos for double-click selection.
    void FindWordBounds(const BlockPos& pos, BlockPos& wordStart, BlockPos& wordEnd) const;

    // Return the link URL (if any) at a given BlockPos. Empty string if none.
    wxString LinkUrlAt(const BlockPos& pos) const;

    // Paint helpers.
    void PaintBlock(wxDC& dc, const Block& b, int yTop, BlockPos selStart, BlockPos selEnd, int blockIdx) const;
    void PaintThinkingDots(wxDC& dc, int xLeft, int yTop) const;
    // Render the visible (post-Relayout) region into a memory DC. Used both
    // for the on-screen paint and for refreshing the cache bitmap.
    void RenderViewport(wxDC& dc, int viewY, int width, int height,
                        BlockPos selStart, BlockPos selEnd) const;

    // Cached palette. Rebuilt in the constructor and on wxEVT_SYS_COLOUR_CHANGED.
    Palette palette_;
    wxColour bgOverride_;  // if set, overrides palette_.bg
    void RebuildPalette();

    // Font lookups — built once, cached.
    void EnsureFonts();
    wxFont fontBody_;
    wxFont fontBodyBold_;
    wxFont fontBodyItalic_;
    wxFont fontBodyBoldItalic_;
    wxFont fontCode_;
    wxFont fontThinking_;       // smaller italic font for thinking-block body
    wxFont fontH_[6];
    wxFont fontHB_[6];   // bold variant for inline bold inside heading
    bool fontsReady_ = false;

    // Returns the right wxFont for a run given its block context.
    const wxFont& FontFor(const InlineRun& r, BlockType bt, int hLvl) const;

    wxDECLARE_EVENT_TABLE();
};
