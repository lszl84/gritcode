#pragma once
#include <wx/string.h>
#include <vector>

enum class BlockType {
    Paragraph,
    Heading,
    CodeBlock,
    UserPrompt,
    Table,
    ToolCall,
};

enum class TableAlign { Left, Center, Right };

// Inline styling — produced by the inline parser when a block is committed.
// Markdown markers are stripped; `text` is the visible substring.
struct InlineRun {
    wxString text;
    bool bold   = false;
    bool italic = false;
    bool code   = false;
};

// One visual line after wrapping. `runs` carry the styling for paint;
// `text` is the concatenated visible text used for selection/copy. Each run
// also stores its `xOffset` within the line so paint can position it without
// re-measuring.
struct WrappedLine {
    wxString text;
    std::vector<InlineRun> runs;
    std::vector<int> runX;          // x of each run's start (line-local)
    std::vector<int> glyphX;        // x of each char boundary in `text`, size = text.size()+1
    int height = 0;
    // Range of visible chars (in the block's visibleText) that this line covers.
    int textStart = 0;
    int textEnd   = 0;
};

// One cell in a Table block. Each cell parses its own inline markdown and
// holds its own wrapped layout so column widths can drive per-cell wrap.
struct TableCell {
    wxString rawText;
    std::vector<InlineRun> runs;
    wxString visibleText;
    std::vector<WrappedLine> lines;   // wrapped at the column's text width
    int height = 0;                   // measured cell content height
};

struct Block {
    BlockType type = BlockType::Paragraph;
    int headingLevel = 0;       // 1..6 for Heading
    wxString lang;              // for CodeBlock
    wxString rawText;           // markdown source (for CodeBlock = code body verbatim)
    std::vector<InlineRun> runs;  // populated for Paragraph/Heading/UserPrompt
    wxString visibleText;       // concatenation of all runs' text — used for selection/copy

    // Layout cache. Invalidated by setting cachedWidth = -1.
    int cachedWidth = -1;
    int cachedHeight = 0;
    std::vector<WrappedLine> lines;

    // Table-specific. tableRows[0] is the header row.
    std::vector<std::vector<TableCell>> tableRows;
    std::vector<TableAlign> tableAligns;
    std::vector<int> tableColX;       // x of each column's left edge (block-local), size = ncols+1
    std::vector<int> tableRowY;       // y of each row's top edge (block-local), size = nrows+1

    // ToolCall-specific. Header is always shown; body is shown only when expanded.
    wxString toolName;
    wxString toolArgs;       // compact JSON of arguments
    wxString toolResult;     // captured result text (already capped)
    bool toolExpanded = false;
    int toolHeaderH = 0;     // measured height of the click-target header row

    // Header paint cache (filled in LayoutBlock so PaintBlock is GetTextExtent-free).
    wxString toolChevStr;    // pre-formatted chevron + trailing space
    int toolChevW = 0;       // pixel width of the chevron string (with the trailing space)
    int toolNameW = 0;       // pixel width of toolName at the code font
    wxString toolArgsFit;    // truncated args display ("(args)" or "(prefix…)") that fits
    int toolArgsX = 0;       // block-local x where toolArgsFit starts
    wxString toolHint;       // right-aligned hint string ("· N lines" / "· ok") — collapsed only
    int toolHintW = 0;       // pixel width of the hint string
};

// (blockIndex, charOffset-in-visibleText). Stable across reflows because both
// fields are content-relative, not visual.
struct BlockPos {
    int block = -1;
    int offset = 0;
    bool IsValid() const { return block >= 0; }
    bool operator==(const BlockPos& o) const { return block == o.block && offset == o.offset; }
    bool operator<(const BlockPos& o) const {
        if (block != o.block) return block < o.block;
        return offset < o.offset;
    }
    bool operator<=(const BlockPos& o) const { return !(o < *this); }
};
