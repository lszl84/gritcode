#pragma once

#include <wx/wx.h>
#include <vector>
#include <memory>
#include <functional>

// Different types of content blocks
enum class BlockType {
    NORMAL,      // Regular text (agent responses)
    USER_PROMPT, // User input/prompts (distinct styling)
    THINKING,    // Small grey text (reasoning/thinking)
    CODE,        // Monospace code blocks
    LOADING      // Animated loading indicator
};

// A styled run of text - for inline formatting (bold, italic, code, etc.)
struct StyledTextRun {
    wxString text;
    wxFont font;
    wxColour colour;
    
    StyledTextRun() = default;
    StyledTextRun(const wxString& t, const wxFont& f, const wxColour& c)
        : text(t), font(f), colour(c) {}
};

// Forward declaration
struct TextBlock;

// Interface for line-based access
class BlockManager {
public:
    BlockManager();
    ~BlockManager();
    
    // Add new content - creates new block or appends to last if same type
    void AppendStream(BlockType type, const wxString& text, bool rtl = false);
    
    // Get block for a specific virtual line
    const TextBlock* GetBlockForLine(int line) const;
    
    // Get block by index
    const TextBlock* GetBlock(size_t index) const;
    size_t GetBlockCount() const { return blocks.size(); }
    
    // Total virtual lines across all blocks
    int GetTotalLines() const { return totalLines; }
    
    // Get the line offset within a block for a virtual line
    int GetLineInBlock(int virtualLine, size_t& blockIndex) const;
    
    // Clear all content
    void Clear();
    
    // Update loading state for a block
    void SetBlockLoading(size_t blockIndex, bool loading);
    
    // Get the last block (for appending)
    TextBlock* GetLastBlock();
    
    // Remove the last block (for replacing streamed text with markdown)
    void RemoveLastBlock();
    
    // Remove all blocks from index onwards
    void RemoveBlocksFrom(size_t fromIndex);
    
    // Add multiple blocks (for markdown rendering)
    void AddBlocks(std::vector<std::unique_ptr<TextBlock>> newBlocks);
    
private:
    std::vector<std::unique_ptr<TextBlock>> blocks;
    int totalLines;
    wxFont normalFont;
    wxFont thinkingFont;
    wxFont codeFont;
    int lineHeight;
    int codeLineHeight;
    int thinkingLineHeight;
    
    void RecalculateLineCounts();
    int CalculateBlockLines(const TextBlock& block);
};

// Individual content block
struct TextBlock {
    BlockType type;
    wxString text;                    // Plain text (for plain text mode)
    std::vector<StyledTextRun> runs;  // Styled runs for markdown rendering
    bool isLoading;
    bool rightToLeft;       // RTL text direction (Arabic, Hebrew, etc.)
    int lineCount;          // How many screen lines this block occupies
    int cachedLineHeight;
    int leftIndent = 0;     // Left indent in pixels (for blockquotes, lists)
    int topSpacing = 0;     // Extra spacing above
    int bottomSpacing = 0;  // Extra spacing below
    
    bool isCollapsed = false;   // Collapsible blocks (thinking) start collapsed

    TextBlock(BlockType t, const wxString& txt, bool rtl = false)
        : type(t), text(txt), isLoading(false), rightToLeft(rtl),
          lineCount(0), cachedLineHeight(0), isCollapsed(t == BlockType::THINKING) {}
    
    // Check if this block has styled runs (markdown mode)
    bool HasStyledRuns() const { return !runs.empty(); }
    
    // Get the full text (either from runs or plain text)
    wxString GetFullText() const {
        if (runs.empty()) return text;
        wxString result;
        for (const auto& run : runs) {
            result += run.text;
        }
        return result;
    }
};
