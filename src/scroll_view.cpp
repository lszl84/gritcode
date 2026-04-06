// FastCode Native — GPU-rendered AI coding harness
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
#ifdef NDEBUG
    bgColor_ = Color::RGB(30, 30, 30);
#else
    bgColor_ = Color::RGB(34, 26, 26);  // Reddish tint in debug
#endif
    normalColor_ = Color::RGB(220, 220, 220);
    userPromptColor_ = Color::RGB(100, 180, 255);
    thinkingColor_ = Color::RGB(160, 160, 160);
    codeColor_ = Color::RGB(230, 230, 230);
    codeBg_ = Color::RGB(45, 45, 48);
    thinkingBg_ = Color::RGB(40, 40, 43);
    userPromptBg_ = Color::RGB(35, 45, 55);
    selBgColor_ = Color::RGB(51, 153, 255);
    selTextColor_ = Color::RGB(255, 255, 255);
}

bool ScrollView::Init(int w, int h, float scale) {
    windowW_ = w;
    windowH_ = h;
    InitColors();

    int baseFontPx = (int)(14 * scale + 0.5f);
    if (baseFontPx < 14) baseFontPx = 14;
    if (!fonts_.Init(baseFontPx)) return false;

    leftMargin_ = 16 * scale;

    float lh = fonts_.LineHeight(FontStyle::Regular);
    topMargin_ = lh;
    blockSpacing_ = lh / 2;
    cachedTotalH_ = topMargin_ * 2;

    return true;
}

FontStyle ScrollView::StyleForType(BlockType t) const {
    switch (t) {
    case BlockType::THINKING: return FontStyle::ThinkingItalic;
    case BlockType::CODE: return FontStyle::Code;
    default: return FontStyle::Regular;
    }
}

Color ScrollView::ColorForType(BlockType t) const {
    switch (t) {
    case BlockType::USER_PROMPT: return userPromptColor_;
    case BlockType::THINKING: return thinkingColor_;
    case BlockType::CODE: return codeColor_;
    default: return normalColor_;
    }
}

Color ScrollView::BgForType(BlockType t) const {
    switch (t) {
    case BlockType::USER_PROMPT: return userPromptBg_;
    case BlockType::THINKING: return thinkingBg_;
    case BlockType::CODE: return codeBg_;
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

    // Invalidate cache for last block
    size_t idx = blocks_.size() - 1;
    if (idx < segValid_.size()) segValid_[idx] = false;
    if (idx < wrappedCache_.size()) {
        for (auto& wl : wrappedCache_[idx]) {
            wl.caretXValid = false;
            wl.shapedValid = false;
        }
    }
    if (idx < shapedCache_.size()) shapedCache_[idx].clear();

    if (autoScroll_) scrollPos_ = 999999;
    if (!inBatch_) needsFullRebuild_ = true;
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
    needsFullRebuild_ = true;
    needsRedraw_ = true;
}

// --- Animation ---

void ScrollView::StartThinking(size_t idx) {
    if (idx < blocks_.size()) {
        blocks_[idx]->isLoading = true;
        animatedBlocks_.insert(idx);
        needsRedraw_ = true;
    }
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
        animTime_ -= ANIMATION_INTERVAL;
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
            bool first = true;

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
                first = false;
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
            float h = segs.empty() ? fonts_.LineHeight(blockStyle) : segs[0].height;
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

        WrappedLine wl = MakeLine(lineText, lineRtl, lineW, lineH > 0 ? lineH : fonts_.LineHeight(blockStyle),
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
        outH += lineH > 0 ? lineH : fonts_.LineHeight(blockStyle);
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
    int lastB = (int)blocks_.size() - 1;
    TextPosition newCaret = newAnchor;
    if (lastB < (int)wrappedCache_.size() && !wrappedCache_[lastB].empty()) {
        auto& lastLines = wrappedCache_[lastB];
        int lastL = (int)lastLines.size() - 1;
        newCaret = {lastB, lastL, utf8_codepoint_count(lastLines[lastL].text)};
    }
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
    float localY = virtualY - blockTop;

    for (size_t li = 0; li < lines.size(); li++) {
        auto& wl = lines[li];
        float lineBot = wl.y + wl.height;

        if (localY < lineBot || li == lines.size() - 1) {
            if (wl.text.empty()) return {(int)bi, (int)li, 0};

            auto& mutWl = const_cast<WrappedLine&>(wl);
            FontStyle style = StyleForType(blocks_[bi]->type);
            const_cast<ScrollView*>(this)->EnsureCaretX(mutWl, style, blocks_[bi]->rightToLeft);

            float localX = px - wl.x;
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

    // Toggle thinking collapse (only expandable blocks)
    if (pos.IsValid() && pos.block < (int)blocks_.size()) {
        auto& b = blocks_[pos.block];
        if (b->type == BlockType::THINKING && b->isExpandable) {
            if (b->isCollapsed || pos.line == 0) {
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

void ScrollView::OnMouseUp(float x, float y) {
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
    float textAreaW = clientW - leftMargin_ * 2;

    // Full rebuild
    if (needsFullRebuild_) {
        wrappedCache_.resize(blockCount);
        shapedCache_.resize(blockCount);
        blockHeightCache_.resize(blockCount);
        charHeightCache_.resize(blockCount);
        segCache_.resize(blockCount);
        segValid_.assign(blockCount, false);    // Force re-measure ALL
        segTextLen_.assign(blockCount, 0);

        for (size_t i = 0; i < blockCount; i++) {
            auto& block = *blocks_[i];
            if (!segValid_[i]) {
                if (block.HasStyledRuns())
                    MeasureStyledSegments(i);
                else
                    MeasureSegments(i);
            }

            float h = 0;
            LayoutFromSegments(i, textAreaW, clientW, wrappedCache_[i], h);
            shapedCache_[i].clear(); // New lines need fresh shapes

            // Track expandability and handle collapse
            if (block.type == BlockType::THINKING) {
                block.isExpandable = (wrappedCache_[i].size() > 1);
                // Single-line AND done loading: not expandable, show uncollapsed
                // But keep collapsed while loading (block may grow)
                if (!block.isExpandable && !block.isLoading) block.isCollapsed = false;

                if (block.isCollapsed && block.isExpandable) {
                    // Collapsed: show only first line, truncated to leave room for dots
                    float firstH = wrappedCache_[i][0].height;
                    wrappedCache_[i].resize(1);
                    h = firstH;

                    if (block.isLoading) {
                        // Truncate first line text to fit dots
                        float ch = fonts_.LineHeight(FontStyle::ThinkingItalic);
                        float dotSpace = ch * 2.5f;  // Space needed for 3 dots
                        float maxTextW = textAreaW - dotSpace - 20;  // 20 for triangle
                        auto& wl = wrappedCache_[i][0];
                        if (wl.width > maxTextW && !wl.text.empty()) {
                            // Truncate word by word
                            std::string truncated = wl.text;
                            while (!truncated.empty() && fonts_.MeasureWidth(truncated, FontStyle::ThinkingItalic) > maxTextW) {
                                size_t sp = truncated.rfind(' ');
                                if (sp == std::string::npos) { truncated.clear(); break; }
                                truncated = truncated.substr(0, sp);
                            }
                            if (!truncated.empty() && truncated.size() < wl.text.size())
                                truncated += "\xe2\x80\xa6";  // …
                            wl.text = truncated;
                            wl.width = fonts_.MeasureWidth(truncated, FontStyle::ThinkingItalic);
                            wl.shapedValid = false;
                            if (i < shapedCache_.size()) shapedCache_[i].clear();
                        }
                    }
                }
            }

            float ch = fonts_.LineHeight(StyleForType(block.type));
            if (block.isLoading && !block.isCollapsed && block.isExpandable) h += ch;
            blockHeightCache_[i] = h;
            charHeightCache_[i] = ch;
        }

        cachedW_ = (int)clientW;
        needsFullRebuild_ = false;
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

        // Block backgrounds with padding
        float bgPad = 6;  // Vertical padding above and below text
        Color bg = BgForType(block.type);
        if (bg.a > 0) {
            if (block.type == BlockType::THINKING) {
                renderer.DrawRect(0, blockTop - bgPad, clientW, blockH + bgPad * 2, bg);
            } else {
                renderer.DrawRect(leftMargin_ - 8, blockTop - bgPad, textAreaW + 16, blockH + bgPad * 2, bg);
            }
        }

        // User prompt left border
        if (block.type == BlockType::USER_PROMPT) {
            renderer.DrawRect(leftMargin_ - 8, blockTop - bgPad, 3, blockH + bgPad * 2, userPromptColor_);
        }

        // Thinking collapse/expand triangle (only for expandable blocks)
        if (block.type == BlockType::THINKING && block.isExpandable && !wrappedCache_[i].empty()) {
            float ch = fonts_.LineHeight(FontStyle::ThinkingItalic);
            float triSize = ch * 0.4f;
            float triY = blockTop + wrappedCache_[i][0].y + ch / 2;
            if (block.isCollapsed)
                renderer.DrawTriRight(4, triY, triSize, thinkingColor_);
            else
                renderer.DrawTriDown(4 + triSize / 2, triY, triSize, thinkingColor_);
        }

        // Draw lines
        auto& lines = wrappedCache_[i];
        for (size_t li = 0; li < lines.size(); li++) {
            auto& wl = lines[li];
            float absY = blockTop + wl.y;

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
                    renderer.DrawRect(wl.x + left, absY, right - left, wl.height, selBgColor_);
                }
            }

            // Ensure shaped runs are cached for this line
            EnsureShapedCache(i, li);
            auto& shapes = shapedCache_[i][li];

            // Draw text using cached shapes with per-glyph selection coloring
            if (!wl.styledRuns.empty() && shapes.size() == wl.styledRuns.size()) {
                // Build byte-to-char map for selection coloring
                int charAcc = 0;
                for (size_t ri = 0; ri < wl.styledRuns.size(); ri++) {
                    auto& run = wl.styledRuns[ri];
                    auto& shaped = shapes[ri];
                    float runX = wl.x + (ri < wl.runXOffsets.size() ? wl.runXOffsets[ri] : 0);
                    float runAsc = fonts_.Ascent(run.style);
                    int runChars = utf8_codepoint_count(run.text);

                    if (!lineSelected || (selFromChar == 0 && selToChar == lineCharLen)) {
                        // Full line: single color
                        Color c = (lineSelected && selFromChar == 0 && selToChar == lineCharLen)
                                  ? selTextColor_ : run.color;
                        renderer.DrawShapedRun(fonts_, shaped, runX, absY, runAsc, c);
                    } else {
                        // Per-glyph coloring for partial selection
                        for (auto& g : shaped.glyphs) {
                            int gc = charAcc + utf8_byte_to_char(run.text, g.cluster);
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

                if (!lineSelected || (selFromChar == 0 && selToChar == lineCharLen)) {
                    Color c = (lineSelected && selFromChar == 0 && selToChar == lineCharLen)
                              ? selTextColor_ : normalC;
                    renderer.DrawShapedRun(fonts_, shaped, wl.x, absY, ascent, c);
                } else if (lineSelected && selFromChar < selToChar) {
                    // Per-glyph coloring for partial selection
                    for (auto& g : shaped.glyphs) {
                        int gc = utf8_byte_to_char(wl.text, g.cluster);
                        Color c = (gc >= selFromChar && gc < selToChar) ? selTextColor_ : normalC;
                        const GlyphInfo& gi = fonts_.EnsureGlyph(g.glyphId, g.faceIdx);
                        renderer.DrawGlyph(gi, wl.x + g.xPos, absY, c, ascent);
                    }
                } else {
                    renderer.DrawShapedRun(fonts_, shaped, wl.x, absY, ascent, normalC);
                }
            }
        }

        // Loading dots for thinking blocks
        if (block.type == BlockType::THINKING && block.isLoading && !lines.empty()) {
            float ch = charHeightCache_[i];
            float dotR = ch / 5;
            float dotSpacing = dotR * 3;

            float dotX, dotY;
            if (block.isCollapsed || !block.isExpandable) {
                // Collapsed or single-line: dots at end of first line
                auto& firstLine = lines[0];
                dotX = firstLine.x + firstLine.width + dotSpacing;
                dotY = blockTop + firstLine.y + ch / 2;
            } else {
                // Expanded multi-line: dots on new line below last text
                auto& lastLine = lines.back();
                dotX = leftMargin_ + 10;
                dotY = blockTop + lastLine.y + lastLine.height + ch / 2;
            }

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
