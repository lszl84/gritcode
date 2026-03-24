#pragma once

#include <wx/wx.h>
#include <wx/control.h>
#include <wx/clipbrd.h>
#include "block_manager.h"
#include <vector>
#include <set>

class LoadingAnimator;

// A single wrapped line within a block, with cached layout data.
// caretX is computed lazily (only when needed for selection/hit-testing).
struct WrappedLine {
    wxString text;
    bool rightToLeft = false;
    int x;        // x position (accounts for margin, RTL = right-aligned)
    int y;        // y position relative to block top
    int width;    // measured text width
    int height;   // line height

    // Visual caret positions: computed lazily via EnsureCaretX().
    // caretX[i] is the visual x offset (relative to line.x) where a caret
    // at logical character position i would be drawn.
    // Size = text.length() + 1 (position 0 = before first char, N = after last).
    // For LTR: caretX[0]=0, caretX[N]=width (monotonically non-decreasing)
    // For RTL: caretX[0]=width, caretX[N]=0 (monotonically non-increasing)
    mutable std::vector<int> caretX;
    mutable bool caretXValid = false;
};

// A position in the text: which block, which line within block, which character
struct TextPosition {
    int block = -1;     // block index, -1 = invalid
    int line = 0;       // line index within block's wrapped lines
    int offset = 0;     // character offset within the line

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

// High-performance streaming text control with pixel-perfect text selection.
//
// Design: ALL measurement happens inside OnPaint using the wxPaintDC.
// Outside of paint, we only set dirty flags and call Refresh().
//
// Performance:
// - caretX arrays computed lazily (only for lines involved in selection/hit-test)
// - Prefix-sum blockTopCache for O(log N) visibility culling and hit-testing
// - No dc.Clear() double-paint: gaps filled explicitly
// - Fonts returned by const reference (avoid refcount bumps)
class StreamingTextCtrl : public wxControl {
public:
    StreamingTextCtrl(wxWindow* parent, wxWindowID id = wxID_ANY,
                      const wxPoint& pos = wxDefaultPosition,
                      const wxSize& size = wxDefaultSize,
                      long style = wxBORDER_THEME);
    ~StreamingTextCtrl();

    void AppendStream(BlockType type, const wxString& text, bool rtl = false);
    void ContinueStream(const wxString& text);
    void Clear();
    void ScrollToBottom();

    void SetAutoScroll(bool enable) { autoScroll = enable; }
    bool GetAutoScroll() const { return autoScroll; }

    void SetNormalFont(const wxFont& font);
    void SetUserPromptFont(const wxFont& font);
    void SetThinkingFont(const wxFont& font);
    void SetCodeFont(const wxFont& font);

    int GetTotalLines() const { return blockManager.GetTotalLines(); }
    size_t GetBlockCount() const { return blockManager.GetBlockCount(); }

    void StartThinking(size_t blockIndex);
    void StopThinking(size_t blockIndex);
    bool HasAnimatedBlocks() const { return !animatedBlocks.empty(); }

    void BeginBatch();
    void EndBatch();

    BlockManager& GetBlockManager() { return blockManager; }

    void UpdateThemeColors();
    bool IsDarkTheme() const;
    int GetLoadingFrame() const;

    // Selection
    bool HasSelection() const;
    wxString GetSelectedText() const;
    void SelectAll();
    void ClearSelection();
    void CopyToClipboard() const;

protected:
    void OnPaint(wxPaintEvent& event);
    void OnSize(wxSizeEvent& event);
    void OnScroll(wxScrollWinEvent& event);
    void OnMouseWheel(wxMouseEvent& event);
    void OnMouseLeftDown(wxMouseEvent& event);
    void OnMouseLeftUp(wxMouseEvent& event);
    void OnMouseMotion(wxMouseEvent& event);
    void OnMouseLeftDClick(wxMouseEvent& event);
    void OnMouseCaptureLost(wxMouseCaptureLostEvent& event);
    void OnKeyDown(wxKeyEvent& event);
    void OnEraseBackground(wxEraseEvent& event);
    void OnSystemColourChanged(wxSysColourChangedEvent& event);
    void OnAutoScrollTimer(wxTimerEvent& event);

    const wxFont& GetFontForType(BlockType type) const;
    const wxColour& GetColorForType(BlockType type) const;
    const wxColour& GetBackgroundColorForType(BlockType type) const;
    void DrawLoadingIndicator(wxDC& dc, int x, int y, int charHeight);

private:
    BlockManager blockManager;
    std::unique_ptr<LoadingAnimator> animator;

    int scrollPositionPx;
    int lineHeight;
    bool autoScroll;

    wxFont normalFont;
    wxFont userPromptFont;
    wxFont thinkingFont;
    wxFont codeFont;

    wxColour normalColor;
    wxColour userPromptColor;
    wxColour thinkingColor;
    wxColour codeColor;
    wxColour codeBackground;
    wxColour thinkingBackground;
    wxColour userPromptBackground;
    wxColour backgroundColor;

    bool isDarkTheme;
    std::set<size_t> animatedBlocks;

    int leftMargin;
    int topMargin;
    int blockSpacing;

    bool inBatch;

    // Per-block layout cache
    // wrappedLinesCache[blockIdx] = vector of WrappedLine for that block
    std::vector<std::vector<WrappedLine>> wrappedLinesCache;
    std::vector<int> blockHeightCache;   // Total pixel height per block
    std::vector<int> charHeightCache;    // Char height per block (for loading dots)
    std::vector<bool> blockDirty;        // True if block needs re-wrap (lazy on resize)

    // Prefix-sum of block Y positions for O(log N) visibility culling.
    // blockTopCache[i] = Y coordinate of top of block i (in virtual coords).
    // Size = blockCount + 1 (last entry = total content height).
    std::vector<int> blockTopCache;

    // Segment caching for O(1) re-layouts
    // Each segment represents a word or whitespace chunk with its pre-measured width.
    struct TextSegment {
        wxString text;
        int width;       // pixel width of this segment
        int height;      // pixel height (line height)
        bool isNewline;  // true = hard newline (text is empty)
        bool isSpace;    // true = space separator (text is " ")
        
        TextSegment() : width(0), height(0), isNewline(false), isSpace(false) {}
    };
    // segmentCache[blockIdx] = vector of segments for that block's text
    std::vector<std::vector<TextSegment>> segmentCache;
    // segmentCacheValid[blockIdx] = true if segments are measured and cached
    std::vector<bool> segmentCacheValid;

    int cachedTotalHeight;
    int cachedWidth;

    // Layout helpers
    void WrapBlock(wxDC& dc, const TextBlock* block, int textAreaWidth, int clientWidth,
                   std::vector<WrappedLine>& outLines, int& outHeight, int& outCharHeight, size_t blockIdx);
    static WrappedLine MakeLine(const wxString& text, bool rtl, int textWidth, int textHeight,
                                int margin, int clientWidth);

    // Segment caching for fast re-layouts
    void MeasureSegments(wxDC& dc, size_t blockIdx);
    void LayoutFromSegments(size_t blockIdx, int textAreaWidth, int clientWidth,
                            std::vector<WrappedLine>& outLines, int& outHeight);

    // Lazy caretX computation
    void EnsureCaretX(WrappedLine& wl, wxDC& dc) const;

    // Rebuild blockTopCache from blockHeightCache
    void RebuildBlockTopCache();

    // Find first visible block index using binary search on blockTopCache
    size_t FindFirstVisibleBlock(int scrollPos) const;

    // Selection state
    TextPosition m_selAnchor;
    TextPosition m_selCaret;
    bool m_selecting = false;

    // Double/triple-click
    int m_clickCount = 0;
    long m_lastClickTime = 0;
    TextPosition m_lastClickPos;
    TextPosition m_wordSelAnchorStart;
    TextPosition m_wordSelAnchorEnd;

    // Auto-scroll for drag selection
    wxTimer m_autoScrollTimer;
    int m_autoScrollDelta = 0;

    // Selection colours
    wxColour m_selBgColour{51, 153, 255};
    wxColour m_selTextColour{255, 255, 255};

    // Hit-testing
    TextPosition HitTest(int pixelX, int pixelY) const;
    int CaretXForOffset(const WrappedLine& wl, int charOffset) const;

    // Selection helpers
    void GetOrderedSelection(TextPosition& start, TextPosition& end) const;
    wxString GetTextBetween(const TextPosition& start, const TextPosition& end) const;
    void FindWordBoundary(const TextPosition& pos, TextPosition& wordStart, TextPosition& wordEnd) const;
    void DrawLineSelection(wxDC& dc, WrappedLine& wl, int blockIdx, int lineIdx,
                           int blockTopY, const TextPosition& selStart, const TextPosition& selEnd);
    void UpdateAutoScroll(const wxMouseEvent& evt);
    void StopAutoScroll();

    // Dirty flags
    bool needsFullRebuild;
    bool needsScrollUpdate;
    bool scrollToBottomPending;
    bool updatingScrollbar;
    int wheelRotationAccum = 0;

    wxDECLARE_EVENT_TABLE();
};

class LoadingAnimator {
public:
    LoadingAnimator(StreamingTextCtrl* ctrl);
    void Start();
    void Stop();
    bool IsRunning() const { return running; }
    int GetFrame() const { return frame; }
    void Notify();

private:
    StreamingTextCtrl* control;
    wxTimer timer;
    int frame;
    bool running;
};
