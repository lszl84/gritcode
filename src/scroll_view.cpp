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

#include "scroll_view.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

static constexpr float ANIMATION_INTERVAL = 0.15f;
static constexpr int LOADING_DOT_COUNT = 3;
static constexpr double MULTI_CLICK_THRESHOLD = 0.4;

ScrollView::ScrollView() = default;

void ScrollView::InitColors() {
    // Palette ported from the wxWidgets branch's chat canvas. Coherent
    // tints instead of independently-picked colors — bg, code, bubble, and
    // tool surfaces all sit on the same neutral hue family.
#ifdef NDEBUG
    bgColor_ = Color::RGB(34, 34, 38);
#else
    bgColor_ = Color::RGB(34, 26, 26);  // Reddish tint in debug
#endif
    normalColor_ = Color::RGB(220, 220, 220);
    userPromptColor_ = Color::RGB(255, 255, 255);
    thinkingColor_ = Color::RGB(170, 170, 180);
    codeColor_ = Color::RGB(210, 215, 225);
    codeBg_ = Color::RGB(44, 44, 50);
    thinkingBg_ = Color::RGB(40, 40, 43);
    userPromptBg_ = Color::RGB(50, 72, 110);
    toolHeaderBg_ = Color::RGB(50, 56, 70);
    toolBodyBg_ = Color::RGB(40, 42, 50);
    toolAccent_ = Color::RGB(140, 200, 255);
    toolDim_ = Color::RGB(140, 145, 158);
    // Tables sit on the page bg (no body fill) with just the header tinted —
    // matches the wx-branch look.
    tableHeaderBg_ = Color::RGB(44, 44, 52);
    tableBorderColor_ = Color::RGB(90, 90, 100);
    selBgColor_ = Color::RGB(51, 153, 255);
    selTextColor_ = Color::RGB(255, 255, 255);
}

bool ScrollView::Init(int w, int h, float scale) {
    windowW_ = w;
    windowH_ = h;
    InitColors();

    int baseFontPx = (int)(15 * scale + 0.5f);
    if (baseFontPx < 15) baseFontPx = 15;
    if (!fonts_.Init(baseFontPx)) return false;

    // Centered chat column (ported from wxWidgets branch). Column caps at
    // ~720px; below that the column shrinks down to the sideMargin floor so
    // the UI still fills narrower windows reasonably.
    sideMargin_ = 24 * scale;
    maxContentW_ = 720 * scale;
    RecomputeColumn();

    userBubblePad_ = 12 * scale;
    userBubbleRadius_ = 8 * scale;
    codeHPad_ = 12 * scale;
    codeVPad_ = 8 * scale;
    codeRadius_ = 6 * scale;
    toolPadX_ = 10 * scale;
    toolPadY_ = 6 * scale;
    toolGap_ = 0;  // header and body share one bubble — no inter-strip gap
    toolRadius_ = 6 * scale;
    tableHPad_ = 10 * scale;
    tableVPad_ = 6 * scale;
    tableBorderW_ = std::max(1.0f, scale);
    paraLineGap_ = 0;

    float lh = fonts_.LineHeight(FontStyle::Regular);
    topMargin_ = lh;
    blockSpacing_ = lh / 2;
    cachedTotalH_ = topMargin_ * 2;

    return true;
}

void ScrollView::RecomputeColumn() {
    float cw = (float)windowW_ - sideMargin_ * 2;
    if (maxContentW_ > 0 && cw > maxContentW_) cw = maxContentW_;
    if (cw < 100) cw = 100;
    contentW_ = cw;
    float floorMargin = sideMargin_ > 0 ? sideMargin_ : 10.0f;
    leftMargin_ = std::max(floorMargin, ((float)windowW_ - contentW_) / 2.0f);
}

FontStyle ScrollView::StyleForType(BlockType t) const {
    switch (t) {
    case BlockType::THINKING: return FontStyle::ThinkingItalic;
    case BlockType::CODE: return FontStyle::Code;
    case BlockType::TOOL_CALL: return FontStyle::Code;
    default: return FontStyle::Regular;
    }
}

Color ScrollView::ColorForType(BlockType t) const {
    switch (t) {
    case BlockType::USER_PROMPT: return userPromptColor_;
    case BlockType::THINKING: return thinkingColor_;
    case BlockType::CODE: return codeColor_;
    case BlockType::TOOL_CALL: return codeColor_;
    default: return normalColor_;
    }
}

Color ScrollView::BgForType(BlockType t) const {
    // USER_PROMPT/CODE/TOOL_CALL chrome is drawn as a rounded shape in Paint,
    // not as a full-block rectangular fill — return transparent so the generic
    // bg pass skips them.
    switch (t) {
    case BlockType::THINKING: return thinkingBg_;
    default: return Color(0, 0, 0, 0);
    }
}

// --- Content management ---

void ScrollView::AppendStream(BlockType type, const std::string& text, bool rtl) {
    if (rtl == false) rtl = DetectRTL(text);
    auto block = std::make_unique<TextBlock>(type, text, rtl);
    blocks_.push_back(std::move(block));

    if (autoScroll_) scrollPos_ = 999999;
    if (!inBatch_) needsFullRebuild_ = true;
    needsRedraw_ = true;
}

void ScrollView::ContinueStream(const std::string& text) {
    if (blocks_.empty()) return;
    auto& last = blocks_.back();
    last->text += text;
    if (!last->rightToLeft) last->rightToLeft = DetectRTL(last->text);

    size_t idx = blocks_.size() - 1;
    if (idx < segValid_.size()) segValid_[idx] = false;
    if (idx < shapedCache_.size()) shapedCache_[idx].clear();

    if (autoScroll_) scrollPos_ = 999999;
    if (!inBatch_) streamDirty_ = true;
    needsRedraw_ = true;
}

void ScrollView::AddBlocks(std::vector<std::unique_ptr<TextBlock>> newBlocks) {
    for (auto& b : newBlocks) {
        if (!b->rightToLeft) b->rightToLeft = DetectRTL(b->GetFullText());
        blocks_.push_back(std::move(b));
    }
    if (autoScroll_) scrollPos_ = 999999;
    if (!inBatch_) needsFullRebuild_ = true;
    needsRedraw_ = true;
}

void ScrollView::RemoveBlocksFrom(size_t from) {
    if (from < blocks_.size()) blocks_.erase(blocks_.begin() + from, blocks_.end());
    needsFullRebuild_ = true;
    needsRedraw_ = true;
}

void ScrollView::Clear() {
    blocks_.clear();
    wrappedCache_.clear();
    shapedCache_.clear();
    blockHeightCache_.clear();
    charHeightCache_.clear();
    blockTopCache_.clear();
    segCache_.clear();
    segValid_.clear();
    segTextLen_.clear();
    animatedBlocks_.clear();
    scrollPos_ = 0;
    cachedTotalH_ = topMargin_ * 2;
    cachedW_ = -1;
    needsFullRebuild_ = true;
    needsRedraw_ = true;
    ClearSelection();
}

void ScrollView::BeginBatch() { inBatch_ = true; }
void ScrollView::EndBatch() {
    inBatch_ = false;
    needsFullRebuild_ = true;
    needsRedraw_ = true;
    if (autoScroll_) scrollPos_ = 999999;
}

// --- Scrolling ---

void ScrollView::ScrollToBottom() { scrollPos_ = 999999; }

void ScrollView::ScrollBy(float delta) {
    float oldPos = scrollPos_;
    scrollPos_ -= delta;
    float maxS = std::max(0.0f, cachedTotalH_ - windowH_);
    scrollPos_ = std::clamp(scrollPos_, 0.0f, maxS);
    autoScroll_ = (scrollPos_ >= maxS - 1);
    if (scrollPos_ != oldPos) needsRedraw_ = true;
}

void ScrollView::OnScroll(float yOffset) {
    float lineH = fonts_.LineHeight(FontStyle::Regular);
    ScrollBy(yOffset * lineH * 3);
}

void ScrollView::OnResize(int w, int h) {
    windowW_ = w;
    windowH_ = h;
    RecomputeColumn();
    needsFullRebuild_ = true;
    needsRedraw_ = true;
}

// --- Animation ---

void ScrollView::StartThinking(size_t idx) {
    if (idx >= blocks_.size()) return;
    for (auto i : animatedBlocks_) {
        if (i < blocks_.size()) blocks_[i]->isLoading = false;
    }
    animatedBlocks_.clear();
    blocks_[idx]->isLoading = true;
    animatedBlocks_.insert(idx);
    needsRedraw_ = true;
}

void ScrollView::StopThinking(size_t idx) {
    if (idx < blocks_.size()) blocks_[idx]->isLoading = false;
    animatedBlocks_.erase(idx);
    needsRedraw_ = true;
}

void ScrollView::StopAllAnimations() {
    for (auto idx : animatedBlocks_) {
        if (idx < blocks_.size()) blocks_[idx]->isLoading = false;
    }
    animatedBlocks_.clear();
}

void ScrollView::ToggleCollapse(size_t idx) {
    if (idx >= blocks_.size()) return;
    auto& b = blocks_[idx];
    b->isCollapsed = !b->isCollapsed;
    needsFullRebuild_ = true;
    needsRedraw_ = true;
}

void ScrollView::Update(float dt) {
    animTime_ += dt;
    if (animTime_ >= ANIMATION_INTERVAL) {
        animTime_ = 0;
        loadingFrame_ = (loadingFrame_ + 1) % LOADING_DOT_COUNT;
        if (!animatedBlocks_.empty()) needsRedraw_ = true;
    }

    // Auto-scroll during drag selection
    if (selecting_ && std::abs(autoScrollDelta_) > 0) {
        float oldPos = scrollPos_;
        float maxS = std::max(0.0f, cachedTotalH_ - windowH_);
        scrollPos_ += autoScrollDelta_ * dt * 120;
        scrollPos_ = std::clamp(scrollPos_, 0.0f, maxS);
        if (scrollPos_ != oldPos) needsRedraw_ = true;
    }
}

// --- Layout ---

WrappedLine ScrollView::MakeLine(const std::string& text, bool rtl, float textW,
                                  float textH, float margin, float clientW, int indent) {
    WrappedLine wl;
    wl.text = text;
    wl.rightToLeft = rtl;
    wl.width = textW;
    wl.height = textH;
    float effectiveMargin = margin + indent;
    if (rtl && textW > 0) {
        float xPos = clientW - margin - textW;
        wl.x = std::max(effectiveMargin, xPos);
    } else {
        wl.x = effectiveMargin;
    }
    return wl;
}

void ScrollView::MeasureSegments(size_t idx) {
    auto& segs = segCache_[idx];
    segs.clear();
    const auto& block = *blocks_[idx];
    FontStyle style = StyleForType(block.type);
    bool rtl = block.rightToLeft;
    float lh = fonts_.LineHeight(style);

    if (block.text.empty()) {
        segs.push_back({"", 0, lh, true, false, style, {}, false});
        segValid_[idx] = true;
        segTextLen_[idx] = 0;
        return;
    }

    float spW = fonts_.SpaceWidth(style);
    const std::string& text = block.text;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();

        if (nl == pos) {
            segs.push_back({"", 0, lh, true, false, style, {}, false});
        } else {
            std::string hardLine = text.substr(pos, nl - pos);
            size_t wpos = 0;

            while (wpos < hardLine.size()) {
                size_t sp = hardLine.find(' ', wpos);
                std::string word;
                if (sp == std::string::npos) {
                    word = hardLine.substr(wpos);
                    wpos = hardLine.size();
                } else {
                    word = hardLine.substr(wpos, sp - wpos);
                    wpos = sp;
                }

                if (!word.empty()) {
                    float w = fonts_.MeasureWidth(word, style, rtl);
                    segs.push_back({word, w, lh, false, false, style, {}, false});
                }

                if (wpos < hardLine.size() && hardLine[wpos] == ' ') {
                    segs.push_back({" ", spW, lh, false, true, style, {}, false});
                    wpos++;
                }
            }
        }

        if (nl < text.size()) {
            segs.push_back({"", 0, lh, true, false, style, {}, false});
        }
        pos = nl + 1;
        if (nl == text.size()) break;
    }

    segValid_[idx] = true;
    segTextLen_[idx] = text.size();
}

void ScrollView::MeasureStyledSegments(size_t idx) {
    auto& segs = segCache_[idx];
    segs.clear();
    const auto& block = *blocks_[idx];
    bool rtl = block.rightToLeft;

    for (const auto& run : block.runs) {
        FontStyle style = run.style;
        Color color = run.color;
        float lh = fonts_.LineHeight(style);
        float spW = fonts_.SpaceWidth(style);
        const std::string& text = run.text;
        size_t pos = 0;

        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();

            if (nl == pos && pos < text.size()) {
                segs.push_back({"", 0, lh, true, false, style, color, true});
            } else if (nl > pos) {
                std::string hardLine = text.substr(pos, nl - pos);
                size_t wpos = 0;

                while (wpos < hardLine.size()) {
                    size_t sp = hardLine.find(' ', wpos);
                    std::string word;
                    if (sp == std::string::npos) {
                        word = hardLine.substr(wpos);
                        wpos = hardLine.size();
                    } else {
                        word = hardLine.substr(wpos, sp - wpos);
                        wpos = sp;
                    }

                    if (!word.empty()) {
                        float w = fonts_.MeasureWidth(word, style, rtl);
                        segs.push_back({word, w, lh, false, false, style, color, true});
                    }

                    if (wpos < hardLine.size() && hardLine[wpos] == ' ') {
                        segs.push_back({" ", spW, lh, false, true, style, color, true});
                        wpos++;
                    }
                }
            }

            if (nl < text.size()) {
                segs.push_back({"", 0, lh, true, false, style, color, true});
            }
            pos = nl + 1;
            if (nl == text.size()) break;
        }
    }

    segValid_[idx] = true;
    segTextLen_[idx] = block.GetFullText().size();
}

void ScrollView::LayoutFromSegments(size_t idx, float textAreaW, float clientW,
                                     std::vector<WrappedLine>& out, float& outH) {
    out.clear();
    const auto& block = *blocks_[idx];
    const auto& segs = segCache_[idx];
    FontStyle blockStyle = StyleForType(block.type);
    bool rtl = block.rightToLeft;
    float indent = (float)block.leftIndent;

    float effectiveW = textAreaW - indent;
    if (effectiveW < 50) effectiveW = 50;
    // Paragraph text gets a tiny bit of extra leading to match wx's
    // GetTextExtent-derived line height. Code/thinking/tool blocks render in
    // dense monospace and keep their native metrics.
    float paraGap = (blockStyle == FontStyle::Regular) ? paraLineGap_ : 0.0f;
    std::string lineText;
    float lineW = 0, lineH = 0;
    // Track styled runs being built for current line
    std::vector<StyledTextRun> lineRuns;
    std::vector<float> lineRunOffsets;
    StyledTextRun currentRun;
    bool hasCurrentRun = false;

    auto flushLine = [&]() {
        // Detect RTL per line so English lines in Arabic blocks aren't reversed
        bool lineRtl = !lineText.empty() ? DetectRTL(lineText) : rtl;

        if (lineText.empty() && lineRuns.empty() && !hasCurrentRun) {
            float h = (segs.empty() ? fonts_.LineHeight(blockStyle) : segs[0].height) + paraGap;
            WrappedLine wl = MakeLine("", lineRtl, 0, h, leftMargin_, clientW, (int)indent);
            wl.y = outH;
            out.push_back(std::move(wl));
            outH += h;
            return;
        }

        // Flush current run
        if (hasCurrentRun && !currentRun.text.empty()) {
            lineRunOffsets.push_back(lineW - fonts_.MeasureWidth(currentRun.text, currentRun.style, lineRtl));
            // Actually we need the offset before adding, let me fix
        }

        float effLh = (lineH > 0 ? lineH : fonts_.LineHeight(blockStyle)) + paraGap;
        WrappedLine wl = MakeLine(lineText, lineRtl, lineW, effLh,
                                   leftMargin_, clientW, (int)indent);
        wl.y = outH;

        // Build styled runs for this line if we have styled segments
        if (!lineRuns.empty() || hasCurrentRun) {
            if (hasCurrentRun && !currentRun.text.empty()) {
                lineRuns.push_back(currentRun);
            }
            // Recompute offsets by measuring each run
            wl.styledRuns = lineRuns;
            wl.runXOffsets.clear();
            float xOff = 0;
            for (auto& r : wl.styledRuns) {
                wl.runXOffsets.push_back(xOff);
                xOff += fonts_.MeasureWidth(r.text, r.style, lineRtl);
            }
        }

        out.push_back(std::move(wl));
        outH += effLh;
        lineText.clear();
        lineW = 0;
        lineH = 0;
        lineRuns.clear();
        lineRunOffsets.clear();
        currentRun = {};
        hasCurrentRun = false;
    };

    bool anyStyled = false;
    for (auto& s : segs) {
        if (s.hasStyle) { anyStyled = true; break; }
    }

    for (size_t si = 0; si < segs.size(); si++) {
        const auto& seg = segs[si];

        if (seg.isNewline) {
            flushLine();
            continue;
        }

        if (seg.isSpace) {
            if (lineW + seg.width <= effectiveW || lineText.empty()) {
                lineText += seg.text;
                lineW += seg.width;
                if (seg.height > lineH) lineH = seg.height;
                if (anyStyled) {
                    if (hasCurrentRun && currentRun.style == seg.style) {
                        currentRun.text += seg.text;
                    } else {
                        if (hasCurrentRun && !currentRun.text.empty())
                            lineRuns.push_back(currentRun);
                        currentRun = {seg.text, seg.style, seg.color};
                        hasCurrentRun = true;
                    }
                }
            }
            continue;
        }

        // Word segment
        if (lineW + seg.width > effectiveW && !lineText.empty()) {
            // Wrap: start new line
            flushLine();
        }

        lineText += seg.text;
        lineW += seg.width;
        if (seg.height > lineH) lineH = seg.height;

        if (anyStyled) {
            if (hasCurrentRun && currentRun.style == seg.style &&
                currentRun.color.r == seg.color.r && currentRun.color.g == seg.color.g &&
                currentRun.color.b == seg.color.b) {
                currentRun.text += seg.text;
            } else {
                if (hasCurrentRun && !currentRun.text.empty())
                    lineRuns.push_back(currentRun);
                currentRun = {seg.text, seg.style, seg.color};
                hasCurrentRun = true;
            }
        }
    }

    // Flush remaining
    if (!lineText.empty() || lineRuns.size() > 0 || hasCurrentRun) {
        flushLine();
    }

    // If no lines produced, add empty line
    if (out.empty()) {
        float h = fonts_.LineHeight(blockStyle);
        WrappedLine wl = MakeLine("", rtl, 0, h, leftMargin_, clientW, (int)indent);
        wl.y = 0;
        out.push_back(std::move(wl));
        outH = h;
    }
}

// --- Table layout ----------------------------------------------------------
//
// Lays out a BlockType::TABLE block into WrappedLines that the existing paint
// path can draw. Each row expands to N wrapped lines where N = max line count
// among its cells after wrapping to the cell's column width. Each resulting
// WrappedLine's `.text` is the tab-joined cell portions on that visual line,
// `styledRuns` is the concatenation of cells' runs on that line (no separator
// runs — the tab characters live only in `.text`), `runXOffsets` carries the
// per-cell column X so cells land in the right column, and `caretX` is
// precomputed so hit-testing jumps cleanly across the gap between cells.
//
// Copy-to-clipboard falls out for free: the `.text` already contains tabs,
// which is what selection extraction emits.

namespace {

// One run, sliced down to a portion that fits on one visual line of a cell.
struct CellChunk {
    std::string text;      // word or whitespace run
    float width;           // measured width
    FontStyle style;
    Color color;
    bool isSpace;          // whitespace chunk (dropped at start of new cell line)
};

// Split a single StyledTextRun into word/space chunks, measuring each word's
// width in its own style. Hard newlines inside cell runs are treated like a
// space (cells don't support multi-line runs in v1).
void ChunkCellRun(const StyledTextRun& run, FontManager& fonts, std::vector<CellChunk>& out) {
    const std::string& text = run.text;
    if (text.empty()) return;
    float spW = fonts.SpaceWidth(run.style);
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            out.push_back({" ", spW, run.style, run.color, true});
            i++;
            continue;
        }
        size_t j = i;
        while (j < text.size()) {
            char cc = text[j];
            if (cc == ' ' || cc == '\t' || cc == '\n' || cc == '\r') break;
            j++;
        }
        std::string word = text.substr(i, j - i);
        float w = fonts.MeasureWidth(word, run.style, false);
        out.push_back({word, w, run.style, run.color, false});
        i = j;
    }
}

// Greedy word-wrap a cell's chunk list into visual lines. Each output line is
// a list of styled runs (adjacent same-style chunks get merged). Returns the
// max line width actually used.
struct CellVisualLine {
    std::vector<StyledTextRun> runs;
    float width = 0;
};

float WrapCellChunks(const std::vector<CellChunk>& chunks, float maxW,
                     FontManager& fonts, std::vector<CellVisualLine>& out) {
    out.clear();
    if (chunks.empty()) return 0;

    auto pushRun = [](CellVisualLine& line, const CellChunk& ch) {
        if (!line.runs.empty() && line.runs.back().style == ch.style &&
            line.runs.back().color.r == ch.color.r &&
            line.runs.back().color.g == ch.color.g &&
            line.runs.back().color.b == ch.color.b) {
            line.runs.back().text += ch.text;
        } else {
            line.runs.push_back({ch.text, ch.style, ch.color});
        }
        line.width += ch.width;
    };

    CellVisualLine current;
    bool lineEmpty = true;
    float maxUsed = 0;
    for (const auto& ch : chunks) {
        if (ch.isSpace) {
            if (lineEmpty) continue;  // drop leading whitespace
            // Tentatively keep the space; a following word may force a wrap.
            if (current.width + ch.width > maxW) {
                // Wrap here, dropping the trailing space.
                if (current.width > maxUsed) maxUsed = current.width;
                out.push_back(std::move(current));
                current = {};
                lineEmpty = true;
                continue;
            }
            pushRun(current, ch);
            continue;
        }

        // Word chunk. If it doesn't fit and line isn't empty, wrap first.
        // A word wider than the column overflows rather than getting chopped
        // mid-word — keeps code identifiers and URLs readable.
        if (!lineEmpty && current.width + ch.width > maxW) {
            if (current.width > maxUsed) maxUsed = current.width;
            out.push_back(std::move(current));
            current = {};
            lineEmpty = true;
        }
        pushRun(current, ch);
        lineEmpty = false;

        // If a single word alone already fills the column, force-wrap after it
        // so subsequent words start on a fresh line.
        if (current.width >= maxW) {
            if (current.width > maxUsed) maxUsed = current.width;
            out.push_back(std::move(current));
            current = {};
            lineEmpty = true;
        }
    }
    if (!lineEmpty || out.empty()) {
        // Strip any trailing pure-whitespace run to keep widths tight.
        while (!current.runs.empty()) {
            auto& last = current.runs.back();
            bool allSpace = true;
            for (char c : last.text) if (c != ' ' && c != '\t') { allSpace = false; break; }
            if (!allSpace) break;
            current.width -= fonts.MeasureWidth(last.text, last.style, false);
            current.runs.pop_back();
        }
        if (current.width > maxUsed) maxUsed = current.width;
        out.push_back(std::move(current));
    }
    return maxUsed;
}

// Measure the max-content width (full unwrapped) and min-content width
// (widest single word) of a cell. Both need to be known to do reasonable
// column fitting under a tight width budget.
void MeasureCellContent(const std::vector<CellChunk>& chunks,
                        float& minContentOut, float& maxContentOut) {
    float maxContent = 0;
    float minContent = 0;
    for (const auto& ch : chunks) {
        maxContent += ch.width;
        if (!ch.isSpace && ch.width > minContent) minContent = ch.width;
    }
    minContentOut = minContent;
    maxContentOut = maxContent;
}

}  // namespace

void ScrollView::LayoutTable(size_t idx, float textAreaW, float,
                              std::vector<WrappedLine>& out, float& outH) {
    out.clear();
    outH = 0;

    auto& block = *blocks_[idx];
    int numCols = block.tableCols;
    if (numCols <= 0 || block.tableRows.empty()) return;

    // Table padding + chrome sizing — matches wx-branch (kTableCellPadX/Y).
    // Applied once per row (above the first visual line, below the last), not
    // per visual line, so multi-line cells stay compact and the row height
    // grows by exactly one lineH per wrap.
    const float hPad = tableHPad_;     // horizontal padding inside each cell (scaled)
    const float vPad = tableVPad_;     // vertical padding above/below cell content (scaled)

    FontStyle bodyStyle = FontStyle::Regular;
    float lineH = fonts_.LineHeight(bodyStyle);

    // Step 1: chunk every cell so we can measure and wrap against a column
    // width without re-parsing runs.
    std::vector<std::vector<std::vector<CellChunk>>> chunkGrid(block.tableRows.size());
    for (size_t r = 0; r < block.tableRows.size(); r++) {
        auto& row = block.tableRows[r];
        chunkGrid[r].resize(numCols);
        for (int c = 0; c < numCols && c < (int)row.size(); c++) {
            for (auto& run : row[c].runs) {
                StyledTextRun runCopy = run;
                if (r == 0) {
                    runCopy.style = (runCopy.style == FontStyle::Italic ||
                                     runCopy.style == FontStyle::BoldItalic)
                                      ? FontStyle::BoldItalic : FontStyle::Bold;
                }
                ChunkCellRun(runCopy, fonts_, chunkGrid[r][c]);
            }
        }
    }

    // Step 2: per-column min/max content widths.
    std::vector<float> colMin(numCols, 0), colMax(numCols, 0);
    for (int c = 0; c < numCols; c++) {
        for (size_t r = 0; r < chunkGrid.size(); r++) {
            float mn = 0, mx = 0;
            MeasureCellContent(chunkGrid[r][c], mn, mx);
            if (mn > colMin[c]) colMin[c] = mn;
            if (mx > colMax[c]) colMax[c] = mx;
        }
    }

    // Step 3: fit columns into textAreaW. Budget = textAreaW minus all cell
    // padding (2 * hPad per column).
    float paddingTotal = numCols * hPad * 2;
    float budget = textAreaW - paddingTotal;
    if (budget < numCols * 20) budget = (float)(numCols * 20);  // floor

    std::vector<float> colW(numCols, 0);
    float maxSum = 0;
    for (int c = 0; c < numCols; c++) maxSum += colMax[c];

    if (maxSum <= budget) {
        // Everything fits — start at max-content and distribute the slack
        // proportionally to each column's max-content width so the table
        // always spans the full content column (wx-branch behavior). Falls
        // back to even distribution if every column is empty.
        for (int c = 0; c < numCols; c++) colW[c] = colMax[c];
        float slack = budget - maxSum;
        if (slack > 0.5f) {
            if (maxSum > 0.5f) {
                for (int c = 0; c < numCols; c++) colW[c] += slack * (colMax[c] / maxSum);
            } else {
                float each = slack / (float)numCols;
                for (int c = 0; c < numCols; c++) colW[c] += each;
            }
        }
    } else {
        // Need to shrink. Start with max-content, then proportionally shrink
        // columns whose width is above their min-content, pulling down until
        // the sum fits. A couple of passes handle most cases cleanly.
        for (int c = 0; c < numCols; c++) colW[c] = colMax[c];
        for (int pass = 0; pass < 4; pass++) {
            float curSum = 0;
            for (int c = 0; c < numCols; c++) curSum += colW[c];
            if (curSum <= budget + 0.5f) break;
            float shrinkable = 0;
            for (int c = 0; c < numCols; c++) {
                if (colW[c] > colMin[c]) shrinkable += (colW[c] - colMin[c]);
            }
            if (shrinkable <= 0.5f) break;
            float excess = curSum - budget;
            float factor = std::min(1.0f, excess / shrinkable);
            for (int c = 0; c < numCols; c++) {
                float slack = colW[c] - colMin[c];
                if (slack > 0) colW[c] -= slack * factor;
            }
        }
        // Final clamp to the budget in case min-content still overflows.
        float curSum = 0;
        for (int c = 0; c < numCols; c++) curSum += colW[c];
        if (curSum > budget) {
            float scale = budget / curSum;
            for (int c = 0; c < numCols; c++) colW[c] *= scale;
        }
    }

    // Step 4: column X positions (relative to the line's left edge).
    std::vector<float> colX(numCols, 0);
    float xAccum = 0;
    for (int c = 0; c < numCols; c++) {
        colX[c] = xAccum;
        xAccum += colW[c] + hPad * 2;
    }
    float totalW = xAccum;

    // Step 5: per-row wrap and visual line emission.
    if (idx >= tableLayoutCache_.size()) tableLayoutCache_.resize(idx + 1);
    TableLayoutInfo& info = tableLayoutCache_[idx];
    info = {};
    info.colX = colX;
    info.colW = colW;
    info.hPad = hPad;
    info.vPad = vPad;
    info.totalW = totalW;

    for (size_t r = 0; r < block.tableRows.size(); r++) {
        // Wrap every cell independently to its column width.
        std::vector<std::vector<CellVisualLine>> cellLines(numCols);
        size_t rowVisLines = 1;
        for (int c = 0; c < numCols; c++) {
            WrapCellChunks(chunkGrid[r][c], colW[c], fonts_, cellLines[c]);
            if (cellLines[c].size() > rowVisLines) rowVisLines = cellLines[c].size();
        }

        // Top padding for this row.
        outH += vPad;

        for (size_t v = 0; v < rowVisLines; v++) {
            WrappedLine wl;
            wl.rightToLeft = false;
            wl.x = leftMargin_;
            wl.y = outH;
            wl.width = totalW;
            wl.height = lineH;
            wl.tableRow = (int)r;

            std::string flatText;
            std::vector<StyledTextRun> flatRuns;
            std::vector<float> flatOffsets;
            std::vector<int> runStarts;
            std::vector<float> caretX;
            caretX.push_back(0.0f);
            int charAccum = 0;

            for (int c = 0; c < numCols; c++) {
                float cellLeft = colX[c] + hPad;

                // Does this cell have content on visual line v?
                if (v < cellLines[c].size()) {
                    auto& cvl = cellLines[c][v];
                    // Alignment offset: left=0, center=slack/2, right=slack.
                    float slack = colW[c] - cvl.width;
                    if (slack < 0) slack = 0;
                    int al = (c < (int)block.tableAlign.size()) ? block.tableAlign[c] : -1;
                    float alignOffset = (al == 0) ? slack / 2 : (al == 1 ? slack : 0);
                    float cellRunX = cellLeft + alignOffset;

                    float cellCumX = 0;
                    for (auto& run : cvl.runs) {
                        runStarts.push_back(charAccum);
                        flatRuns.push_back(run);
                        flatOffsets.push_back(cellRunX + cellCumX);

                        auto positions = fonts_.CaretPositions(run.text, run.style, false);
                        int rc = utf8_codepoint_count(run.text);
                        for (int i = 1; i <= rc; i++) {
                            caretX.push_back(cellRunX + cellCumX + positions[i]);
                        }
                        if (!positions.empty()) cellCumX += positions.back();
                        flatText += run.text;
                        charAccum += rc;
                    }
                }

                // Tab separator between cells (not after the last column).
                // Lives in .text so selection copy gets cell separators for
                // free; no styled run, so nothing draws at the gap.
                if (c + 1 < numCols) {
                    flatText += '\t';
                    // Caret position after the tab lands at the next cell's left edge.
                    caretX.push_back(colX[c + 1] + hPad);
                    charAccum += 1;
                }
            }

            wl.text = std::move(flatText);
            wl.styledRuns = std::move(flatRuns);
            wl.runXOffsets = std::move(flatOffsets);
            wl.runCharStarts = std::move(runStarts);
            wl.caretX = std::move(caretX);
            wl.caretXValid = true;

            out.push_back(std::move(wl));
            outH += lineH;
        }

        // Bottom padding for this row.
        outH += vPad;

        // Remember where the header rule should sit.
        if (r == 0) info.headerBottomY = outH;

        info.rowBottomY.push_back(outH);
    }
}

// Per-block chrome offsets — describes where the text should land relative
// to the baseline wl.x/wl.y, and what the rounded shape behind it spans.
// Computed lazily off wrappedCache_ contents; cheap O(lines) for USER_PROMPT
// where we need the longest line, O(1) for everything else.
ScrollView::BlockChrome ScrollView::ChromeFor(size_t i) const {
    BlockChrome ch;
    if (i >= blocks_.size() || i >= wrappedCache_.size()) return ch;
    const auto& block = *blocks_[i];
    const auto& lines = wrappedCache_[i];

    if (block.type == BlockType::USER_PROMPT) {
        float maxW = 0;
        for (const auto& wl : lines) if (wl.width > maxW) maxW = wl.width;
        float bubbleW = maxW + userBubblePad_ * 2;
        if (bubbleW > contentW_) bubbleW = contentW_;
        if (bubbleW < userBubblePad_ * 2 + 4) bubbleW = userBubblePad_ * 2 + 4;
        ch.bubbleW = bubbleW;
        ch.bubbleX = leftMargin_ + contentW_ - bubbleW;
        ch.xOff = ch.bubbleX + userBubblePad_ - leftMargin_;
        ch.yOff = userBubblePad_;
    } else if (block.type == BlockType::CODE) {
        ch.bubbleX = leftMargin_;
        ch.bubbleW = contentW_;
        ch.xOff = codeHPad_;
        ch.yOff = codeVPad_;
    } else if (block.type == BlockType::TOOL_CALL) {
        ch.bubbleX = leftMargin_;
        ch.bubbleW = contentW_;
        ch.headerH = fonts_.LineHeight(FontStyle::Code) + toolPadY_ * 2;
        if (!block.isCollapsed && !lines.empty()) {
            float h = toolPadY_ * 2;
            for (const auto& wl : lines) h += wl.height;
            ch.bodyH = h;
        }
        ch.xOff = toolPadX_;
        // Body text sits inside the body region, which begins at headerH (no
        // gap — header and body are one continuous bubble). chrome.yOff adds
        // toolPadY_ for the body's inner top pad.
        ch.yOff = ch.headerH + toolPadY_;
    } else if (block.type == BlockType::THINKING) {
        float thinkingLh = fonts_.LineHeight(FontStyle::ThinkingItalic);
        ch.bubbleX = leftMargin_;
        ch.bubbleW = contentW_;
        ch.headerH = thinkingLh + toolPadY_ * 2;
        if (block.isExpandable && !block.isCollapsed && lines.size() > 1) {
            // Single continuous bubble: header strip on top, body region
            // below — both painted on the same rounded rect with a tint
            // change at headerH. Body extends from headerH down through the
            // last wrapped line's bottom plus toolPadY_ bottom pad; loading
            // adds one extra line for the animated dots.
            auto& last = lines.back();
            ch.bodyH = last.y + last.height + toolPadY_ - ch.headerH;
            if (block.isLoading) ch.bodyH += thinkingLh;
        }
        ch.xOff = toolPadX_;
        ch.yOff = toolPadY_;
    }
    return ch;
}

// Lays out the body text of an expanded TOOL_CALL block as monospace lines
// wrapped at bodyTextW. Lines are appended to wrappedCache_[idx] starting at
// y=0 (paint shifts them by the header offset). Each line carries a single
// styled run in code style so the existing per-glyph paint path handles them.
void ScrollView::LayoutToolCallBody(size_t idx, float bodyTextW) {
    auto& block = *blocks_[idx];
    auto& lines = wrappedCache_[idx];
    lines.clear();

    std::string body;
    if (!block.toolArgs.empty()) body += "args: " + block.toolArgs + "\n\n";
    body += block.toolResult;
    if (body.empty()) return;

    FontStyle style = FontStyle::Code;
    float lh = fonts_.LineHeight(style);
    if (bodyTextW < 20) bodyTextW = 20;

    float y = 0;
    auto emitLine = [&](const std::string& seg) {
        WrappedLine wl;
        wl.text = seg;
        wl.rightToLeft = false;
        wl.x = leftMargin_;
        wl.y = y;
        wl.width = fonts_.MeasureWidth(seg, style, false);
        wl.height = lh;
        if (!seg.empty()) {
            wl.styledRuns.push_back({seg, style, codeColor_});
            wl.runXOffsets.push_back(0);
        }
        lines.push_back(std::move(wl));
        y += lh;
    };

    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        std::string srcLine = (nl == std::string::npos) ? body.substr(pos)
                                                        : body.substr(pos, nl - pos);
        if (srcLine.empty()) {
            emitLine("");
        } else {
            // Greedy char-level wrap. Soft-break preference: backtrack to the
            // last space in (start, end]. Avoids spilling long paths/URLs
            // past the body's clipped rounded box.
            size_t start = 0;
            while (start < srcLine.size()) {
                size_t end = start;
                float w = 0;
                size_t lastSpace = std::string::npos;
                while (end < srcLine.size()) {
                    size_t stride = utf8_stride(srcLine, end);
                    std::string chs = srcLine.substr(end, stride);
                    float cw = fonts_.MeasureWidth(chs, style, false);
                    if (w + cw > bodyTextW && end > start) break;
                    if (srcLine[end] == ' ') lastSpace = end;
                    w += cw;
                    end += stride;
                }
                size_t breakAt = end;
                if (end < srcLine.size() && lastSpace != std::string::npos &&
                    lastSpace + 1 > start) {
                    breakAt = lastSpace + 1;
                }
                emitLine(srcLine.substr(start, breakAt - start));
                start = breakAt;
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

void ScrollView::RebuildSingleBlock(size_t i, float textAreaW, float clientW) {
    auto& block = *blocks_[i];
    float h = 0;

    // Bubbled / boxed blocks lay out their text inside an inset, so the
    // effective wrap width is the column width minus 2x the inner pad. The
    // padding itself is added back into blockHeightCache_ at the bottom of
    // this function so the cached height matches the painted box.
    float effectiveW = textAreaW;
    if (block.type == BlockType::USER_PROMPT) effectiveW = textAreaW - userBubblePad_ * 2;
    else if (block.type == BlockType::CODE) effectiveW = textAreaW - codeHPad_ * 2;
    else if (block.type == BlockType::TOOL_CALL) effectiveW = textAreaW - toolPadX_ * 2;
    else if (block.type == BlockType::THINKING) effectiveW = textAreaW - toolPadX_ * 2;
    if (effectiveW < 50) effectiveW = 50;

    if (block.type == BlockType::TOOL_CALL) {
        float headerH = fonts_.LineHeight(FontStyle::Code) + toolPadY_ * 2;
        if (block.isCollapsed) {
            wrappedCache_[i].clear();
            shapedCache_[i].clear();
            segValid_[i] = true;
            blockHeightCache_[i] = headerH;
            charHeightCache_[i] = fonts_.LineHeight(FontStyle::Code);
            return;
        }
        LayoutToolCallBody(i, effectiveW);
        shapedCache_[i].clear();
        segValid_[i] = true;
        // Expanded but nothing to show in the body (e.g. clicked before the
        // tool result arrived) — just render the header strip alone.
        if (wrappedCache_[i].empty()) {
            blockHeightCache_[i] = headerH;
            charHeightCache_[i] = fonts_.LineHeight(FontStyle::Code);
            return;
        }
        float bodyH = toolPadY_ * 2;
        for (auto& wl : wrappedCache_[i]) bodyH += wl.height;
        blockHeightCache_[i] = headerH + bodyH;
        charHeightCache_[i] = fonts_.LineHeight(FontStyle::Code);
        return;
    }

    if (block.type == BlockType::TABLE) {
        if (i >= tableLayoutCache_.size()) tableLayoutCache_.resize(i + 1);
        LayoutTable(i, textAreaW, clientW, wrappedCache_[i], h);
        shapedCache_[i].clear();
        segValid_[i] = true;
        blockHeightCache_[i] = h;
        charHeightCache_[i] = fonts_.LineHeight(FontStyle::Regular);
        return;
    }
    if (!segValid_[i]) {
        if (block.HasStyledRuns())
            MeasureStyledSegments(i);
        else
            MeasureSegments(i);
    }

    LayoutFromSegments(i, effectiveW, clientW, wrappedCache_[i], h);
    shapedCache_[i].clear();

    if (block.type == BlockType::THINKING) {
        // THINKING renders as a tool-style card: rounded header strip with a
        // chevron + peek line, plus a body box of wrapped content when
        // expanded. Italic style and thinking bg colors are kept so it still
        // reads as a "thought" surface, not a tool call.
        float thinkingLh = fonts_.LineHeight(FontStyle::ThinkingItalic);
        float headerH = thinkingLh + toolPadY_ * 2;

        // A block is only worth expanding if doing so reveals non-whitespace
        // content. A trailing "\n" in block.text or an expandText of just
        // whitespace would otherwise make the chevron appear but the expanded
        // body show only a blank row.
        auto hasNonWs = [](const std::string& s) {
            for (unsigned char c : s) if (!std::isspace(c)) return true;
            return false;
        };
        bool bodyHasContent = hasNonWs(block.expandText);
        if (!bodyHasContent) {
            for (size_t k = 1; k < wrappedCache_[i].size(); k++) {
                if (hasNonWs(wrappedCache_[i][k].text)) { bodyHasContent = true; break; }
            }
        }
        block.isExpandable = bodyHasContent;
        float chevronW = block.isExpandable
                         ? fonts_.MeasureWidth("\xe2\x96\xb8 ", FontStyle::ThinkingItalic) : 0;
        // Note: don't force isCollapsed=false for non-expandable blocks here.
        // During streaming a THINKING block starts as one wrapped line
        // (isExpandable=false) and grows as more text arrives — flipping
        // isCollapsed to false at that moment leaves it permanently expanded
        // once it becomes multi-line. The constructor already initializes
        // THINKING blocks as collapsed; trust that and only toggle on user
        // clicks. Single-line blocks render identically in either state.

        if (!block.isExpandable) {
            // Single-line system message ("Connecting...", "Switched to x").
            // Header strip only — chevron-less, plain italic on thinking bg.
            wrappedCache_[i].resize(1);
            shapedCache_[i].clear();
            blockHeightCache_[i] = headerH;
            charHeightCache_[i] = thinkingLh;
            return;
        }

        // Expandable. Shift line 0 right by chevron width so the chevron
        // glyph fits before the peek text inside the header.
        auto bumpForChevron = [&](size_t li) {
            if (li < wrappedCache_[i].size()) wrappedCache_[i][li].x += chevronW;
        };

        if (block.isCollapsed) {
            // Keep only the first wrapped line as the header peek. If we're
            // loading, also leave room for the animated dots at line's end.
            wrappedCache_[i].resize(1);
            auto& wl = wrappedCache_[i][0];
            if (block.isLoading) {
                float dotSpace = thinkingLh * 2.5f;
                float maxTextW = effectiveW - chevronW - dotSpace - toolPadX_;
                if (maxTextW < 20) maxTextW = 20;
                if (wl.width > maxTextW && !wl.text.empty()) {
                    std::string truncated = wl.text;
                    while (!truncated.empty() &&
                           fonts_.MeasureWidth(truncated, FontStyle::ThinkingItalic) > maxTextW) {
                        size_t sp = truncated.rfind(' ');
                        if (sp == std::string::npos) { truncated.clear(); break; }
                        truncated = truncated.substr(0, sp);
                    }
                    if (!truncated.empty() && truncated.size() < wl.text.size())
                        truncated += "\xe2\x80\xa6";
                    wl.text = truncated;
                    wl.width = fonts_.MeasureWidth(truncated, FontStyle::ThinkingItalic);
                    wl.shapedValid = false;
                }
            }
            bumpForChevron(0);
            shapedCache_[i].clear();
            blockHeightCache_[i] = headerH;
            charHeightCache_[i] = thinkingLh;
            return;
        }

        // Expanded. If there's an expandText (e.g. raw JSON detail), splice it
        // onto the visible text and re-wrap so the body box shows everything.
        if (!block.expandText.empty()) {
            std::string fullText = block.text + "\n\n" + block.expandText;
            std::string origText = block.text;
            block.text = fullText;
            segValid_[i] = false;
            MeasureSegments(i);
            block.text = origText;
            LayoutFromSegments(i, effectiveW, clientW, wrappedCache_[i], h);
            shapedCache_[i].clear();
        }

        // The header peek is line 0; the body holds lines 1..N. Shift the body
        // lines' y so they land just below the header strip — no gap, the
        // header/body share one continuous bubble. Paint adds chrome.yOff (=
        // toolPadY_) on top of wl.y, so wl.y[1] needs to be exactly headerH.
        if (wrappedCache_[i].size() >= 2) {
            float desiredY1 = headerH;
            float delta = desiredY1 - wrappedCache_[i][1].y;
            for (size_t li = 1; li < wrappedCache_[i].size(); li++) {
                wrappedCache_[i][li].y += delta;
            }
        }
        bumpForChevron(0);

        float totalH;
        if (wrappedCache_[i].size() <= 1) {
            totalH = headerH;
        } else {
            auto& last = wrappedCache_[i].back();
            // last.y was shifted into body coordinate space (starts at headerH);
            // paint adds chrome.yOff (toolPadY_) for the body's inner top pad.
            // The trailing toolPadY_ here is the body's inner BOTTOM pad.
            totalH = last.y + last.height + toolPadY_;
        }
        if (block.isLoading) totalH += thinkingLh;
        blockHeightCache_[i] = totalH;
        charHeightCache_[i] = thinkingLh;
        return;
    }

    float ch = fonts_.LineHeight(StyleForType(block.type));
    if (block.type == BlockType::USER_PROMPT) h += userBubblePad_ * 2;
    else if (block.type == BlockType::CODE) h += codeVPad_ * 2;
    blockHeightCache_[i] = h;
    charHeightCache_[i] = ch;
}

void ScrollView::RebuildBlockTopCache() {
    size_t n = blockHeightCache_.size();
    blockTopCache_.resize(n + 1);
    float y = topMargin_;
    for (size_t i = 0; i < n; i++) {
        blockTopCache_[i] = y;
        y += blockHeightCache_[i] + blockSpacing_;
    }
    blockTopCache_[n] = y;
    cachedTotalH_ = y + topMargin_;
}

void ScrollView::UpdateBlockTopCacheTail(size_t from) {
    if (blockTopCache_.size() < from + 1) blockTopCache_.resize(from + 1);

    float y = from > 0 ? blockTopCache_[from - 1] + blockHeightCache_[from - 1] + blockSpacing_
                        : topMargin_;
    for (size_t i = from; i < blockHeightCache_.size(); i++) {
        if (i >= blockTopCache_.size()) blockTopCache_.push_back(0);
        blockTopCache_[i] = y;
        y += blockHeightCache_[i] + blockSpacing_;
    }
    if (blockTopCache_.size() <= blockHeightCache_.size())
        blockTopCache_.push_back(y);
    else
        blockTopCache_[blockHeightCache_.size()] = y;
    cachedTotalH_ = y + topMargin_;
}

size_t ScrollView::FindFirstVisible(float scrollPos) const {
    if (blockTopCache_.size() <= 1) return 0;
    auto it = std::upper_bound(blockTopCache_.begin(), blockTopCache_.end(), scrollPos);
    if (it == blockTopCache_.begin()) return 0;
    --it;
    size_t idx = it - blockTopCache_.begin();
    return idx < blocks_.size() ? idx : 0;
}

// --- Selection ---

bool ScrollView::HasSelection() const {
    return selAnchor_.IsValid() && selCaret_.IsValid() && selAnchor_ != selCaret_;
}

void ScrollView::SelectAll() {
    if (blocks_.empty() || wrappedCache_.empty()) return;
    TextPosition newAnchor = {0, 0, 0};
    TextPosition newCaret = newAnchor;
    // Walk backwards to anchor the caret on the last block with wrapped lines.
    // Collapsed TOOL_CALL blocks have an empty wrappedCache_ slot — if the
    // conversation ends with one, the original `lastB`-only path would leave
    // caret == anchor and HasSelection() would return false.
    for (int bi = (int)blocks_.size() - 1; bi >= 0; bi--) {
        if (bi >= (int)wrappedCache_.size() || wrappedCache_[bi].empty()) continue;
        auto& lines = wrappedCache_[bi];
        int lastL = (int)lines.size() - 1;
        newCaret = {bi, lastL, utf8_codepoint_count(lines[lastL].text)};
        break;
    }
    if (newAnchor == newCaret) return;
    if (newAnchor != selAnchor_ || newCaret != selCaret_) needsRedraw_ = true;
    selAnchor_ = newAnchor;
    selCaret_ = newCaret;
}

void ScrollView::ClearSelection() {
    if (HasSelection()) needsRedraw_ = true;
    selAnchor_ = {};
    selCaret_ = {};
    selecting_ = false;
    clickCount_ = 0;
}

void ScrollView::GetOrderedSel(TextPosition& s, TextPosition& e) const {
    if (selAnchor_ <= selCaret_) { s = selAnchor_; e = selCaret_; }
    else { s = selCaret_; e = selAnchor_; }
}

std::string ScrollView::TextBetween(const TextPosition& start, const TextPosition& end) const {
    if (!start.IsValid() || !end.IsValid()) return "";
    std::string result;
    for (int bi = start.block; bi <= end.block && bi < (int)wrappedCache_.size(); bi++) {
        auto& lines = wrappedCache_[bi];
        int firstL = (bi == start.block) ? start.line : 0;
        int lastL = (bi == end.block) ? end.line : (int)lines.size() - 1;

        // Tool calls: emit a canonical "name args\nresult" representation so
        // a cross-block drag-select (or Ctrl+A) captures tool output even
        // when the block is collapsed and has no wrapped body lines. This
        // mirrors origin/main where tool output rendered as plain text and
        // was naturally part of any spanning selection. Expanded tool blocks
        // also funnel through here so the clipboard always carries the same
        // representation regardless of collapse state — partial selection
        // within an expanded body would otherwise drift from what the user
        // sees in the bubble.
        if (bi < (int)blocks_.size() && blocks_[bi]->type == BlockType::TOOL_CALL) {
            auto& block = *blocks_[bi];
            std::string body;
            if (!block.toolName.empty()) {
                body += block.toolName;
                if (!block.toolArgs.empty()) body += " " + block.toolArgs;
            }
            if (!block.toolResult.empty()) {
                if (!body.empty()) body += "\n";
                body += block.toolResult;
            }
            if (!body.empty()) result += body;
            if (bi < end.block) result += '\n';
            continue;
        }

        // Table blocks: rebuild rows from the source data rather than
        // reassembling the wrapped visual-line pieces. This avoids the
        // ugly "\n\t<continuation>" artefact that falls out of a cell that
        // wrapped across multiple visual lines, and matches the Excel /
        // Sheets clipboard convention (one row per line, tab between cells).
        if (bi < (int)blocks_.size() && blocks_[bi]->type == BlockType::TABLE) {
            auto& block = *blocks_[bi];
            int rowMin = -1, rowMax = -1;
            for (int li = firstL; li <= lastL && li < (int)lines.size(); li++) {
                int tr = lines[li].tableRow;
                if (tr < 0) continue;
                if (rowMin < 0 || tr < rowMin) rowMin = tr;
                if (tr > rowMax) rowMax = tr;
            }
            if (rowMax >= 0) {
                for (int r = rowMin; r <= rowMax && r < (int)block.tableRows.size(); r++) {
                    auto& row = block.tableRows[r];
                    for (size_t c = 0; c < row.size(); c++) {
                        if (c > 0) result += '\t';
                        for (auto& run : row[c].runs) result += run.text;
                    }
                    if (r < rowMax || bi < end.block) result += '\n';
                }
            }
            continue;
        }

        for (int li = firstL; li <= lastL && li < (int)lines.size(); li++) {
            auto& wl = lines[li];
            int charLen = utf8_codepoint_count(wl.text);
            int from = (bi == start.block && li == start.line) ? start.offset : 0;
            int to = (bi == end.block && li == end.line) ? end.offset : charLen;
            from = std::clamp(from, 0, charLen);
            to = std::clamp(to, 0, charLen);
            if (from < to) {
                size_t byteFrom = utf8_char_to_byte(wl.text, from);
                size_t byteTo = utf8_char_to_byte(wl.text, to);
                result += wl.text.substr(byteFrom, byteTo - byteFrom);
            }
            if (li < lastL || bi < end.block) result += '\n';
        }
    }
    return result;
}

std::string ScrollView::GetSelectedText() const {
    if (!HasSelection()) return "";
    TextPosition s, e;
    GetOrderedSel(s, e);
    return TextBetween(s, e);
}

// Ensure shaped runs are cached for a line within a block
void ScrollView::EnsureShapedCache(size_t bi, size_t li) const {
    if (bi >= wrappedCache_.size() || li >= wrappedCache_[bi].size()) return;
    auto& wl = wrappedCache_[bi][li];
    if (wl.shapedValid) return;

    // Grow shaped cache if needed
    if (bi >= shapedCache_.size())
        shapedCache_.resize(bi + 1);
    if (li >= shapedCache_[bi].size())
        shapedCache_[bi].resize(li + 1);

    auto& shapes = shapedCache_[bi][li];
    shapes.clear();

    bool rtl = wl.rightToLeft;
    if (!wl.styledRuns.empty()) {
        for (auto& run : wl.styledRuns)
            shapes.push_back(fonts_.Shape(run.text, run.style, rtl));
    } else if (!wl.text.empty()) {
        FontStyle style = StyleForType(blocks_[bi]->type);
        shapes.push_back(fonts_.Shape(wl.text, style, rtl));
    }
    wl.shapedValid = true;
}

void ScrollView::EnsureCaretX(WrappedLine& wl, FontStyle style, bool rtl) const {
    if (wl.caretXValid) return;

    if (!wl.styledRuns.empty()) {
        wl.caretX.clear();
        wl.caretX.push_back(0.0f);
        float xAccum = 0;
        for (auto& run : wl.styledRuns) {
            // CaretPositions will call Shape internally - but this path
            // only runs once per line (caretXValid persists until text changes)
            auto positions = fonts_.CaretPositions(run.text, run.style, false);
            int rc = utf8_codepoint_count(run.text);
            for (int i = 1; i <= rc; i++)
                wl.caretX.push_back(xAccum + positions[i]);
            if (rc > 0) xAccum += positions[rc];
        }
    } else {
        wl.caretX = fonts_.CaretPositions(wl.text, style, rtl);
    }
    wl.caretXValid = true;
}

float ScrollView::CaretXForOffset(const WrappedLine& wl, int off) const {
    if (off < 0) return 0;
    if (off >= (int)wl.caretX.size()) return wl.caretX.empty() ? 0 : wl.caretX.back();
    return wl.caretX[off];
}

TextPosition ScrollView::HitTest(float px, float py) const {
    float virtualY = py + scrollPos_;

    if (blockTopCache_.size() < 2) return {};
    size_t bi = FindFirstVisible(virtualY);
    size_t safeCount = std::min(blocks_.size(), wrappedCache_.size());
    if (safeCount == 0) return {};

    // Find exact block
    while (bi < safeCount - 1) {
        float blockTop = blockTopCache_[bi];
        float blockBot = blockTop + blockHeightCache_[bi];
        if (virtualY < blockBot) break;
        if (bi + 1 < blockTopCache_.size() && virtualY < blockTopCache_[bi + 1]) break;
        bi++;
    }

    if (bi >= wrappedCache_.size()) return {};
    auto& lines = wrappedCache_[bi];
    if (lines.empty()) return {(int)bi, 0, 0};

    float blockTop = blockTopCache_[bi];
    BlockChrome chrome = ChromeFor(bi);
    float localY = virtualY - blockTop - chrome.yOff;

    for (size_t li = 0; li < lines.size(); li++) {
        auto& wl = lines[li];
        float lineBot = wl.y + wl.height;

        if (localY < lineBot || li == lines.size() - 1) {
            if (wl.text.empty()) return {(int)bi, (int)li, 0};

            auto& mutWl = const_cast<WrappedLine&>(wl);
            FontStyle style = StyleForType(blocks_[bi]->type);
            const_cast<ScrollView*>(this)->EnsureCaretX(mutWl, style, blocks_[bi]->rightToLeft);

            float localX = px - wl.x - chrome.xOff;
            int len = utf8_codepoint_count(wl.text);

            if (!wl.rightToLeft) {
                if (localX <= 0) return {(int)bi, (int)li, 0};
                if (localX >= wl.width) return {(int)bi, (int)li, len};

                int lo = 0, hi = len + 1;
                while (lo < hi) {
                    int mid = (lo + hi) / 2;
                    if (wl.caretX[mid] <= localX) lo = mid + 1;
                    else hi = mid;
                }
                if (lo == 0) return {(int)bi, (int)li, 0};
                if (lo > len) return {(int)bi, (int)li, len};
                float leftX = wl.caretX[lo - 1];
                float rightX = wl.caretX[lo];
                float midX = leftX + (rightX - leftX) / 2;
                return {(int)bi, (int)li, (localX < midX) ? lo - 1 : lo};
            } else {
                if (localX <= 0) return {(int)bi, (int)li, len};
                if (localX >= wl.width) return {(int)bi, (int)li, 0};
                int lo = 0, hi = len + 1;
                while (lo < hi) {
                    int mid = (lo + hi) / 2;
                    if (wl.caretX[mid] >= localX) lo = mid + 1;
                    else hi = mid;
                }
                if (lo == 0) return {(int)bi, (int)li, 0};
                if (lo > len) return {(int)bi, (int)li, len};
                float rightX = wl.caretX[lo - 1];
                float leftX = wl.caretX[lo];
                float midX = leftX + (rightX - leftX) / 2;
                return {(int)bi, (int)li, (localX >= midX) ? lo - 1 : lo};
            }
        }
    }
    auto& lastLine = lines.back();
    return {(int)bi, (int)lines.size() - 1, utf8_codepoint_count(lastLine.text)};
}

void ScrollView::FindWordBoundary(const TextPosition& pos, TextPosition& ws, TextPosition& we) const {
    ws = we = pos;
    if (!pos.IsValid() || pos.block >= (int)wrappedCache_.size()) return;
    auto& lines = wrappedCache_[pos.block];
    if (pos.line >= (int)lines.size()) return;

    const std::string& text = lines[pos.line].text;
    int charLen = utf8_codepoint_count(text);
    int off = std::clamp(pos.offset, 0, charLen);
    if (charLen == 0) { ws = we = {pos.block, pos.line, 0}; return; }

    int charIdx = (off >= charLen) ? charLen - 1 : off;

    // Decode character at charIdx
    size_t bytePos = utf8_char_to_byte(text, charIdx);
    uint32_t cp = utf8_decode_at(text, bytePos);

    auto isWord = [](uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                            (c >= '0' && c <= '9') || c == '_' || c > 0x7F; };
    auto isSpace = [](uint32_t c) { return c == ' ' || c == '\t' || c == '\n'; };

    bool clickedWord = isWord(cp);
    bool clickedSpace = isSpace(cp);

    auto sameCategory = [&](uint32_t c) {
        if (clickedWord) return isWord(c);
        if (clickedSpace) return isSpace(c);
        return !isWord(c) && !isSpace(c);
    };

    int s = charIdx;
    while (s > 0) {
        size_t bp = utf8_char_to_byte(text, s - 1);
        if (!sameCategory(utf8_decode_at(text, bp))) break;
        s--;
    }

    int e = charIdx;
    while (e < charLen) {
        size_t bp = utf8_char_to_byte(text, e);
        if (!sameCategory(utf8_decode_at(text, bp))) break;
        e++;
    }

    ws = {pos.block, pos.line, s};
    we = {pos.block, pos.line, e};
}

// --- Input ---

void ScrollView::OnMouseDown(float x, float y, bool shift) {
    TextPosition pos = HitTest(x, y);

    // Card-style blocks (TOOL_CALL, expandable THINKING): clicks on the
    // header strip toggle collapse; clicks in the body select text via the
    // normal hit-test path. When collapsed, the entire block IS the header,
    // so any click toggles.
    if (pos.IsValid() && pos.block < (int)blocks_.size()) {
        auto& b = blocks_[pos.block];
        bool isExpandableThinking = (b->type == BlockType::THINKING && b->isExpandable);
        if (b->type == BlockType::TOOL_CALL || isExpandableThinking) {
            float blockTop = (pos.block < (int)blockTopCache_.size())
                             ? blockTopCache_[pos.block] - scrollPos_ : 0;
            BlockChrome chrome = ChromeFor(pos.block);
            if (b->isCollapsed || y - blockTop < chrome.headerH) {
                ToggleCollapse(pos.block);
                return;
            }
        }
    }

    double now = GetMonotonicTime();
    bool isMulti = (now - lastClickTime_) < MULTI_CLICK_THRESHOLD &&
                   pos.block == lastClickPos_.block && pos.line == lastClickPos_.line;

    if (isMulti) clickCount_++;
    else clickCount_ = 1;
    lastClickTime_ = now;
    lastClickPos_ = pos;

    if (clickCount_ >= 3) {
        clickCount_ = 3;
        if (pos.IsValid() && pos.block < (int)wrappedCache_.size()) {
            auto& lines = wrappedCache_[pos.block];
            if (pos.line < (int)lines.size()) {
                TextPosition newAnchor = {pos.block, pos.line, 0};
                TextPosition newCaret = {pos.block, pos.line, utf8_codepoint_count(lines[pos.line].text)};
                if (newAnchor != selAnchor_ || newCaret != selCaret_) needsRedraw_ = true;
                selAnchor_ = newAnchor;
                selCaret_ = newCaret;
                wordAnchorStart_ = selAnchor_;
                wordAnchorEnd_ = selCaret_;
            }
        }
        selecting_ = true;
    } else if (clickCount_ == 2) {
        TextPosition ws, we;
        FindWordBoundary(pos, ws, we);
        if (ws != selAnchor_ || we != selCaret_) needsRedraw_ = true;
        selAnchor_ = ws;
        selCaret_ = we;
        wordAnchorStart_ = ws;
        wordAnchorEnd_ = we;
        selecting_ = true;
    } else {
        if (shift && selAnchor_.IsValid()) {
            if (pos != selCaret_) needsRedraw_ = true;
            selCaret_ = pos;
        } else {
            bool hadSelection = HasSelection();
            if (hadSelection || pos != selAnchor_) needsRedraw_ = true;
            selAnchor_ = pos;
            selCaret_ = pos;
        }
        selecting_ = true;
    }
}

void ScrollView::OnMouseUp(float, float) {
    selecting_ = false;
    autoScrollDelta_ = 0;
}

void ScrollView::OnMouseMove(float x, float y, bool leftDown) {
    if (!selecting_ || !leftDown) return;

    TextPosition pos = HitTest(x, y);
    TextPosition oldAnchor = selAnchor_;
    TextPosition oldCaret = selCaret_;

    if (clickCount_ >= 3) {
        if (pos.IsValid() && pos.block < (int)wrappedCache_.size()) {
            auto& lines = wrappedCache_[pos.block];
            if (pos < wordAnchorStart_) {
                selAnchor_ = wordAnchorEnd_;
                selCaret_ = {pos.block, pos.line, 0};
            } else if (pos > wordAnchorEnd_) {
                selAnchor_ = wordAnchorStart_;
                int lineLen = (pos.line < (int)lines.size()) ? utf8_codepoint_count(lines[pos.line].text) : 0;
                selCaret_ = {pos.block, pos.line, lineLen};
            } else {
                selAnchor_ = wordAnchorStart_;
                selCaret_ = wordAnchorEnd_;
            }
        }
    } else if (clickCount_ == 2) {
        TextPosition ws, we;
        FindWordBoundary(pos, ws, we);
        if (pos < wordAnchorStart_) { selAnchor_ = wordAnchorEnd_; selCaret_ = ws; }
        else if (pos > wordAnchorEnd_) { selAnchor_ = wordAnchorStart_; selCaret_ = we; }
        else { selAnchor_ = wordAnchorStart_; selCaret_ = wordAnchorEnd_; }
    } else {
        selCaret_ = pos;
    }

    if (selAnchor_ != oldAnchor || selCaret_ != oldCaret) needsRedraw_ = true;

    // Auto-scroll at edges
    float edgeMargin = 30;
    if (y < edgeMargin) autoScrollDelta_ = -(edgeMargin - y);
    else if (y > windowH_ - edgeMargin) autoScrollDelta_ = y - (windowH_ - edgeMargin);
    else autoScrollDelta_ = 0;
}

void ScrollView::OnKey(int key, int mods) {
    bool ctrl = (mods & Mod::Ctrl) != 0;

    if (ctrl && key == Key::A) {
        SelectAll();
    } else if (ctrl && key == Key::C) {
        if (HasSelection() && clipboardFn_) {
            clipboardFn_(GetSelectedText());
        }
    } else if (key == Key::Escape) {
        ClearSelection();
    } else if (key == Key::Up) {
        float lh = fonts_.LineHeight(FontStyle::Regular);
        ScrollBy(lh);
    } else if (key == Key::Down) {
        float lh = fonts_.LineHeight(FontStyle::Regular);
        ScrollBy(-lh);
    } else if (key == Key::PageUp) {
        ScrollBy((float)windowH_);
    } else if (key == Key::PageDown || key == Key::Space) {
        ScrollBy(-(float)windowH_);
    } else if (key == Key::Home) {
        scrollPos_ = 0;
        needsRedraw_ = true;
    } else if (key == Key::End) {
        ScrollToBottom();
    }
}

// --- Paint ---

void ScrollView::Paint(GLRenderer& renderer) {
    size_t blockCount = blocks_.size();
    float clientW = (float)windowW_;
    float clientH = (float)windowH_;
    // textAreaW is the centered chat column width. wl.x for non-bubble blocks
    // is leftMargin_, and bubble/code/tool chrome insets text further via
    // ChromeFor().xOff at paint time.
    float textAreaW = contentW_;

    // Incremental rebuild: only last block changed (streaming)
    if (streamDirty_ && !needsFullRebuild_ && blockCount > 0) {
        size_t lastIdx = blockCount - 1;
        if (lastIdx < wrappedCache_.size()) {
            RebuildSingleBlock(lastIdx, textAreaW, clientW);
            UpdateBlockTopCacheTail(lastIdx);
        }
        streamDirty_ = false;
    }

    // Full rebuild
    if (needsFullRebuild_) {
        wrappedCache_.resize(blockCount);
        shapedCache_.resize(blockCount);
        blockHeightCache_.resize(blockCount);
        charHeightCache_.resize(blockCount);
        segCache_.resize(blockCount);
        size_t oldValidSize = segValid_.size();
        segValid_.resize(blockCount, false);
        segTextLen_.resize(blockCount, 0);
        if ((int)clientW != cachedW_) {
            for (size_t i = 0; i < oldValidSize && i < blockCount; i++)
                segValid_[i] = false;
        }
        tableLayoutCache_.clear();
        tableLayoutCache_.resize(blockCount);

        for (size_t i = 0; i < blockCount; i++) {
            RebuildSingleBlock(i, textAreaW, clientW);
        }

        cachedW_ = (int)clientW;
        needsFullRebuild_ = false;
        streamDirty_ = false;
        RebuildBlockTopCache();
    }

    if (blockCount == 0) return;

    // Auto-scroll
    if (autoScroll_) {
        float maxS = std::max(0.0f, cachedTotalH_ - clientH);
        scrollPos_ = maxS;
    }

    float maxS = std::max(0.0f, cachedTotalH_ - clientH);
    scrollPos_ = std::clamp(scrollPos_, 0.0f, maxS);

    // Selection
    TextPosition selStart, selEnd;
    bool hasSel = HasSelection();
    if (hasSel) GetOrderedSel(selStart, selEnd);

    // Background
    renderer.DrawRect(0, 0, clientW, clientH, bgColor_);

    size_t firstVis = FindFirstVisible(scrollPos_);
    size_t drawLimit = std::min({blockCount, wrappedCache_.size(), blockHeightCache_.size()});

    for (size_t i = firstVis; i < drawLimit; i++) {
        float blockH = blockHeightCache_[i];
        float blockTop = blockTopCache_[i] - scrollPos_;

        if (blockTop > clientH) break;
        if (blockTop + blockH < 0) continue;

        auto& block = *blocks_[i];
        FontStyle blockStyle = StyleForType(block.type);
        float ascent = fonts_.Ascent(blockStyle);

        BlockChrome chrome = ChromeFor(i);

        // THINKING: one continuous rounded bubble in thinkingBg_. Collapsed
        // is just the header strip; expanded keeps the same bubble shape and
        // extends it downward — there is no visual seam between header and
        // body, matching the wx branch design.
        if (block.type == BlockType::THINKING) {
            renderer.DrawRoundedRect(chrome.bubbleX, blockTop, chrome.bubbleW,
                                     blockH, toolRadius_, thinkingBg_);

            // Chevron at the header's left pad, vertically centered with the
            // peek line. Only drawn for expandable thinking blocks.
            if (block.isExpandable) {
                FontStyle ts = FontStyle::ThinkingItalic;
                float thinkAsc = fonts_.Ascent(ts);
                std::string chev = block.isCollapsed ? "\xe2\x96\xb8 "
                                                     : "\xe2\x96\xbe ";
                float chevX = chrome.bubbleX + toolPadX_;
                float chevY = blockTop + toolPadY_;
                ShapedRun cs = fonts_.Shape(chev, ts, false);
                renderer.DrawShapedRun(fonts_, cs, chevX, chevY, thinkAsc,
                                       thinkingColor_);
            }
        }

        // USER_PROMPT bubble: rounded rect right-aligned in the column. Width
        // hugs the longest line + 2*bubblePad so short prompts read as a chip.
        if (block.type == BlockType::USER_PROMPT) {
            renderer.DrawRoundedRect(chrome.bubbleX, blockTop, chrome.bubbleW,
                                     blockH, userBubbleRadius_, userPromptBg_);
        }

        // CODE block: rounded bg spanning the full column. Text is inset by
        // codeHPad_/codeVPad_ via chrome.xOff/yOff.
        if (block.type == BlockType::CODE) {
            renderer.DrawRoundedRect(chrome.bubbleX, blockTop, chrome.bubbleW,
                                     blockH, codeRadius_, codeBg_);
        }

        // TOOL_CALL: one continuous rounded bubble — body bg fills the whole
        // shape when expanded; when collapsed only the header strip is drawn.
        // For the expanded case the header tint is composed on top of the
        // upper strip (rounded top corners, square bottom) so it reads as a
        // single bubble with two color bands, not two separate cards.
        if (block.type == BlockType::TOOL_CALL) {
            if (chrome.bodyH > 0) {
                renderer.DrawRoundedRect(chrome.bubbleX, blockTop, chrome.bubbleW,
                                         blockH, toolRadius_, toolBodyBg_);
                // Header tint: round only the top corners. A short rounded rect
                // gives the top curves; a straight rect below fills the rest of
                // the header height with a flat bottom edge against the body.
                renderer.DrawRoundedRect(chrome.bubbleX, blockTop, chrome.bubbleW,
                                         toolRadius_ * 2, toolRadius_, toolHeaderBg_);
                renderer.DrawRect(chrome.bubbleX, blockTop + toolRadius_,
                                  chrome.bubbleW, chrome.headerH - toolRadius_,
                                  toolHeaderBg_);
            } else {
                renderer.DrawRoundedRect(chrome.bubbleX, blockTop, chrome.bubbleW,
                                         blockH, toolRadius_, toolHeaderBg_);
            }

            // Selection feedback. A collapsed tool block has no wrapped
            // lines, so the per-line selection pass below can't draw
            // anything — when the selection fully spans this block, we
            // paint tight rects behind just the actual text glyphs (left
            // text segment + right-aligned hint), matching how selection
            // looks on any other line of text instead of tinting the
            // entire bubble width.
            bool headerSelected = false;
            if (hasSel) {
                TextPosition bStart = {(int)i, 0, 0};
                TextPosition bEnd = {(int)i + 1, 0, 0};
                headerSelected = (selStart < bEnd && selEnd >= bStart && selStart <= bStart);
            }

            // Header content. Pre-formatted on the fly: chevron + name +
            // " (args)" + right-aligned hint when collapsed (e.g. "· 12 lines").
            std::string chev = block.isCollapsed ? "\xe2\x96\xb8 "  // ▸
                                                 : "\xe2\x96\xbe "; // ▾
            FontStyle code = FontStyle::Code;
            float codeAsc = fonts_.Ascent(code);
            float codeLh = fonts_.LineHeight(code);
            float textY = blockTop + toolPadY_;
            float leftStart = chrome.bubbleX + toolPadX_;
            float xCursor = leftStart;

            float chevW = fonts_.MeasureWidth(chev, code, false);
            std::string nameStr = block.toolName.empty() ? "(tool)" : block.toolName;
            float nameW = fonts_.MeasureWidth(nameStr, code, false);

            std::string hint;
            if (block.isCollapsed && !block.toolResult.empty()) {
                int nlines = 1;
                for (char c : block.toolResult) if (c == '\n') ++nlines;
                if (nlines > 1) hint = " \xc2\xb7 " + std::to_string(nlines) + " lines";
                else hint = " \xc2\xb7 ok";
            } else if (block.isCollapsed && block.isLoading) {
                hint = " \xc2\xb7 \xe2\x80\xa6";  // · …
            }
            float hintW = hint.empty() ? 0 : fonts_.MeasureWidth(hint, code, false);
            float rightLimit = chrome.bubbleX + chrome.bubbleW - toolPadX_;
            float argsRight = rightLimit - hintW;
            float argsAvail = argsRight - (leftStart + chevW + nameW);

            // Truncate args display ahead of drawing so we know the
            // left-segment's true width for the selection rect.
            std::string argsDisplay;
            float argsDisplayW = 0;
            if (!block.toolArgs.empty() && argsAvail > 20) {
                argsDisplay = "(" + block.toolArgs + ")";
                argsDisplayW = fonts_.MeasureWidth(argsDisplay, code, false);
                if (argsDisplayW > argsAvail) {
                    const std::string ell = "\xe2\x80\xa6)";  // …)
                    float ellW = fonts_.MeasureWidth(ell, code, false);
                    float budget = argsAvail - ellW;
                    if (budget < 1) budget = 1;
                    size_t cut = 1;  // start past the leading '('
                    float w = fonts_.MeasureWidth("(", code, false);
                    while (cut < argsDisplay.size()) {
                        size_t stride = utf8_stride(argsDisplay, cut);
                        std::string ch = argsDisplay.substr(cut, stride);
                        float cw = fonts_.MeasureWidth(ch, code, false);
                        if (w + cw > budget) break;
                        w += cw;
                        cut += stride;
                    }
                    argsDisplay = argsDisplay.substr(0, cut) + ell;
                    argsDisplayW = fonts_.MeasureWidth(argsDisplay, code, false);
                }
            }

            // Selection rects behind the two text segments (left = chev+name+args,
            // right = hint), sized to glyph height like a regular text line.
            if (headerSelected) {
                float leftW = chevW + nameW + argsDisplayW;
                if (leftW > 0) {
                    renderer.DrawRect(leftStart, textY, leftW, codeLh, selBgColor_);
                }
                if (hintW > 0) {
                    renderer.DrawRect(argsRight, textY, hintW, codeLh, selBgColor_);
                }
            }

            // Color the header runs. When selected, swap to selTextColor_
            // so text stays legible against the selection background.
            Color cChev = headerSelected ? selTextColor_ : toolAccent_;
            Color cName = headerSelected ? selTextColor_ : toolAccent_;
            Color cArgs = headerSelected ? selTextColor_ : codeColor_;
            Color cHint = headerSelected ? selTextColor_ : toolDim_;

            ShapedRun chevShape = fonts_.Shape(chev, code, false);
            renderer.DrawShapedRun(fonts_, chevShape, xCursor, textY, codeAsc, cChev);
            xCursor += chevW;

            ShapedRun nameShape = fonts_.Shape(nameStr, code, false);
            renderer.DrawShapedRun(fonts_, nameShape, xCursor, textY, codeAsc, cName);
            xCursor += nameW;

            if (!argsDisplay.empty()) {
                ShapedRun argsShape = fonts_.Shape(argsDisplay, code, false);
                renderer.DrawShapedRun(fonts_, argsShape, xCursor, textY, codeAsc, cArgs);
            }
            if (!hint.empty()) {
                ShapedRun hintShape = fonts_.Shape(hint, code, false);
                renderer.DrawShapedRun(fonts_, hintShape, argsRight, textY, codeAsc, cHint);
            }
        }

        // Table chrome (wx-branch look): no body fill (page bg shows through),
        // tinted header row, full grid of 1px rules at every internal column
        // and row edge, plus a 1px outer border. Glyphs render on top.
        if (block.type == BlockType::TABLE && i < tableLayoutCache_.size()) {
            auto& tinfo = tableLayoutCache_[i];
            if (!tinfo.colX.empty() && !wrappedCache_[i].empty()) {
                const float tableX = leftMargin_;
                const float tableY = blockTop;
                const float tableW = tinfo.totalW;
                const float tableH = blockH;

                // Header row fill.
                if (tinfo.headerBottomY > 0) {
                    renderer.DrawRect(tableX, tableY, tableW, tinfo.headerBottomY, tableHeaderBg_);
                }

                const float bw = tableBorderW_;
                const float bhalf = bw * 0.5f;

                // Horizontal rules between rows.
                for (size_t r = 0; r + 1 < tinfo.rowBottomY.size(); r++) {
                    float ry = tableY + tinfo.rowBottomY[r];
                    renderer.DrawRect(tableX, ry - bhalf, tableW, bw, tableBorderColor_);
                }

                // Vertical column dividers — internal edges only. colX has N
                // entries for an N-column table (left edge of each column);
                // internal dividers sit at colX[1..N-1].
                for (size_t c = 1; c < tinfo.colX.size(); c++) {
                    float dx = tableX + tinfo.colX[c];
                    renderer.DrawRect(dx - bhalf, tableY, bw, tableH, tableBorderColor_);
                }

                // Outer border outline (top/bottom/left/right).
                renderer.DrawRect(tableX, tableY, tableW, bw, tableBorderColor_);
                renderer.DrawRect(tableX, tableY + tableH - bw, tableW, bw, tableBorderColor_);
                renderer.DrawRect(tableX, tableY, bw, tableH, tableBorderColor_);
                renderer.DrawRect(tableX + tableW - bw, tableY, bw, tableH, tableBorderColor_);
            }
        }

        // Draw lines. For bubble/code/tool blocks the wrapped lines store
        // wl.x = leftMargin_; chrome.xOff/yOff shift them into the bubble or
        // code box, and the line draw + selection rect must apply the same
        // shift the hit-test does (else clicks land off-text).
        auto& lines = wrappedCache_[i];
        for (size_t li = 0; li < lines.size(); li++) {
            auto& wl = lines[li];
            float absY = blockTop + wl.y + chrome.yOff;

            if (absY + wl.height < 0) continue;
            if (absY > clientH) break;

            // Check selection (skip utf8 counting when no selection active)
            bool lineSelected = false;
            int lineCharLen = 0;
            int selFromChar = 0, selToChar = 0;
            if (hasSel) {
                lineCharLen = utf8_codepoint_count(wl.text);
                TextPosition lineStart = {(int)i, (int)li, 0};
                TextPosition lineEnd = {(int)i, (int)li, lineCharLen};
                lineSelected = selStart <= lineEnd && selEnd >= lineStart;
            }

            if (lineSelected) {
                // Compute character range of selection on this line
                TextPosition lineStart = {(int)i, (int)li, 0};
                TextPosition lineEnd = {(int)i, (int)li, lineCharLen};
                if (selStart <= lineStart && lineEnd <= selEnd) {
                    selFromChar = 0;
                    selToChar = lineCharLen;
                } else {
                    selFromChar = (selStart.block == (int)i && selStart.line == (int)li) ? selStart.offset : 0;
                    selToChar = (selEnd.block == (int)i && selEnd.line == (int)li) ? selEnd.offset : lineCharLen;
                }
                selFromChar = std::clamp(selFromChar, 0, lineCharLen);
                selToChar = std::clamp(selToChar, 0, lineCharLen);

                if (selFromChar < selToChar) {
                    // Compute selection rect
                    auto& mutWl = const_cast<WrappedLine&>(wl);
                    EnsureCaretX(mutWl, blockStyle, wl.rightToLeft);

                    float vx1 = CaretXForOffset(wl, selFromChar);
                    float vx2 = CaretXForOffset(wl, selToChar);
                    float left = std::min(vx1, vx2);
                    float right = std::max(vx1, vx2);
                    renderer.DrawRect(wl.x + chrome.xOff + left, absY, right - left, wl.height, selBgColor_);
                }
            }

            // Ensure shaped runs are cached for this line
            EnsureShapedCache(i, li);
            auto& shapes = shapedCache_[i][li];

            // Draw text using cached shapes with per-glyph selection coloring
            if (!wl.styledRuns.empty() && shapes.size() == wl.styledRuns.size()) {
                // Build byte-to-char map for selection coloring. Table rows
                // carry explicit per-run starting char offsets (runCharStarts)
                // because the tab characters between cells live in `.text`
                // but have no matching styled run, which desynchronises the
                // default running accumulator.
                bool useExplicitStarts = !wl.runCharStarts.empty() &&
                                         wl.runCharStarts.size() == wl.styledRuns.size();
                int charAcc = 0;
                for (size_t ri = 0; ri < wl.styledRuns.size(); ri++) {
                    auto& run = wl.styledRuns[ri];
                    auto& shaped = shapes[ri];
                    float runX = wl.x + chrome.xOff + (ri < wl.runXOffsets.size() ? wl.runXOffsets[ri] : 0);
                    float runAsc = fonts_.Ascent(run.style);
                    int runChars = utf8_codepoint_count(run.text);
                    int runStartChar = useExplicitStarts ? wl.runCharStarts[ri] : charAcc;

                    if (!lineSelected || (selFromChar == 0 && selToChar == lineCharLen)) {
                        // Full line: single color
                        Color c = (lineSelected && selFromChar == 0 && selToChar == lineCharLen)
                                  ? selTextColor_ : run.color;
                        renderer.DrawShapedRun(fonts_, shaped, runX, absY, runAsc, c);
                    } else {
                        // Per-glyph coloring for partial selection
                        for (auto& g : shaped.glyphs) {
                            int gc = runStartChar + utf8_byte_to_char(run.text, g.cluster);
                            Color c = (gc >= selFromChar && gc < selToChar) ? selTextColor_ : run.color;
                            const GlyphInfo& gi = fonts_.EnsureGlyph(g.glyphId, g.faceIdx);
                            renderer.DrawGlyph(gi, runX + g.xPos, absY, c, runAsc);
                        }
                    }
                    charAcc += runChars;
                }
            } else if (!wl.text.empty() && !shapes.empty()) {
                auto& shaped = shapes[0];
                Color normalC = ColorForType(block.type);
                float drawX = wl.x + chrome.xOff;

                if (!lineSelected || (selFromChar == 0 && selToChar == lineCharLen)) {
                    Color c = (lineSelected && selFromChar == 0 && selToChar == lineCharLen)
                              ? selTextColor_ : normalC;
                    renderer.DrawShapedRun(fonts_, shaped, drawX, absY, ascent, c);
                } else if (lineSelected && selFromChar < selToChar) {
                    // Per-glyph coloring for partial selection
                    for (auto& g : shaped.glyphs) {
                        int gc = utf8_byte_to_char(wl.text, g.cluster);
                        Color c = (gc >= selFromChar && gc < selToChar) ? selTextColor_ : normalC;
                        const GlyphInfo& gi = fonts_.EnsureGlyph(g.glyphId, g.faceIdx);
                        renderer.DrawGlyph(gi, drawX + g.xPos, absY, c, ascent);
                    }
                } else {
                    renderer.DrawShapedRun(fonts_, shaped, drawX, absY, ascent, normalC);
                }
            }
        }

        // Loading dots for thinking blocks. Anchored relative to the column
        // (chrome.bubbleX + toolPadX_) so they never spill into the side
        // margin, and bounded against the right edge so collapsed peek lines
        // can't push them off the bubble.
        if (block.type == BlockType::THINKING && block.isLoading && !lines.empty()) {
            float ch = charHeightCache_[i];
            float dotR = ch / 5;
            float dotSpacing = dotR * 3;
            float dotsW = dotSpacing * (LOADING_DOT_COUNT - 1) + dotR * 2;
            float rightLimit = chrome.bubbleX + chrome.bubbleW - toolPadX_;

            float dotX, dotY;
            if (block.isCollapsed || !block.isExpandable) {
                // Collapsed / single-line: dots at end of peek text on the
                // header row.
                auto& firstLine = lines[0];
                dotX = firstLine.x + chrome.xOff + firstLine.width + dotSpacing;
                dotY = blockTop + firstLine.y + chrome.yOff + ch / 2;
            } else {
                // Expanded: dots on a new row at the bottom of the body box.
                // RebuildSingleBlock already reserved `thinkingLh` for this.
                auto& lastLine = lines.back();
                dotX = chrome.bubbleX + toolPadX_;
                dotY = blockTop + lastLine.y + chrome.yOff + lastLine.height + ch / 2;
            }
            if (dotX + dotsW > rightLimit) dotX = rightLimit - dotsW;
            if (dotX < chrome.bubbleX + toolPadX_) dotX = chrome.bubbleX + toolPadX_;

            for (int d = 0; d < LOADING_DOT_COUNT; d++) {
                float cx = dotX + d * dotSpacing;
                bool active = (d == loadingFrame_);
                float r = active ? dotR : dotR * 0.6f;
                Color dc = active ? thinkingColor_ : Color::RGB(100, 100, 100);
                renderer.DrawRect(cx - r, dotY - r, r * 2, r * 2, dc);
            }
        }
    }
}
