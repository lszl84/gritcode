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

    void Paint(GLRenderer& renderer);
    bool NeedsRedraw() const { return needsRedraw_; }
    void MarkDirty() { needsRedraw_ = true; }
    void ClearDirty() { needsRedraw_ = false; }

    void StartThinking(size_t blockIdx);
    void StopThinking(size_t blockIdx);
    void StopAllAnimations();
    void ToggleCollapse(size_t blockIdx);
    void Update(float dt);

    size_t BlockCount() const { return blocks_.size(); }
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

    Color bgColor_, normalColor_, userPromptColor_, thinkingColor_, codeColor_;
    Color codeBg_, thinkingBg_, userPromptBg_;
    Color selBgColor_, selTextColor_;

    mutable std::vector<std::vector<std::vector<ShapedRun>>> shapedCache_;
    std::vector<std::vector<WrappedLine>> wrappedCache_;
    std::vector<float> blockHeightCache_;
    std::vector<float> charHeightCache_;
    std::vector<float> blockTopCache_;
    float cachedTotalH_ = 0;
    int cachedW_ = -1;
    bool needsFullRebuild_ = true;
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
    WrappedLine MakeLine(const std::string& text, bool rtl, float textW,
                         float textH, float margin, float clientW, int indent = 0);

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
};
