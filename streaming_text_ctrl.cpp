#include "streaming_text_ctrl.h"
#include "markdown_renderer.h"
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/settings.h>
#include <wx/dataobj.h>
#include <algorithm>
#include <cstdio>
#include <cmath>

// Logging macro - writes to stderr so we can observe behavior
#define LOG(fmt, ...) fprintf(stderr, "[STC] " fmt "\n", ##__VA_ARGS__)

static const int ANIMATION_INTERVAL = 150;
static const int LOADING_DOT_COUNT = 3;
static constexpr int AUTO_SCROLL_TIMER_ID = 3001;
static constexpr int AUTO_SCROLL_INTERVAL_MS = 30;
static constexpr int MULTI_CLICK_THRESHOLD_MS = 400;

// Detect RTL from the first strong directional character in the text.
// This mirrors browser "dir=auto" behavior.
static bool DetectRTL(const wxString& text) {
    for (size_t i = 0; i < text.length(); ++i) {
        wxUniChar ch = text[i];
        unsigned int cp = ch.GetValue();

        // RTL scripts
        if ((cp >= 0x0590 && cp <= 0x05FF) ||   // Hebrew
            (cp >= 0x0600 && cp <= 0x06FF) ||   // Arabic
            (cp >= 0x0700 && cp <= 0x074F) ||   // Syriac
            (cp >= 0x0750 && cp <= 0x077F) ||   // Arabic Supplement
            (cp >= 0x0780 && cp <= 0x07BF) ||   // Thaana
            (cp >= 0x07C0 && cp <= 0x07FF) ||   // NKo
            (cp >= 0x0800 && cp <= 0x083F) ||   // Samaritan
            (cp >= 0x0840 && cp <= 0x085F) ||   // Mandaic
            (cp >= 0x08A0 && cp <= 0x08FF) ||   // Arabic Extended-A
            (cp >= 0xFB50 && cp <= 0xFDFF) ||   // Arabic Presentation Forms-A
            (cp >= 0xFE70 && cp <= 0xFEFF) ||   // Arabic Presentation Forms-B
            (cp >= 0x10800 && cp <= 0x1083F) || // Cypriot Syllabary
            (cp >= 0x1E800 && cp <= 0x1E8DF))   // Mende Kikakui
            return true;

        // LTR scripts (Latin, Greek, Cyrillic, CJK, etc.)
        if ((cp >= 0x0041 && cp <= 0x005A) ||   // A-Z
            (cp >= 0x0061 && cp <= 0x007A) ||   // a-z
            (cp >= 0x00C0 && cp <= 0x024F) ||   // Latin Extended
            (cp >= 0x0370 && cp <= 0x03FF) ||   // Greek
            (cp >= 0x0400 && cp <= 0x04FF) ||   // Cyrillic
            (cp >= 0x0900 && cp <= 0x097F) ||   // Devanagari
            (cp >= 0x3000 && cp <= 0x9FFF) ||   // CJK
            (cp >= 0xAC00 && cp <= 0xD7AF))     // Hangul
            return false;

        // Skip neutral characters (spaces, digits, punctuation) and keep scanning
    }
    return false;  // Default to LTR
}

wxBEGIN_EVENT_TABLE(StreamingTextCtrl, wxControl)
    EVT_PAINT(StreamingTextCtrl::OnPaint)
    EVT_SIZE(StreamingTextCtrl::OnSize)
    EVT_SCROLLWIN(StreamingTextCtrl::OnScroll)
    EVT_MOUSEWHEEL(StreamingTextCtrl::OnMouseWheel)
    EVT_LEFT_DOWN(StreamingTextCtrl::OnMouseLeftDown)
    EVT_LEFT_UP(StreamingTextCtrl::OnMouseLeftUp)
    EVT_MOTION(StreamingTextCtrl::OnMouseMotion)
    EVT_LEFT_DCLICK(StreamingTextCtrl::OnMouseLeftDClick)
    EVT_MOUSE_CAPTURE_LOST(StreamingTextCtrl::OnMouseCaptureLost)
    EVT_KEY_DOWN(StreamingTextCtrl::OnKeyDown)
    EVT_ERASE_BACKGROUND(StreamingTextCtrl::OnEraseBackground)
    EVT_SYS_COLOUR_CHANGED(StreamingTextCtrl::OnSystemColourChanged)
    EVT_TIMER(AUTO_SCROLL_TIMER_ID, StreamingTextCtrl::OnAutoScrollTimer)
wxEND_EVENT_TABLE()

// ============================================================================
// LoadingAnimator
// ============================================================================

LoadingAnimator::LoadingAnimator(StreamingTextCtrl* ctrl)
    : control(ctrl), frame(0), running(false) {
    timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { this->Notify(); });
}

void LoadingAnimator::Start() {
    if (!running) {
        running = true;
        timer.Start(ANIMATION_INTERVAL);
    }
}

void LoadingAnimator::Stop() {
    if (running) {
        running = false;
        timer.Stop();
    }
}

void LoadingAnimator::Notify() {
    frame = (frame + 1) % LOADING_DOT_COUNT;
    if (control && control->HasAnimatedBlocks()) {
        control->Refresh();
    } else {
        Stop();
    }
}

// ============================================================================
// StreamingTextCtrl construction
// ============================================================================

StreamingTextCtrl::StreamingTextCtrl(wxWindow* parent, wxWindowID id,
                                     const wxPoint& pos, const wxSize& size,
                                     long style)
    : wxControl(parent, id, pos, size, style | wxVSCROLL | wxWANTS_CHARS)
    , scrollPositionPx(0)
    , lineHeight(20)
    , autoScroll(true)
    , isDarkTheme(false)
    , leftMargin(10)
    , topMargin(0)
    , blockSpacing(0)
    , inBatch(false)
    , cachedWidth(-1)
    , cachedTotalHeight(0)
    , needsFullRebuild(true)
    , needsScrollUpdate(true)
    , scrollToBottomPending(false)
    , updatingScrollbar(false)
    , m_autoScrollTimer(this, AUTO_SCROLL_TIMER_ID) {

    wxFont defaultGuiFont = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    int defaultSize = defaultGuiFont.GetPointSize();

    normalFont = defaultGuiFont;
    userPromptFont = defaultGuiFont;
    thinkingFont = wxFont(defaultSize - 2, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC, wxFONTWEIGHT_LIGHT);
    codeFont = wxFont(defaultSize, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);

    UpdateThemeColors();
    animator = std::make_unique<LoadingAnimator>(this);

    wxClientDC dc(this);
    dc.SetFont(normalFont);
    lineHeight = dc.GetCharHeight();
    
    // Cache space width (measured once, reused everywhere)
    wxCoord sw, sh;
    dc.GetTextExtent(wxS(" "), &sw, &sh);
    cachedSpaceWidth = sw;
    cachedSpaceHeight = sh;

    topMargin = lineHeight;
    blockSpacing = lineHeight / 2;
    cachedTotalHeight = topMargin * 2;

    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_IBEAM));
}

StreamingTextCtrl::~StreamingTextCtrl() = default;

// ============================================================================
// Layout: two-phase wrapping with cached word measurements
//
// Phase 1 (MeasureSegments): Uses DC to measure word widths. Expensive.
//         Only called when block text changes. Results cached.
// Phase 2 (LayoutFromSegments): Pure arithmetic line-breaking using cached
//         widths. No DC needed. Called on every width change, but is fast.
// ============================================================================

WrappedLine StreamingTextCtrl::MakeLine(const wxString& text, bool rtl,
                                         int textWidth, int textHeight,
                                         int margin, int clientWidth) {
    WrappedLine wl;
    wl.text = text;
    wl.rightToLeft = rtl;
    wl.y = 0;
    wl.width = textWidth;
    wl.height = textHeight;
    wl.caretXValid = false;

    if (rtl && textWidth > 0) {
        int xPos = clientWidth - margin - textWidth;
        wl.x = (xPos < margin) ? margin : xPos;
    } else {
        wl.x = margin;
    }

    return wl;
}

void StreamingTextCtrl::EnsureCaretX(WrappedLine& wl, wxDC& dc) const {
    if (wl.caretXValid) return;

    int len = (int)wl.text.length();
    wl.caretX.resize(len + 1);

    if (len == 0) {
        wl.caretX[0] = 0;
        wl.caretXValid = true;
        return;
    }

    if (!wl.styledRuns.empty()) {
        // Styled line: measure each run with its own font
        wl.caretX[0] = 0;
        int charIdx = 0;
        int xAccum = 0;
        
        for (size_t ri = 0; ri < wl.styledRuns.size(); ri++) {
            const auto& run = wl.styledRuns[ri];
            dc.SetFont(run.font);
            
            wxArrayInt offsets;
            dc.GetPartialTextExtents(run.text, offsets);
            
            for (int i = 0; i < (int)run.text.length(); i++) {
                charIdx++;
                if (charIdx <= len) {
                    wl.caretX[charIdx] = xAccum + offsets[i];
                }
            }
            
            if (!offsets.IsEmpty()) {
                xAccum += offsets.Last();
            }
        }
        
        // Fill any remaining positions (shouldn't happen, but safety)
        for (int i = charIdx + 1; i <= len; i++) {
            wl.caretX[i] = xAccum;
        }
    } else {
        // Plain line: single font measurement
        wxArrayInt offsets;
        dc.GetPartialTextExtents(wl.text, offsets);

        if (wl.rightToLeft) {
            wl.caretX[0] = wl.width;
            for (int i = 1; i <= len; ++i)
                wl.caretX[i] = wl.width - offsets[i - 1];
        } else {
            wl.caretX[0] = 0;
            for (int i = 1; i <= len; ++i)
                wl.caretX[i] = offsets[i - 1];
        }
    }

    wl.caretXValid = true;
}

// Phase 1: Measure all text segments in a block. Requires DC.
void StreamingTextCtrl::MeasureSegments(wxDC& dc, size_t blockIdx) {
    const TextBlock* block = blockManager.GetBlock(blockIdx);
    auto& segs = segmentCache[blockIdx];
    segs.clear();

    if (!block || block->text.IsEmpty()) {
        TextSegment seg;
        seg.text = wxEmptyString;
        seg.width = 0;
        seg.height = dc.GetCharHeight();
        seg.isNewline = true;
        seg.isSpace = false;
        segs.push_back(std::move(seg));
        segmentCacheValid[blockIdx] = true;
        segmentCachedTextLen[blockIdx] = 0;
        return;
    }

    const wxString& fullText = block->text;
    size_t pos = 0;

    while (pos <= fullText.length()) {
        size_t nlPos = fullText.find('\n', pos);
        if (nlPos == wxString::npos) nlPos = fullText.length();

        if (nlPos == pos) {
            TextSegment seg;
            seg.text = wxEmptyString;
            seg.width = 0;
            seg.height = dc.GetCharHeight();
            seg.isNewline = true;
            seg.isSpace = false;
            segs.push_back(std::move(seg));
        } else {
            wxString hardLine = fullText.Mid(pos, nlPos - pos);
            size_t wpos = 0;
            bool firstInLine = true;

            while (wpos < hardLine.length()) {
                size_t spPos = hardLine.find(' ', wpos);

                wxString word;
                if (spPos == wxString::npos) {
                    word = hardLine.Mid(wpos);
                    wpos = hardLine.length();
                } else {
                    word = hardLine.Mid(wpos, spPos - wpos);
                    wpos = spPos + 1;
                }

                if (!firstInLine) {
                    TextSegment spaceSeg;
                    spaceSeg.text = wxS(" ");
                    spaceSeg.width = cachedSpaceWidth;
                    spaceSeg.height = cachedSpaceHeight;
                    spaceSeg.isNewline = false;
                    spaceSeg.isSpace = true;
                    segs.push_back(std::move(spaceSeg));
                }

                if (!word.IsEmpty()) {
                    wxCoord tw, th;
                    dc.GetTextExtent(word, &tw, &th);
                    TextSegment seg;
                    seg.text = word;
                    seg.width = tw;
                    seg.height = th;
                    seg.isNewline = false;
                    seg.isSpace = false;
                    segs.push_back(std::move(seg));
                }

                firstInLine = false;
            }

            if (nlPos < fullText.length()) {
                TextSegment nlSeg;
                nlSeg.text = wxEmptyString;
                nlSeg.width = 0;
                nlSeg.height = dc.GetCharHeight();
                nlSeg.isNewline = true;
                nlSeg.isSpace = false;
                segs.push_back(std::move(nlSeg));
            }
        }

        pos = nlPos + 1;
    }

    segmentCacheValid[blockIdx] = true;
    segmentCachedTextLen[blockIdx] = fullText.length();
}

// Phase 1b: Incremental segment measurement - only measure NEW text.
// Finds the last newline boundary in previously cached text, pops segments
// back to that boundary, and re-measures only from there.
void StreamingTextCtrl::MeasureSegmentsIncremental(wxDC& dc, size_t blockIdx) {
    const TextBlock* block = blockManager.GetBlock(blockIdx);
    if (!block || block->text.IsEmpty()) {
        MeasureSegments(dc, blockIdx);
        return;
    }
    
    auto& segs = segmentCache[blockIdx];
    size_t prevLen = segmentCachedTextLen[blockIdx];
    const wxString& fullText = block->text;
    
    // If no previous cache or text was modified (not just appended), do full measure
    if (segs.empty() || prevLen == 0 || prevLen > fullText.length()) {
        MeasureSegments(dc, blockIdx);
        return;
    }
    
    // Find the start of the last hard line in the previously cached text.
    // We need to re-measure from there because new text may continue that line.
    size_t lastNl = fullText.rfind('\n', prevLen > 0 ? prevLen - 1 : 0);
    size_t remeasureFrom = (lastNl == wxString::npos) ? 0 : lastNl + 1;
    
    // Pop segments belonging to the last incomplete hard line.
    // Walk backwards removing segments until we hit a newline segment or empty.
    while (!segs.empty()) {
        const auto& last = segs.back();
        if (last.isNewline && remeasureFrom > 0) {
            // Keep this newline segment - it belongs to a completed line
            break;
        }
        segs.pop_back();
    }
    
    // Now measure from remeasureFrom to end of text
    size_t pos = remeasureFrom;
    while (pos <= fullText.length()) {
        size_t nlPos = fullText.find('\n', pos);
        if (nlPos == wxString::npos) nlPos = fullText.length();

        if (nlPos == pos) {
            TextSegment seg;
            seg.text = wxEmptyString;
            seg.width = 0;
            seg.height = dc.GetCharHeight();
            seg.isNewline = true;
            seg.isSpace = false;
            segs.push_back(std::move(seg));
        } else {
            wxString hardLine = fullText.Mid(pos, nlPos - pos);
            size_t wpos = 0;
            bool firstInLine = true;

            while (wpos < hardLine.length()) {
                size_t spPos = hardLine.find(' ', wpos);

                wxString word;
                if (spPos == wxString::npos) {
                    word = hardLine.Mid(wpos);
                    wpos = hardLine.length();
                } else {
                    word = hardLine.Mid(wpos, spPos - wpos);
                    wpos = spPos + 1;
                }

                if (!firstInLine) {
                    TextSegment spaceSeg;
                    spaceSeg.text = wxS(" ");
                    spaceSeg.width = cachedSpaceWidth;
                    spaceSeg.height = cachedSpaceHeight;
                    spaceSeg.isNewline = false;
                    spaceSeg.isSpace = true;
                    segs.push_back(std::move(spaceSeg));
                }

                if (!word.IsEmpty()) {
                    wxCoord tw, th;
                    dc.GetTextExtent(word, &tw, &th);
                    TextSegment seg;
                    seg.text = word;
                    seg.width = tw;
                    seg.height = th;
                    seg.isNewline = false;
                    seg.isSpace = false;
                    segs.push_back(std::move(seg));
                }

                firstInLine = false;
            }

            if (nlPos < fullText.length()) {
                TextSegment nlSeg;
                nlSeg.text = wxEmptyString;
                nlSeg.width = 0;
                nlSeg.height = dc.GetCharHeight();
                nlSeg.isNewline = true;
                nlSeg.isSpace = false;
                segs.push_back(std::move(nlSeg));
            }
        }

        pos = nlPos + 1;
    }

    segmentCacheValid[blockIdx] = true;
    segmentCachedTextLen[blockIdx] = fullText.length();
}

// Phase 1c: Measure styled segments for blocks with StyledTextRuns.
// Each run may have a different font, so we measure each word with its own font.
// The segment cache stores font+colour per segment for use in layout and drawing.
void StreamingTextCtrl::MeasureStyledSegments(wxDC& dc, size_t blockIdx) {
    const TextBlock* block = blockManager.GetBlock(blockIdx);
    auto& segs = segmentCache[blockIdx];
    segs.clear();
    
    if (!block || block->runs.empty()) {
        TextSegment seg;
        seg.text = wxEmptyString;
        seg.width = 0;
        seg.height = dc.GetCharHeight();
        seg.isNewline = true;
        seg.isSpace = false;
        segs.push_back(std::move(seg));
        segmentCacheValid[blockIdx] = true;
        segmentCachedTextLen[blockIdx] = 0;
        return;
    }
    
    // Walk each styled run, split into word/space/newline segments
    for (const auto& run : block->runs) {
        dc.SetFont(run.font);
        
        const wxString& runText = run.text;
        size_t pos = 0;
        
        while (pos < runText.length()) {
            wxUniChar ch = runText[pos];
            
            if (ch == '\n') {
                TextSegment seg;
                seg.text = wxEmptyString;
                seg.width = 0;
                seg.height = dc.GetCharHeight();
                seg.isNewline = true;
                seg.isSpace = false;
                seg.font = run.font;
                seg.colour = run.colour;
                seg.hasStyle = true;
                segs.push_back(std::move(seg));
                pos++;
                continue;
            }
            
            if (ch == ' ') {
                wxCoord sw, sh;
                dc.GetTextExtent(wxS(" "), &sw, &sh);
                TextSegment seg;
                seg.text = wxS(" ");
                seg.width = sw;
                seg.height = sh;
                seg.isNewline = false;
                seg.isSpace = true;
                seg.font = run.font;
                seg.colour = run.colour;
                seg.hasStyle = true;
                segs.push_back(std::move(seg));
                pos++;
                continue;
            }
            
            // Collect a word (non-space, non-newline)
            size_t wordStart = pos;
            while (pos < runText.length() && runText[pos] != ' ' && runText[pos] != '\n') {
                pos++;
            }
            
            wxString word = runText.Mid(wordStart, pos - wordStart);
            wxCoord tw, th;
            dc.GetTextExtent(word, &tw, &th);
            
            TextSegment seg;
            seg.text = word;
            seg.width = tw;
            seg.height = th;
            seg.isNewline = false;
            seg.isSpace = false;
            seg.font = run.font;
            seg.colour = run.colour;
            seg.hasStyle = true;
            segs.push_back(std::move(seg));
        }
    }
    
    segmentCacheValid[blockIdx] = true;
    // Mark as non-incremental (styled blocks don't use incremental append)
    segmentCachedTextLen[blockIdx] = block->GetFullText().length();
}

// Phase 2: Line-breaking from cached segment widths. No DC needed.
void StreamingTextCtrl::LayoutFromSegments(size_t blockIdx, int textAreaWidth, int clientWidth,
                                            std::vector<WrappedLine>& outLines, int& outHeight) {
    outLines.clear();
    outHeight = 0;

    const TextBlock* block = blockManager.GetBlock(blockIdx);
    bool rtl = block ? block->rightToLeft : false;
    bool styled = block && block->HasStyledRuns();
    int indent = block ? block->leftIndent : 0;
    int effectiveMargin = leftMargin + indent;
    const auto& segs = segmentCache[blockIdx];

    if (segs.empty()) {
        WrappedLine wl = MakeLine(wxEmptyString, rtl, 0, lineHeight, effectiveMargin, clientWidth);
        wl.y = 0;
        outLines.push_back(std::move(wl));
        outHeight = lineHeight;
        return;
    }

    int availableWidth = (textAreaWidth - indent > 0) ? (textAreaWidth - indent) : 1;
    int yPos = 0;

    // Accumulate segments into lines
    wxString currentLine;
    currentLine.reserve(256);
    int currentWidth = 0;
    int currentHeight = 0;
    
    // For styled blocks: collect runs per line
    std::vector<StyledTextRun> currentRuns;
    std::vector<int> currentRunXOffsets;

    auto flushLine = [&]() {
        if (currentHeight == 0) currentHeight = lineHeight;
        WrappedLine wl = MakeLine(currentLine, rtl, currentWidth, currentHeight,
                                   effectiveMargin, clientWidth);
        wl.y = yPos;
        if (styled) {
            wl.styledRuns = std::move(currentRuns);
            wl.runXOffsets = std::move(currentRunXOffsets);
            currentRuns.clear();
            currentRunXOffsets.clear();
        }
        outLines.push_back(std::move(wl));
        yPos += currentHeight;
        currentLine.clear();
        currentWidth = 0;
        currentHeight = 0;
    };

    for (size_t si = 0; si < segs.size(); si++) {
        const auto& seg = segs[si];

        if (seg.isNewline) {
            flushLine();
            continue;
        }

        int testWidth = currentWidth + seg.width;

        if (testWidth <= availableWidth || currentLine.IsEmpty()) {
            // Fits, or first segment on line
            if (styled && seg.hasStyle) {
                if (seg.isSpace) {
                    // Append space to last run if exists, otherwise start new run
                    if (!currentRuns.empty()) {
                        currentRuns.back().text += seg.text;
                    }
                } else {
                    // Try to merge with last run if same style
                    if (!currentRuns.empty() && 
                        currentRuns.back().font == seg.font && 
                        currentRuns.back().colour == seg.colour) {
                        currentRuns.back().text += seg.text;
                    } else {
                        currentRunXOffsets.push_back(currentWidth);
                        currentRuns.emplace_back(seg.text, seg.font, seg.colour);
                    }
                }
            }
            currentLine += seg.text;
            currentWidth += seg.width;
            if (seg.height > currentHeight) currentHeight = seg.height;
        } else {
            flushLine();
            if (!seg.isSpace) {
                currentLine = seg.text;
                currentWidth = seg.width;
                currentHeight = seg.height;
                if (styled && seg.hasStyle) {
                    currentRunXOffsets.push_back(0);
                    currentRuns.emplace_back(seg.text, seg.font, seg.colour);
                }
            }
        }
    }

    if (!currentLine.IsEmpty()) {
        flushLine();
    }

    if (outHeight == 0) outHeight = lineHeight;
    outHeight = yPos;
    if (outHeight < lineHeight) outHeight = lineHeight;
}

// Full wrap: measure segments if needed, then layout
void StreamingTextCtrl::WrapBlock(wxDC& dc, const TextBlock* block, int textAreaWidth,
                                   int clientWidth,
                                   std::vector<WrappedLine>& outLines, int& outHeight,
                                   int& outCharHeight, size_t blockIdx) {
    outCharHeight = dc.GetCharHeight();

    // Ensure segment cache vectors are sized
    if (blockIdx >= segmentCache.size()) {
        segmentCache.resize(blockIdx + 1);
        segmentCacheValid.resize(blockIdx + 1, false);
        segmentCachedTextLen.resize(blockIdx + 1, 0);
    }
    if (!segmentCacheValid[blockIdx]) {
        if (block && block->HasStyledRuns()) {
            // Styled block: measure with per-run fonts
            MeasureStyledSegments(dc, blockIdx);
        } else {
            dc.SetFont(GetFontForType(block ? block->type : BlockType::NORMAL));
            // Use incremental if we have a partial cache
            if (segmentCachedTextLen[blockIdx] > 0 && !segmentCache[blockIdx].empty()) {
                MeasureSegmentsIncremental(dc, blockIdx);
            } else {
                MeasureSegments(dc, blockIdx);
            }
        }
    }

    LayoutFromSegments(blockIdx, textAreaWidth, clientWidth, outLines, outHeight);
}

// ============================================================================
// Prefix-sum blockTopCache for O(log N) visibility culling
// ============================================================================

void StreamingTextCtrl::RebuildBlockTopCache() {
    size_t n = blockHeightCache.size();
    blockTopCache.resize(n + 1);
    blockTopCache[0] = topMargin;
    for (size_t i = 0; i < n; i++) {
        blockTopCache[i + 1] = blockTopCache[i] + blockHeightCache[i] + blockSpacing;
    }
}

// O(1) update for when only blocks from fromIdx onwards changed
void StreamingTextCtrl::UpdateBlockTopCacheTail(size_t fromIdx) {
    size_t n = blockHeightCache.size();
    blockTopCache.resize(n + 1);
    if (fromIdx == 0) {
        blockTopCache[0] = topMargin;
    }
    for (size_t i = fromIdx; i < n; i++) {
        blockTopCache[i + 1] = blockTopCache[i] + blockHeightCache[i] + blockSpacing;
    }
}

size_t StreamingTextCtrl::FindFirstVisibleBlock(int scrollPos) const {
    if (blockTopCache.size() <= 1) return 0;

    size_t blockCount = blockTopCache.size() - 1;

    // Binary search: find last block whose top + height is above scrollPos
    // i.e., find first block whose bottom edge (blockTopCache[i+1]) > scrollPos
    size_t lo = 0, hi = blockCount;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (blockTopCache[mid + 1] <= scrollPos)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

// ============================================================================
// Hit-testing (uses blockTopCache for O(log N) block lookup)
// ============================================================================

int StreamingTextCtrl::CaretXForOffset(const WrappedLine& wl, int charOffset) const {
    int len = (int)wl.text.length();
    charOffset = std::clamp(charOffset, 0, len);
    if (wl.caretXValid && charOffset < (int)wl.caretX.size())
        return wl.caretX[charOffset];
    // Fallback for uncomputed caretX
    return wl.rightToLeft ? 0 : wl.width;
}

TextPosition StreamingTextCtrl::HitTest(int pixelX, int pixelY) const {
    size_t blockCount = blockManager.GetBlockCount();
    if (blockCount == 0 || wrappedLinesCache.empty() || blockTopCache.size() <= 1)
        return {};

    int virtualY = pixelY + scrollPositionPx;

    // Binary search for block using prefix-sum cache
    size_t bi = FindFirstVisibleBlock(virtualY);
    if (bi >= blockCount) bi = blockCount - 1;

    // Walk forward to find exact block (virtualY might be in spacing between blocks)
    while (bi < blockCount - 1) {
        int blockTop = blockTopCache[bi];
        int blockBottom = blockTop + blockHeightCache[bi];
        if (virtualY < blockBottom) break;
        // Check if in spacing gap
        if (virtualY < blockTopCache[bi + 1]) break;
        bi++;
    }

    if (bi >= wrappedLinesCache.size()) return {};
    const auto& lines = wrappedLinesCache[bi];
    if (lines.empty()) return {(int)bi, 0, 0};

    int blockTop = blockTopCache[bi];
    int localY = virtualY - blockTop;
    if (localY < 0) return {(int)bi, 0, 0};

    for (size_t li = 0; li < lines.size(); li++) {
        const auto& wl = lines[li];
        int lineBottom = wl.y + wl.height;

        if (localY < lineBottom || li == lines.size() - 1) {
            if (wl.text.IsEmpty())
                return {(int)bi, (int)li, 0};

            // Lazy: ensure caretX is computed for this line
            // We need a mutable reference but HitTest is const.
            // caretX and caretXValid are mutable, so this is safe.
            auto& mutableWl = const_cast<WrappedLine&>(wl);
            if (!mutableWl.caretXValid) {
                // We need a DC for GetPartialTextExtents.
                // Use wxClientDC since we may not be in OnPaint.
                wxClientDC tmpDc(const_cast<StreamingTextCtrl*>(this));
                const TextBlock* block = blockManager.GetBlock(bi);
                if (block) tmpDc.SetFont(GetFontForType(block->type));
                const_cast<StreamingTextCtrl*>(this)->EnsureCaretX(mutableWl, tmpDc);
            }

            int localX = pixelX - wl.x;
            int len = (int)wl.text.length();

            if (!wl.rightToLeft) {
                if (localX <= 0) return {(int)bi, (int)li, 0};
                if (localX >= wl.width) return {(int)bi, (int)li, len};

                int lo = 0, hi = len + 1;
                while (lo < hi) {
                    int mid = (lo + hi) / 2;
                    if (wl.caretX[mid] <= localX)
                        lo = mid + 1;
                    else
                        hi = mid;
                }

                if (lo == 0) return {(int)bi, (int)li, 0};
                if (lo > len) return {(int)bi, (int)li, len};

                int leftX = wl.caretX[lo - 1];
                int rightX = wl.caretX[lo];
                int midX = leftX + (rightX - leftX) / 2;
                return {(int)bi, (int)li, (localX < midX) ? lo - 1 : lo};
            } else {
                if (localX <= 0) return {(int)bi, (int)li, len};
                if (localX >= wl.width) return {(int)bi, (int)li, 0};

                int lo = 0, hi = len + 1;
                while (lo < hi) {
                    int mid = (lo + hi) / 2;
                    if (wl.caretX[mid] >= localX)
                        lo = mid + 1;
                    else
                        hi = mid;
                }

                if (lo == 0) return {(int)bi, (int)li, 0};
                if (lo > len) return {(int)bi, (int)li, len};

                int rightX = wl.caretX[lo - 1];
                int leftX = wl.caretX[lo];
                int midX = leftX + (rightX - leftX) / 2;
                return {(int)bi, (int)li, (localX >= midX) ? lo - 1 : lo};
            }
        }
    }

    const auto& lastLine = lines.back();
    return {(int)bi, (int)lines.size() - 1, (int)lastLine.text.length()};
}

// ============================================================================
// Selection helpers
// ============================================================================

bool StreamingTextCtrl::HasSelection() const {
    return m_selAnchor.IsValid() && m_selCaret.IsValid() && m_selAnchor != m_selCaret;
}

void StreamingTextCtrl::SelectAll() {
    size_t blockCount = blockManager.GetBlockCount();
    if (blockCount == 0 || wrappedLinesCache.empty()) return;

    m_selAnchor = {0, 0, 0};

    int lastBlock = (int)blockCount - 1;
    if (lastBlock < (int)wrappedLinesCache.size() && !wrappedLinesCache[lastBlock].empty()) {
        const auto& lastLines = wrappedLinesCache[lastBlock];
        int lastLine = (int)lastLines.size() - 1;
        m_selCaret = {lastBlock, lastLine, (int)lastLines[lastLine].text.length()};
    }
    Refresh();
}

void StreamingTextCtrl::ClearSelection() {
    m_selAnchor = {};
    m_selCaret = {};
    m_selecting = false;
    m_clickCount = 0;
    Refresh();
}

void StreamingTextCtrl::CopyToClipboard() const {
    if (!HasSelection()) return;

    wxString text = GetSelectedText();
    if (text.IsEmpty()) return;

    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(new wxTextDataObject(text));
        wxTheClipboard->Close();
    }
}

wxString StreamingTextCtrl::GetSelectedText() const {
    if (!HasSelection()) return wxEmptyString;

    TextPosition start, end;
    GetOrderedSelection(start, end);
    return GetTextBetween(start, end);
}

void StreamingTextCtrl::GetOrderedSelection(TextPosition& start, TextPosition& end) const {
    if (m_selAnchor <= m_selCaret) {
        start = m_selAnchor;
        end = m_selCaret;
    } else {
        start = m_selCaret;
        end = m_selAnchor;
    }
}

wxString StreamingTextCtrl::GetTextBetween(const TextPosition& start, const TextPosition& end) const {
    if (!start.IsValid() || !end.IsValid()) return wxEmptyString;

    wxString result;

    for (int bi = start.block; bi <= end.block && bi < (int)wrappedLinesCache.size(); ++bi) {
        const auto& lines = wrappedLinesCache[bi];
        int firstLine = (bi == start.block) ? start.line : 0;
        int lastLine = (bi == end.block) ? end.line : (int)lines.size() - 1;

        for (int li = firstLine; li <= lastLine && li < (int)lines.size(); ++li) {
            const auto& wl = lines[li];
            int from = (bi == start.block && li == start.line) ? start.offset : 0;
            int to = (bi == end.block && li == end.line) ? end.offset : (int)wl.text.length();

            from = std::clamp(from, 0, (int)wl.text.length());
            to = std::clamp(to, 0, (int)wl.text.length());

            if (from < to)
                result += wl.text.Mid(from, to - from);

            if (li < lastLine || bi < end.block)
                result += '\n';
        }
    }

    return result;
}

void StreamingTextCtrl::FindWordBoundary(const TextPosition& pos,
                                          TextPosition& wordStart,
                                          TextPosition& wordEnd) const {
    if (!pos.IsValid() || pos.block >= (int)wrappedLinesCache.size()) {
        wordStart = wordEnd = pos;
        return;
    }
    const auto& lines = wrappedLinesCache[pos.block];
    if (pos.line >= (int)lines.size()) {
        wordStart = wordEnd = pos;
        return;
    }

    const wxString& text = lines[pos.line].text;
    int len = (int)text.length();
    int off = std::clamp(pos.offset, 0, len);

    if (len == 0) {
        wordStart = wordEnd = {pos.block, pos.line, 0};
        return;
    }

    int charIdx = (off >= len) ? len - 1 : off;

    auto isWordChar = [](wxUniChar ch) -> bool {
        return wxIsalnum(ch) || ch == '_';
    };

    bool clickedWord = isWordChar(text[charIdx]);
    bool clickedSpace = wxIsspace(text[charIdx]);

    auto sameCategory = [&](wxUniChar ch) -> bool {
        if (clickedWord) return isWordChar(ch);
        if (clickedSpace) return wxIsspace(ch);
        return !isWordChar(ch) && !wxIsspace(ch);
    };

    int s = charIdx;
    while (s > 0 && sameCategory(text[s - 1]))
        --s;

    int e = charIdx;
    while (e < len && sameCategory(text[e]))
        ++e;

    wordStart = {pos.block, pos.line, s};
    wordEnd = {pos.block, pos.line, e};
}

// ============================================================================
// Selection drawing (lazily computes caretX for selected lines only)
// ============================================================================

void StreamingTextCtrl::DrawLineSelection(wxDC& dc, WrappedLine& wl,
                                           int blockIdx, int lineIdx,
                                           int blockTopY,
                                           const TextPosition& selStart,
                                           const TextPosition& selEnd) {
    int lineLen = (int)wl.text.length();
    TextPosition lineStart = {blockIdx, lineIdx, 0};
    TextPosition lineEnd = {blockIdx, lineIdx, lineLen};

    int fromChar, toChar;

    if (selStart <= lineStart && lineEnd <= selEnd) {
        fromChar = 0;
        toChar = lineLen;
    } else if (selStart > lineEnd || selEnd < lineStart) {
        return;
    } else {
        fromChar = (selStart.block == blockIdx && selStart.line == lineIdx)
                   ? selStart.offset : 0;
        toChar = (selEnd.block == blockIdx && selEnd.line == lineIdx)
                 ? selEnd.offset : lineLen;
    }

    fromChar = std::clamp(fromChar, 0, lineLen);
    toChar = std::clamp(toChar, 0, lineLen);
    if (fromChar >= toChar) return;

    // Lazy: compute caretX only for lines that actually have selection
    EnsureCaretX(wl, dc);

    int vx1 = CaretXForOffset(wl, fromChar);
    int vx2 = CaretXForOffset(wl, toChar);
    int left = std::min(vx1, vx2);
    int right = std::max(vx1, vx2);

    int absY = blockTopY + wl.y;

    // Draw selection highlight rectangle
    wxRect selRect(wl.x + left, absY, right - left, wl.height);
    dc.SetBrush(wxBrush(m_selBgColour));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(selRect);

    // Re-draw text clipped to selection rect in selection colour.
    // Preserves glyph shaping, ligatures, kerning.
    const TextBlock* block = blockManager.GetBlock(blockIdx);
    if (block) {
        dc.SetTextForeground(m_selTextColour);
        dc.SetClippingRegion(selRect);
        
        if (!wl.styledRuns.empty()) {
            // Styled line: redraw each run at layout positions, selection text colour
            for (size_t ri = 0; ri < wl.styledRuns.size(); ri++) {
                const auto& run = wl.styledRuns[ri];
                int runX = wl.x + (ri < wl.runXOffsets.size() ? wl.runXOffsets[ri] : 0);
                dc.SetFont(run.font);
                if (!run.text.IsEmpty()) {
                    dc.DrawText(run.text, runX, absY);
                }
            }
        } else {
            // Plain line: single DrawText
            dc.SetFont(GetFontForType(block->type));
            dc.DrawText(wl.text, wl.x, absY);
        }
        
        dc.DestroyClippingRegion();
    }
}

// ============================================================================
// Mouse handlers
// ============================================================================

void StreamingTextCtrl::OnMouseLeftDown(wxMouseEvent& evt) {
    SetFocus();

    TextPosition pos = HitTest(evt.GetX(), evt.GetY());

    long now = wxGetLocalTimeMillis().GetLo();
    bool isMultiClick = (now - m_lastClickTime) < MULTI_CLICK_THRESHOLD_MS
                        && pos.block == m_lastClickPos.block
                        && pos.line == m_lastClickPos.line;

    if (isMultiClick)
        m_clickCount++;
    else
        m_clickCount = 1;

    m_lastClickTime = now;
    m_lastClickPos = pos;

    if (m_clickCount >= 3) {
        m_clickCount = 3;
        if (pos.IsValid() && pos.block < (int)wrappedLinesCache.size()) {
            const auto& lines = wrappedLinesCache[pos.block];
            if (pos.line < (int)lines.size()) {
                m_selAnchor = {pos.block, pos.line, 0};
                m_selCaret = {pos.block, pos.line, (int)lines[pos.line].text.length()};
                m_wordSelAnchorStart = m_selAnchor;
                m_wordSelAnchorEnd = m_selCaret;
            }
        }
        m_selecting = true;
    } else if (m_clickCount == 2) {
        TextPosition wStart, wEnd;
        FindWordBoundary(pos, wStart, wEnd);
        m_selAnchor = wStart;
        m_selCaret = wEnd;
        m_wordSelAnchorStart = wStart;
        m_wordSelAnchorEnd = wEnd;
        m_selecting = true;
    } else {
        if (evt.ShiftDown() && m_selAnchor.IsValid()) {
            m_selCaret = pos;
        } else {
            m_selAnchor = pos;
            m_selCaret = pos;
        }
        m_selecting = true;
    }

    if (!HasCapture())
        CaptureMouse();

    Refresh();
}

void StreamingTextCtrl::OnMouseLeftDClick(wxMouseEvent& evt) {
    // Handled via click-counting in OnMouseLeftDown
}

void StreamingTextCtrl::OnMouseLeftUp(wxMouseEvent& evt) {
    if (m_selecting) {
        m_selecting = false;
        StopAutoScroll();
        if (HasCapture())
            ReleaseMouse();
    }
}

void StreamingTextCtrl::OnMouseMotion(wxMouseEvent& evt) {
    if (!m_selecting || !evt.LeftIsDown())
        return;

    TextPosition pos = HitTest(evt.GetX(), evt.GetY());

    if (m_clickCount >= 3) {
        if (pos.IsValid() && pos.block < (int)wrappedLinesCache.size()) {
            const auto& lines = wrappedLinesCache[pos.block];
            if (pos < m_wordSelAnchorStart) {
                m_selAnchor = m_wordSelAnchorEnd;
                m_selCaret = {pos.block, pos.line, 0};
            } else if (pos > m_wordSelAnchorEnd) {
                m_selAnchor = m_wordSelAnchorStart;
                int lineLen = (pos.line < (int)lines.size()) ? (int)lines[pos.line].text.length() : 0;
                m_selCaret = {pos.block, pos.line, lineLen};
            } else {
                m_selAnchor = m_wordSelAnchorStart;
                m_selCaret = m_wordSelAnchorEnd;
            }
        }
    } else if (m_clickCount == 2) {
        TextPosition wStart, wEnd;
        FindWordBoundary(pos, wStart, wEnd);

        if (pos < m_wordSelAnchorStart) {
            m_selAnchor = m_wordSelAnchorEnd;
            m_selCaret = wStart;
        } else if (pos > m_wordSelAnchorEnd) {
            m_selAnchor = m_wordSelAnchorStart;
            m_selCaret = wEnd;
        } else {
            m_selAnchor = m_wordSelAnchorStart;
            m_selCaret = m_wordSelAnchorEnd;
        }
    } else {
        m_selCaret = pos;
    }

    UpdateAutoScroll(evt);
    Refresh();
}

void StreamingTextCtrl::OnMouseCaptureLost(wxMouseCaptureLostEvent& evt) {
    m_selecting = false;
    StopAutoScroll();
}

// ============================================================================
// Auto-scroll for drag selection
// ============================================================================

void StreamingTextCtrl::UpdateAutoScroll(const wxMouseEvent& evt) {
    static constexpr int EDGE_MARGIN = 30;

    int clientH = GetClientSize().GetHeight();
    int mouseY = evt.GetY();

    if (mouseY < EDGE_MARGIN) {
        m_autoScrollDelta = mouseY - EDGE_MARGIN;
        if (!m_autoScrollTimer.IsRunning())
            m_autoScrollTimer.Start(AUTO_SCROLL_INTERVAL_MS);
    } else if (mouseY > clientH - EDGE_MARGIN) {
        m_autoScrollDelta = mouseY - (clientH - EDGE_MARGIN);
        if (!m_autoScrollTimer.IsRunning())
            m_autoScrollTimer.Start(AUTO_SCROLL_INTERVAL_MS);
    } else {
        StopAutoScroll();
    }
}

void StreamingTextCtrl::StopAutoScroll() {
    if (m_autoScrollTimer.IsRunning())
        m_autoScrollTimer.Stop();
    m_autoScrollDelta = 0;
}

void StreamingTextCtrl::OnAutoScrollTimer(wxTimerEvent& evt) {
    if (!m_selecting) {
        StopAutoScroll();
        return;
    }

    int clientHeight = GetClientSize().GetHeight();
    int maxScroll = std::max(0, cachedTotalHeight - clientHeight);

    int pixelDelta = std::clamp(m_autoScrollDelta, -60, 60);
    int newPos = scrollPositionPx + pixelDelta;
    newPos = std::clamp(newPos, 0, maxScroll);

    if (newPos != scrollPositionPx)
        scrollPositionPx = newPos;

    wxPoint mousePos = ScreenToClient(wxGetMousePosition());
    TextPosition pos = HitTest(mousePos.x, mousePos.y);

    if (m_clickCount >= 3) {
        if (pos.IsValid() && pos.block < (int)wrappedLinesCache.size()) {
            const auto& lines = wrappedLinesCache[pos.block];
            if (pos < m_wordSelAnchorStart) {
                m_selAnchor = m_wordSelAnchorEnd;
                m_selCaret = {pos.block, pos.line, 0};
            } else {
                m_selAnchor = m_wordSelAnchorStart;
                int lineLen = (pos.line < (int)lines.size()) ? (int)lines[pos.line].text.length() : 0;
                m_selCaret = {pos.block, pos.line, lineLen};
            }
        }
    } else if (m_clickCount == 2) {
        TextPosition wStart, wEnd;
        FindWordBoundary(pos, wStart, wEnd);
        if (pos < m_wordSelAnchorStart) {
            m_selAnchor = m_wordSelAnchorEnd;
            m_selCaret = wStart;
        } else {
            m_selAnchor = m_wordSelAnchorStart;
            m_selCaret = wEnd;
        }
    } else {
        m_selCaret = pos;
    }

    Refresh();
}

// ============================================================================
// Painting
// ============================================================================

void StreamingTextCtrl::OnPaint(wxPaintEvent& event) {
    wxPaintDC dc(this);
    wxSize clientSize = GetClientSize();
    int clientWidth = clientSize.GetWidth();
    int clientHeight = clientSize.GetHeight();

    bool widthChanged = (clientWidth != cachedWidth);
    size_t blockCount = blockManager.GetBlockCount();
    int textAreaWidth = clientWidth - leftMargin * 2;

    if (needsFullRebuild) {
        // Full rebuild: measure and wrap all blocks
        wrappedLinesCache.resize(blockCount);
        blockHeightCache.resize(blockCount);
        charHeightCache.resize(blockCount);
        blockDirty.assign(blockCount, false);
        segmentCache.resize(blockCount);
        segmentCacheValid.assign(blockCount, false);  // Force re-measure
        segmentCachedTextLen.assign(blockCount, 0);   // Reset incremental tracking
        cachedTotalHeight = topMargin * 2;

        for (size_t i = 0; i < blockCount; i++) {
            const TextBlock* block = blockManager.GetBlock(i);
            dc.SetFont(GetFontForType(block ? block->type : BlockType::NORMAL));

            int h = 0, ch = 0;
            WrapBlock(dc, block, textAreaWidth, clientWidth, wrappedLinesCache[i], h, ch, i);
            blockHeightCache[i] = h;
            charHeightCache[i] = ch;
            cachedTotalHeight += h + blockSpacing;
        }

        cachedWidth = clientWidth;
        needsFullRebuild = false;
        RebuildBlockTopCache();
    } else if (widthChanged) {
        // Width changed: segment measurements are still valid!
        // Just re-run layout (pure arithmetic) for blocks that have been measured.
        size_t cachedCount = std::min({wrappedLinesCache.size(), segmentCache.size(), blockHeightCache.size()});
        cachedTotalHeight = topMargin * 2;
        for (size_t i = 0; i < cachedCount; i++) {
            if (i < segmentCacheValid.size() && segmentCacheValid[i]) {
                int h = 0;
                LayoutFromSegments(i, textAreaWidth, clientWidth, wrappedLinesCache[i], h);
                blockHeightCache[i] = h;
            }
            cachedTotalHeight += blockHeightCache[i] + blockSpacing;
        }
        blockDirty.assign(cachedCount, false);
        cachedWidth = clientWidth;
        RebuildBlockTopCache();
    }

    // Handle new blocks (incremental append)
    if (wrappedLinesCache.size() < blockCount) {
        size_t oldSize = wrappedLinesCache.size();
        wrappedLinesCache.resize(blockCount);
        blockHeightCache.resize(blockCount);
        charHeightCache.resize(blockCount);
        blockDirty.resize(blockCount, false);
        segmentCache.resize(blockCount);
        segmentCacheValid.resize(blockCount, false);
        segmentCachedTextLen.resize(blockCount, 0);

        for (size_t i = oldSize; i < blockCount; i++) {
            const TextBlock* block = blockManager.GetBlock(i);
            dc.SetFont(GetFontForType(block ? block->type : BlockType::NORMAL));

            int h = 0, ch = 0;
            WrapBlock(dc, block, textAreaWidth, clientWidth, wrappedLinesCache[i], h, ch, i);
            blockHeightCache[i] = h;
            charHeightCache[i] = ch;
            cachedTotalHeight += h + blockSpacing;
        }

        // O(1) update: only rebuild from the first new block
        UpdateBlockTopCacheTail(oldSize);
    }

    if (blockCount == 0) {
        dc.SetBackground(wxBrush(backgroundColor));
        dc.Clear();
        if (!updatingScrollbar) {
            updatingScrollbar = true;
            SetScrollbar(wxVERTICAL, 0, clientHeight, 0);
            updatingScrollbar = false;
        }
        return;
    }

    int totalHeight = cachedTotalHeight;

    // Handle scroll-to-bottom
    if (scrollToBottomPending) {
        int maxScroll = std::max(0, totalHeight - clientHeight);
        scrollPositionPx = maxScroll;
        scrollToBottomPending = false;
    }

    int maxScroll = std::max(0, totalHeight - clientHeight);
    if (scrollPositionPx > maxScroll) scrollPositionPx = maxScroll;
    if (scrollPositionPx < 0) scrollPositionPx = 0;

    // Update scrollbar
    if (!updatingScrollbar) {
        int currentPos = GetScrollPos(wxVERTICAL);
        int currentRange = GetScrollRange(wxVERTICAL);
        int currentPage = GetScrollThumb(wxVERTICAL);

        if (scrollPositionPx != currentPos || totalHeight != currentRange || clientHeight != currentPage) {
            updatingScrollbar = true;
            SetScrollbar(wxVERTICAL, scrollPositionPx, clientHeight, totalHeight);
            updatingScrollbar = false;
        }
    }

    // Selection state
    TextPosition selStart, selEnd;
    bool hasSel = HasSelection();
    if (hasSel)
        GetOrderedSelection(selStart, selEnd);

    // --- No dc.Clear(): fill background only for visible gaps ---
    // Fill entire viewport with background first (simpler, avoids gap tracking)
    // but use a single fill rather than dc.Clear() + dc.SetBackground()
    dc.SetBrush(wxBrush(backgroundColor));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, clientWidth, clientHeight);

    // O(log N) first visible block lookup
    size_t firstVisible = FindFirstVisibleBlock(scrollPositionPx);

    // Draw visible blocks
    for (size_t i = firstVisible; i < blockCount; i++) {
        int blockHeight = blockHeightCache[i];
        int blockTop = blockTopCache[i] - scrollPositionPx;

        // Stop if past viewport
        if (blockTop > clientHeight) break;

        const TextBlock* block = blockManager.GetBlock(i);
        if (!block) continue;

        dc.SetFont(GetFontForType(block->type));

        // Draw block background
        if (block->type == BlockType::CODE) {
            dc.SetBrush(wxBrush(codeBackground));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(leftMargin - 5, blockTop, textAreaWidth + 10, blockHeight + 4);
        } else if (block->type == BlockType::THINKING) {
            dc.SetBrush(wxBrush(thinkingBackground));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(leftMargin - 5, blockTop, textAreaWidth + 10, blockHeight + 4);
        } else if (block->type == BlockType::USER_PROMPT) {
            dc.SetBrush(wxBrush(userPromptBackground));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(leftMargin - 5, blockTop, textAreaWidth + 10, blockHeight + 4);
            dc.SetPen(wxPen(userPromptColor, 3));
            dc.DrawLine(leftMargin - 5, blockTop, leftMargin - 5, blockTop + blockHeight + 4);
        }

        // Draw each wrapped line
        auto& lines = wrappedLinesCache[i];
        for (size_t li = 0; li < lines.size(); li++) {
            auto& wl = lines[li];
            int absY = blockTop + wl.y;

            if (absY + wl.height < 0) continue;
            if (absY > clientHeight) break;

            if (!wl.styledRuns.empty()) {
                // Styled line: draw each run with its own font/colour
                // Use runXOffsets from layout for positioning (no DC measurement needed)
                for (size_t ri = 0; ri < wl.styledRuns.size(); ri++) {
                    const auto& run = wl.styledRuns[ri];
                    int runX = wl.x + (ri < wl.runXOffsets.size() ? wl.runXOffsets[ri] : 0);
                    dc.SetFont(run.font);
                    dc.SetTextForeground(run.colour);
                    if (!run.text.IsEmpty()) {
                        dc.DrawText(run.text, runX, absY);
                    }
                }
            } else {
                // Plain line: single DrawText
                dc.SetTextForeground(GetColorForType(block->type));
                if (!wl.text.IsEmpty()) {
                    dc.DrawText(wl.text, wl.x, absY);
                }
            }

            // Selection overlay (lazily computes caretX only for selected lines)
            if (hasSel) {
                TextPosition lineStart = {(int)i, (int)li, 0};
                TextPosition lineEnd = {(int)i, (int)li, (int)wl.text.length()};

                if (selStart <= lineEnd && selEnd >= lineStart) {
                    DrawLineSelection(dc, wl, (int)i, (int)li, blockTop, selStart, selEnd);
                }
            }
        }

        // Loading indicator
        if (block->isLoading && !lines.empty()) {
            const auto& lastLine = lines.back();
            int ch = charHeightCache[i];
            int dotRadius = std::max(3, ch / 4);
            int dw = dotRadius * 2;
            int dotY = blockTop + lastLine.y + ch / 2;

            DrawLoadingIndicator(dc, leftMargin + lastLine.width + dw, dotY, ch);
        }
    }
}

// ============================================================================
// Events
// ============================================================================

void StreamingTextCtrl::OnSize(wxSizeEvent& event) {
    Refresh();
    event.Skip();
}

void StreamingTextCtrl::OnScroll(wxScrollWinEvent& event) {
    if (updatingScrollbar) return;

    int clientHeight = GetClientSize().GetHeight();
    int maxScroll = std::max(0, cachedTotalHeight - clientHeight);
    int newPos = scrollPositionPx;

    wxEventType type = event.GetEventType();
    if (type == wxEVT_SCROLLWIN_THUMBTRACK || type == wxEVT_SCROLLWIN_THUMBRELEASE) {
        newPos = event.GetPosition();
    } else if (type == wxEVT_SCROLLWIN_LINEUP) {
        newPos -= lineHeight;
    } else if (type == wxEVT_SCROLLWIN_LINEDOWN) {
        newPos += lineHeight;
    } else if (type == wxEVT_SCROLLWIN_PAGEUP) {
        newPos -= clientHeight;
    } else if (type == wxEVT_SCROLLWIN_PAGEDOWN) {
        newPos += clientHeight;
    } else if (type == wxEVT_SCROLLWIN_TOP) {
        newPos = 0;
    } else if (type == wxEVT_SCROLLWIN_BOTTOM) {
        newPos = maxScroll;
    }

    newPos = std::max(0, std::min(newPos, maxScroll));

    if (newPos != scrollPositionPx) {
        scrollPositionPx = newPos;
        Refresh();
    }
}

void StreamingTextCtrl::OnMouseWheel(wxMouseEvent& event) {
    int rotation = event.GetWheelRotation();
    int delta = event.GetWheelDelta();
    if (delta == 0) return;

    wheelRotationAccum += rotation;
    int ticks = wheelRotationAccum / delta;
    if (ticks == 0) return;
    wheelRotationAccum -= ticks * delta;

    int lines = -ticks * 3;
    int pixelDelta = lines * lineHeight;
    int newScrollPos = scrollPositionPx + pixelDelta;

    int clientHeight = GetClientSize().GetHeight();
    int maxScroll = std::max(0, cachedTotalHeight - clientHeight);
    newScrollPos = std::max(0, std::min(newScrollPos, maxScroll));

    if (newScrollPos != scrollPositionPx) {
        scrollPositionPx = newScrollPos;
        Refresh();
    }
}

void StreamingTextCtrl::OnKeyDown(wxKeyEvent& event) {
    if (event.ControlDown() || event.CmdDown()) {
        switch (event.GetKeyCode()) {
        case 'A':
            SelectAll();
            return;
        case 'C':
            CopyToClipboard();
            return;
        }
    }

    if (event.GetKeyCode() == WXK_ESCAPE) {
        ClearSelection();
        return;
    }

    int clientHeight = GetClientSize().GetHeight();

    switch (event.GetKeyCode()) {
        case WXK_UP:       scrollPositionPx -= lineHeight; break;
        case WXK_DOWN:     scrollPositionPx += lineHeight; break;
        case WXK_PAGEUP:   scrollPositionPx -= clientHeight; break;
        case WXK_PAGEDOWN:
        case WXK_SPACE:    scrollPositionPx += clientHeight; break;
        case WXK_HOME:     scrollPositionPx = 0; break;
        case WXK_END:      scrollToBottomPending = true; break;
        default: event.Skip(); return;
    }
    Refresh();
}

void StreamingTextCtrl::OnEraseBackground(wxEraseEvent& event) {
    // Do nothing
}

void StreamingTextCtrl::OnSystemColourChanged(wxSysColourChangedEvent& event) {
    UpdateThemeColors();
    Refresh();
    event.Skip();
}

// ============================================================================
// Content management
// ============================================================================

void StreamingTextCtrl::AppendStream(BlockType type, const wxString& text, bool rtl) {
    bool isRtl = rtl || DetectRTL(text);

    size_t oldCount = blockManager.GetBlockCount();
    blockManager.AppendStream(type, text, isRtl);
    size_t newCount = blockManager.GetBlockCount();

    size_t lastIdx = newCount - 1;

    if (newCount == oldCount && lastIdx < wrappedLinesCache.size()) {
        cachedTotalHeight -= blockHeightCache[lastIdx] + blockSpacing;
        wrappedLinesCache.resize(lastIdx);
        blockHeightCache.resize(lastIdx);
        charHeightCache.resize(lastIdx);
        // Invalidate segment cache for merged block (text changed)
        if (lastIdx < segmentCacheValid.size())
            segmentCacheValid[lastIdx] = false;
    }

    if (!inBatch) {
        if (autoScroll) scrollToBottomPending = true;
        Refresh();
    }
}

void StreamingTextCtrl::BeginBatch() {
    inBatch = true;
}

void StreamingTextCtrl::EndBatch() {
    inBatch = false;
    needsFullRebuild = true;
    if (autoScroll) scrollToBottomPending = true;
    Refresh();
}

void StreamingTextCtrl::ContinueStream(const wxString& text) {
    if (blockManager.GetBlockCount() > 0) {
        TextBlock* lastBlock = blockManager.GetLastBlock();
        if (lastBlock) {
            lastBlock->text += text;
            size_t lastIdx = blockManager.GetBlockCount() - 1;
            if (lastIdx < wrappedLinesCache.size()) {
                cachedTotalHeight -= blockHeightCache[lastIdx] + blockSpacing;
                wrappedLinesCache.resize(lastIdx);
                blockHeightCache.resize(lastIdx);
                charHeightCache.resize(lastIdx);
            }
            // Mark segment cache invalid but KEEP the cached segments + text length
            // so MeasureSegmentsIncremental can do incremental measurement
            if (lastIdx < segmentCacheValid.size())
                segmentCacheValid[lastIdx] = false;
            // Note: segmentCachedTextLen[lastIdx] retains old value for incremental
            
            if (!inBatch) {
                if (autoScroll) scrollToBottomPending = true;
                // Only repaint the bottom portion where streaming text appears
                int clientHeight = GetClientSize().GetHeight();
                int refreshHeight = std::min(clientHeight, clientHeight / 2);
                wxRect refreshRect(0, clientHeight - refreshHeight, 
                                   GetClientSize().GetWidth(), refreshHeight);
                RefreshRect(refreshRect);
            }
        }
    } else {
        AppendStream(BlockType::NORMAL, text);
    }
}

void StreamingTextCtrl::Clear() {
    blockManager.Clear();
    scrollPositionPx = 0;
    animatedBlocks.clear();
    if (animator && animator->IsRunning())
        animator->Stop();
    wrappedLinesCache.clear();
    blockHeightCache.clear();
    charHeightCache.clear();
    blockDirty.clear();
    segmentCache.clear();
    segmentCacheValid.clear();
    segmentCachedTextLen.clear();
    blockTopCache.clear();
    cachedTotalHeight = topMargin * 2;
    needsFullRebuild = true;
    scrollToBottomPending = false;
    ClearSelection();
    Refresh();
}

void StreamingTextCtrl::ScrollToBottom() {
    scrollToBottomPending = true;
    Refresh();
}

void StreamingTextCtrl::RemoveLastBlock() {
    size_t count = blockManager.GetBlockCount();
    if (count > 0) {
        RemoveBlocksFrom(count - 1);
    }
}

void StreamingTextCtrl::RemoveBlocksFrom(size_t fromIndex) {
    size_t count = blockManager.GetBlockCount();
    if (fromIndex >= count) return;
    
    // Remove blocks from block manager
    blockManager.RemoveBlocksFrom(fromIndex);
    
    // Truncate all caches to fromIndex
    if (wrappedLinesCache.size() > fromIndex)
        wrappedLinesCache.resize(fromIndex);
    if (blockHeightCache.size() > fromIndex) {
        // Recalculate total height
        cachedTotalHeight = topMargin * 2;
        for (size_t i = 0; i < fromIndex; i++) {
            cachedTotalHeight += blockHeightCache[i] + blockSpacing;
        }
        blockHeightCache.resize(fromIndex);
    }
    if (charHeightCache.size() > fromIndex)
        charHeightCache.resize(fromIndex);
    if (blockDirty.size() > fromIndex)
        blockDirty.resize(fromIndex);
    if (segmentCache.size() > fromIndex)
        segmentCache.resize(fromIndex);
    if (segmentCacheValid.size() > fromIndex)
        segmentCacheValid.resize(fromIndex);
    if (segmentCachedTextLen.size() > fromIndex)
        segmentCachedTextLen.resize(fromIndex);
    if (blockTopCache.size() > fromIndex + 1)
        blockTopCache.resize(fromIndex + 1);
}

void StreamingTextCtrl::RenderMarkdown(const std::string& markdown) {
    // Create markdown renderer with current normal font
    MarkdownRenderer renderer(normalFont);
    
    // Render markdown to styled blocks
    auto newBlocks = renderer.Render(markdown, isDarkTheme);
    
    if (newBlocks.empty()) return;
    
    // Add the rendered blocks to the block manager.
    // Don't resize caches here - let OnPaint's "new blocks appended" path
    // handle them incrementally (only measures the new blocks, O(1) blockTopCache update).
    blockManager.AddBlocks(std::move(newBlocks));
    
    if (!inBatch) {
        if (autoScroll) scrollToBottomPending = true;
        Refresh();
    }
}

// ============================================================================
// Thinking animation
// ============================================================================

void StreamingTextCtrl::StartThinking(size_t blockIndex) {
    animatedBlocks.insert(blockIndex);
    blockManager.SetBlockLoading(blockIndex, true);
    if (!animator->IsRunning()) animator->Start();
    Refresh();
}

void StreamingTextCtrl::StopThinking(size_t blockIndex) {
    animatedBlocks.erase(blockIndex);
    blockManager.SetBlockLoading(blockIndex, false);
    if (animatedBlocks.empty()) animator->Stop();
    Refresh();
}

int StreamingTextCtrl::GetLoadingFrame() const {
    return animator ? animator->GetFrame() : 0;
}

// ============================================================================
// Font/Color helpers (return by const reference to avoid copies)
// ============================================================================

void StreamingTextCtrl::SetNormalFont(const wxFont& font) { normalFont = font; }
void StreamingTextCtrl::SetUserPromptFont(const wxFont& font) { userPromptFont = font; }
void StreamingTextCtrl::SetThinkingFont(const wxFont& font) { thinkingFont = font; }
void StreamingTextCtrl::SetCodeFont(const wxFont& font) { codeFont = font; }

const wxFont& StreamingTextCtrl::GetFontForType(BlockType type) const {
    switch (type) {
        case BlockType::USER_PROMPT: return userPromptFont;
        case BlockType::THINKING: return thinkingFont;
        case BlockType::CODE:     return codeFont;
        default:                  return normalFont;
    }
}

const wxColour& StreamingTextCtrl::GetColorForType(BlockType type) const {
    switch (type) {
        case BlockType::USER_PROMPT: return userPromptColor;
        case BlockType::THINKING: return thinkingColor;
        case BlockType::CODE:     return codeColor;
        default:                  return normalColor;
    }
}

const wxColour& StreamingTextCtrl::GetBackgroundColorForType(BlockType type) const {
    switch (type) {
        case BlockType::USER_PROMPT: return userPromptBackground;
        case BlockType::CODE:     return codeBackground;
        case BlockType::THINKING: return thinkingBackground;
        default:                  return backgroundColor;
    }
}

void StreamingTextCtrl::DrawLoadingIndicator(wxDC& dc, int x, int y, int charHeight) {
    int currentFrame = animator ? animator->GetFrame() : 0;

    wxColour activeColor = isDarkTheme ? wxColour(200, 200, 200) : wxColour(100, 100, 100);
    wxColour dimColor = isDarkTheme ? wxColour(80, 80, 80) : wxColour(180, 180, 180);

    dc.SetPen(*wxTRANSPARENT_PEN);

    int dotRadius = std::max(3, charHeight / 4);
    int dw = dotRadius * 2;
    int dotSpacing = dw / 4;

    for (int i = 0; i < LOADING_DOT_COUNT; i++) {
        bool isActive = (i == (currentFrame % LOADING_DOT_COUNT));
        dc.SetBrush(wxBrush(isActive ? activeColor : dimColor));
        int r = isActive ? dotRadius : std::max(1, dotRadius - 1);
        dc.DrawCircle(x + i * (dw + dotSpacing), y, r);
    }
}

// ============================================================================
// Theme
// ============================================================================

void StreamingTextCtrl::UpdateThemeColors() {
    isDarkTheme = wxSystemSettings::GetAppearance().IsDark();

    normalColor = wxSystemSettings::SelectLightDark(
        wxColour(50, 50, 50), wxColour(220, 220, 220));
    userPromptColor = wxSystemSettings::SelectLightDark(
        wxColour(0, 100, 200), wxColour(100, 180, 255));
    thinkingColor = wxSystemSettings::SelectLightDark(
        wxColour(128, 128, 128), wxColour(160, 160, 160));
    codeColor = wxSystemSettings::SelectLightDark(
        wxColour(30, 30, 30), wxColour(230, 230, 230));
    codeBackground = wxSystemSettings::SelectLightDark(
        wxColour(245, 245, 245), wxColour(45, 45, 48));
    thinkingBackground = wxSystemSettings::SelectLightDark(
        wxColour(250, 250, 250), wxColour(40, 40, 43));
    userPromptBackground = wxSystemSettings::SelectLightDark(
        wxColour(240, 248, 255), wxColour(35, 45, 55));
    backgroundColor = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

    m_selBgColour = wxSystemSettings::SelectLightDark(
        wxColour(51, 153, 255), wxColour(51, 153, 255));
    m_selTextColour = wxSystemSettings::SelectLightDark(
        wxColour(255, 255, 255), wxColour(255, 255, 255));
}

bool StreamingTextCtrl::IsDarkTheme() const { return isDarkTheme; }
