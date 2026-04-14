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

#include "markdown_renderer.h"
#include <cmark.h>
#include <stack>
#include <cstring>

MarkdownRenderer::MarkdownRenderer(int baseSizePt) : baseSizePt_(baseSizePt) {}

Color MarkdownRenderer::TextColour(bool dark) {
    return dark ? Color::RGB(220, 220, 220) : Color::RGB(0, 0, 0);
}
Color MarkdownRenderer::HeadingColour(bool dark) {
    return dark ? Color::RGB(100, 150, 255) : Color::RGB(30, 60, 120);
}
Color MarkdownRenderer::CodeColour(bool dark) {
    return dark ? Color::RGB(200, 180, 140) : Color::RGB(120, 80, 30);
}
Color MarkdownRenderer::BlockquoteColour(bool dark) {
    return dark ? Color::RGB(160, 160, 160) : Color::RGB(100, 100, 100);
}
Color MarkdownRenderer::LinkColour(bool dark) {
    return dark ? Color::RGB(100, 180, 255) : Color::RGB(30, 90, 180);
}

namespace {

FontStyle HeadingStyle(int level) {
    switch (level) {
    case 1: return FontStyle::Heading1;
    case 2: return FontStyle::Heading2;
    case 3: return FontStyle::Heading3;
    case 4: return FontStyle::Heading4;
    case 5: return FontStyle::Heading5;
    default: return FontStyle::Heading6;
    }
}

struct WalkState {
    const MarkdownRenderer* renderer;
    bool isDarkTheme;
    std::vector<std::unique_ptr<TextBlock>> blocks;
    TextBlock* current = nullptr;

    bool bold = false, italic = false, inCode = false, inCodeBlock = false;
    int headingLevel = 0, blockquoteDepth = 0, listDepth = 0;

    struct ListInfo {
        cmark_list_type type;
        int start, itemIndex;
    };
    std::stack<ListInfo> listStack;

    WalkState(const MarkdownRenderer* r, bool dark) : renderer(r), isDarkTheme(dark) {}

    FontStyle CurrentStyle() const {
        if (inCode || inCodeBlock) return FontStyle::Code;
        if (headingLevel > 0) return HeadingStyle(headingLevel);
        if (bold && italic) return FontStyle::BoldItalic;
        if (bold) return FontStyle::Bold;
        if (italic) return FontStyle::Italic;
        return FontStyle::Regular;
    }

    Color CurrentColour() const {
        if (inCode || inCodeBlock) return MarkdownRenderer::CodeColour(isDarkTheme);
        if (headingLevel > 0) return MarkdownRenderer::HeadingColour(isDarkTheme);
        if (blockquoteDepth > 0) return MarkdownRenderer::BlockquoteColour(isDarkTheme);
        return MarkdownRenderer::TextColour(isDarkTheme);
    }

    void FlushBlock() {
        if (current && !current->runs.empty()) {
            current->text = current->GetFullText();
            blocks.push_back(std::unique_ptr<TextBlock>(current));
            current = nullptr;
        } else if (current) {
            delete current;
            current = nullptr;
        }
    }

    void EnsureBlock(BlockType type = BlockType::NORMAL) {
        if (!current) {
            current = new TextBlock(type, "");
            current->isCollapsed = false;
            current->leftIndent = blockquoteDepth * 20 + listDepth * 24;
        }
    }

    void AddText(const std::string& text) {
        if (text.empty()) return;
        EnsureBlock();
        current->runs.push_back({text, CurrentStyle(), CurrentColour()});
    }
};

// Walk a cmark document into the shared WalkState. Used both for the main
// non-table markdown regions and — with a fresh state — for parsing the
// inline formatting inside a single table cell.
void WalkDocument(cmark_node* doc, WalkState& state, bool isDarkTheme) {
    cmark_iter* iter = cmark_iter_new(doc);
    cmark_event_type evType;

    while ((evType = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cmark_node* node = cmark_iter_get_node(iter);
        cmark_node_type nodeType = cmark_node_get_type(node);
        bool entering = (evType == CMARK_EVENT_ENTER);

        switch (nodeType) {
        case CMARK_NODE_DOCUMENT:
            break;

        case CMARK_NODE_PARAGRAPH:
            if (entering)
                state.EnsureBlock();
            else
                state.FlushBlock();
            break;

        case CMARK_NODE_HEADING:
            if (entering) {
                state.headingLevel = cmark_node_get_heading_level(node);
                state.EnsureBlock();
                state.current->topSpacing = (state.headingLevel <= 2) ? 12 : 6;
                state.current->bottomSpacing = 4;
            } else {
                state.FlushBlock();
                state.headingLevel = 0;
            }
            break;

        case CMARK_NODE_BLOCK_QUOTE:
            if (entering) {
                state.FlushBlock();
                state.blockquoteDepth++;
            } else {
                state.FlushBlock();
                state.blockquoteDepth--;
            }
            break;

        case CMARK_NODE_LIST:
            if (entering) {
                state.FlushBlock();
                WalkState::ListInfo li;
                li.type = cmark_node_get_list_type(node);
                li.start = cmark_node_get_list_start(node);
                li.itemIndex = li.start;
                state.listStack.push(li);
                state.listDepth++;
            } else {
                state.FlushBlock();
                if (!state.listStack.empty()) state.listStack.pop();
                state.listDepth--;
            }
            break;

        case CMARK_NODE_ITEM:
            if (entering) {
                state.FlushBlock();
                state.EnsureBlock();
                if (!state.listStack.empty()) {
                    auto& li = state.listStack.top();
                    if (li.type == CMARK_ORDERED_LIST) {
                        state.AddText(std::to_string(li.itemIndex++) + ". ");
                    } else {
                        state.AddText("\xe2\x80\xa2 ");
                    }
                }
            } else {
                state.FlushBlock();
            }
            break;

        case CMARK_NODE_CODE_BLOCK:
            if (entering) {
                state.FlushBlock();
                state.inCodeBlock = true;
                state.EnsureBlock(BlockType::CODE);
                const char* content = cmark_node_get_literal(node);
                if (content) {
                    std::string code(content);
                    if (!code.empty() && code.back() == '\n') code.pop_back();
                    state.AddText(code);
                }
                state.FlushBlock();
                state.inCodeBlock = false;
            }
            break;

        case CMARK_NODE_THEMATIC_BREAK:
            if (entering) {
                state.FlushBlock();
                auto* hr = new TextBlock(BlockType::NORMAL, "");
                hr->isCollapsed = false;
                std::string line;
                for (int i = 0; i < 40; i++) line += "\xe2\x94\x80";  // U+2500
                hr->runs.push_back({line, FontStyle::Regular, MarkdownRenderer::TextColour(isDarkTheme)});
                hr->text = hr->GetFullText();
                state.blocks.push_back(std::unique_ptr<TextBlock>(hr));
            }
            break;

        case CMARK_NODE_TEXT: {
            const char* content = cmark_node_get_literal(node);
            if (content) state.AddText(content);
            break;
        }

        case CMARK_NODE_SOFTBREAK:
            state.AddText("\n");
            break;

        case CMARK_NODE_LINEBREAK:
            state.AddText("\n");
            break;

        case CMARK_NODE_CODE:
            if (entering) {
                state.inCode = true;
                const char* content = cmark_node_get_literal(node);
                if (content) state.AddText(content);
                state.inCode = false;
            }
            break;

        case CMARK_NODE_EMPH:
            state.italic = entering;
            break;

        case CMARK_NODE_STRONG:
            state.bold = entering;
            break;

        case CMARK_NODE_LINK:
            break;

        case CMARK_NODE_IMAGE:
            if (entering) {
                state.AddText("[image]");
                cmark_iter_reset(iter, node, CMARK_EVENT_EXIT);
            }
            break;

        case CMARK_NODE_HTML_BLOCK:
        case CMARK_NODE_HTML_INLINE: {
            const char* content = cmark_node_get_literal(node);
            if (content && entering) {
                state.inCode = true;
                state.AddText(content);
                state.inCode = false;
            }
            break;
        }

        default:
            break;
        }
    }

    state.FlushBlock();
    cmark_iter_free(iter);
}

// Parse a chunk of markdown through cmark and return the resulting blocks.
// Used for every region of the input that isn't a GFM table.
std::vector<std::unique_ptr<TextBlock>> RenderNonTable(const MarkdownRenderer* renderer,
                                                       const std::string& markdown,
                                                       bool isDarkTheme) {
    if (markdown.empty()) return {};
    cmark_node* doc = cmark_parse_document(markdown.c_str(), markdown.size(), CMARK_OPT_DEFAULT);
    if (!doc) return {};

    WalkState state(renderer, isDarkTheme);
    WalkDocument(doc, state, isDarkTheme);
    cmark_node_free(doc);
    return std::move(state.blocks);
}

// Parse the inline formatting inside a single table cell. The cell's text is
// fed to cmark as a one-paragraph document; the first resulting block's runs
// are what we want. Falls back to a single plain-text run on anything weird.
std::vector<StyledTextRun> ParseCellRuns(const std::string& cellText, bool isDarkTheme) {
    if (cellText.empty()) return {};
    cmark_node* doc = cmark_parse_document(cellText.c_str(), cellText.size(), CMARK_OPT_DEFAULT);
    if (!doc) {
        return {{cellText, FontStyle::Regular, MarkdownRenderer::TextColour(isDarkTheme)}};
    }
    WalkState state(nullptr, isDarkTheme);
    WalkDocument(doc, state, isDarkTheme);
    cmark_node_free(doc);

    if (state.blocks.empty() || state.blocks[0]->runs.empty()) {
        return {{cellText, FontStyle::Regular, MarkdownRenderer::TextColour(isDarkTheme)}};
    }
    return std::move(state.blocks[0]->runs);
}

// --- GFM table detection + parsing ----------------------------------------
//
// We hand-parse GFM tables before cmark ever sees them. Base cmark treats the
// whole thing as a paragraph with pipes and dashes as literal text, so to get
// a table block out we need to identify the span and pull it aside.
//
// The parser is intentionally tolerant: leading/trailing pipes optional,
// escaped pipes (\|) survive the cell split, alignment colons on the delimiter
// row drive per-column alignment, and rows with a wrong number of cells are
// padded or truncated rather than rejected.

std::vector<std::string> SplitInputIntoLines(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

// Trim leading/trailing ASCII whitespace (and \r).
std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

// Split a table row on unescaped `|`. Strips one leading and one trailing
// pipe (GFM tolerates either). Returns cell strings with outer whitespace
// trimmed, backslash-pipe escapes unescaped to a literal pipe.
std::vector<std::string> SplitTableRow(const std::string& line) {
    std::string trimmed = Trim(line);
    size_t start = 0;
    size_t end = trimmed.size();
    if (start < end && trimmed[start] == '|') start++;
    if (end > start && trimmed[end - 1] == '|') {
        // Only strip a trailing pipe if it's not escaped.
        if (end - start < 2 || trimmed[end - 2] != '\\') end--;
    }

    std::vector<std::string> cells;
    std::string cur;
    for (size_t i = start; i < end; i++) {
        char c = trimmed[i];
        if (c == '\\' && i + 1 < end && trimmed[i + 1] == '|') {
            cur += '|';
            i++;
        } else if (c == '|') {
            cells.push_back(Trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    cells.push_back(Trim(cur));
    return cells;
}

// Test whether a line can be a GFM delimiter row and, if so, report its
// per-column alignment into `alignOut`. Each cell must be non-empty and
// contain only dashes with optional leading/trailing `:`.
bool IsDelimiterRow(const std::string& line, std::vector<int>& alignOut) {
    std::string t = Trim(line);
    if (t.empty()) return false;
    // Must contain a pipe OR at least match the --- pattern between pipes.
    // Strip outer pipes.
    size_t a = 0, b = t.size();
    if (a < b && t[a] == '|') a++;
    if (b > a && t[b - 1] == '|') b--;
    if (b <= a) return false;

    alignOut.clear();
    std::string inner = t.substr(a, b - a);
    std::string cur;
    auto process = [&](const std::string& cell) -> bool {
        std::string c = Trim(cell);
        if (c.empty()) return false;
        bool leftColon = (c.front() == ':');
        bool rightColon = (c.back() == ':');
        size_t s = leftColon ? 1 : 0;
        size_t e = rightColon ? c.size() - 1 : c.size();
        if (e <= s) return false;
        for (size_t i = s; i < e; i++) if (c[i] != '-') return false;
        int al = -1;
        if (leftColon && rightColon) al = 0;
        else if (rightColon) al = 1;
        else al = -1;
        alignOut.push_back(al);
        return true;
    };
    for (size_t i = 0; i < inner.size(); i++) {
        if (inner[i] == '|') {
            if (!process(cur)) return false;
            cur.clear();
        } else {
            cur += inner[i];
        }
    }
    if (!process(cur)) return false;
    return !alignOut.empty();
}

// A line is "table-shaped" if after trimming it contains at least one pipe
// outside of backticks. This is the gate used to grow body rows.
bool LineLooksTabular(const std::string& line) {
    std::string t = Trim(line);
    if (t.empty()) return false;
    bool inCode = false;
    for (size_t i = 0; i < t.size(); i++) {
        char c = t[i];
        if (c == '`') { inCode = !inCode; continue; }
        if (c == '\\' && i + 1 < t.size()) { i++; continue; }
        if (c == '|' && !inCode) return true;
    }
    return false;
}

// Try to parse a GFM table starting at `startIdx`. On success fills `outBlock`
// with a TABLE-typed TextBlock and sets `endIdx` to the first line after the
// table. On failure, returns false and leaves outputs untouched.
bool TryParseTable(const std::vector<std::string>& lines,
                   size_t startIdx,
                   size_t& endIdx,
                   std::unique_ptr<TextBlock>& outBlock,
                   bool isDarkTheme) {
    if (startIdx + 1 >= lines.size()) return false;
    const std::string& headerLine = lines[startIdx];
    const std::string& delimLine = lines[startIdx + 1];
    if (!LineLooksTabular(headerLine)) return false;

    std::vector<int> alignment;
    if (!IsDelimiterRow(delimLine, alignment)) return false;

    auto headerCells = SplitTableRow(headerLine);
    if (headerCells.empty()) return false;

    int numCols = (int)headerCells.size();
    // Normalize alignment length to numCols: pad left-aligned or truncate.
    while ((int)alignment.size() < numCols) alignment.push_back(-1);
    if ((int)alignment.size() > numCols) alignment.resize(numCols);

    auto block = std::make_unique<TextBlock>(BlockType::TABLE, "");
    block->tableCols = numCols;
    block->tableAlign = std::move(alignment);

    auto addRow = [&](const std::vector<std::string>& cells) {
        std::vector<TableCell> row;
        row.reserve(numCols);
        for (int c = 0; c < numCols; c++) {
            TableCell cell;
            if (c < (int)cells.size()) {
                cell.runs = ParseCellRuns(cells[c], isDarkTheme);
            }
            row.push_back(std::move(cell));
        }
        block->tableRows.push_back(std::move(row));
    };

    addRow(headerCells);

    size_t i = startIdx + 2;
    while (i < lines.size()) {
        const std::string& ln = lines[i];
        if (Trim(ln).empty()) break;
        if (!LineLooksTabular(ln)) break;
        addRow(SplitTableRow(ln));
        i++;
    }

    endIdx = i;
    outBlock = std::move(block);
    return true;
}

}  // namespace

std::vector<std::unique_ptr<TextBlock>> MarkdownRenderer::Render(
    const std::string& markdown, bool isDarkTheme) const {
    // Fast path: no pipes anywhere → no tables possible, one cmark call.
    if (markdown.find('|') == std::string::npos) {
        return RenderNonTable(this, markdown, isDarkTheme);
    }

    auto lines = SplitInputIntoLines(markdown);
    std::vector<std::unique_ptr<TextBlock>> blocks;
    std::string buffer;

    auto flushBuffer = [&]() {
        if (buffer.empty()) return;
        auto sub = RenderNonTable(this, buffer, isDarkTheme);
        for (auto& b : sub) blocks.push_back(std::move(b));
        buffer.clear();
    };

    size_t i = 0;
    while (i < lines.size()) {
        size_t tableEnd = 0;
        std::unique_ptr<TextBlock> tableBlock;
        if (TryParseTable(lines, i, tableEnd, tableBlock, isDarkTheme)) {
            flushBuffer();
            blocks.push_back(std::move(tableBlock));
            i = tableEnd;
            continue;
        }
        buffer += lines[i];
        buffer += '\n';
        i++;
    }
    flushBuffer();

    return blocks;
}
