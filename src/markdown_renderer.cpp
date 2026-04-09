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

} // namespace

std::vector<std::unique_ptr<TextBlock>> MarkdownRenderer::Render(
    const std::string& markdown, bool isDarkTheme) const {
    cmark_node* doc = cmark_parse_document(markdown.c_str(), markdown.size(), CMARK_OPT_DEFAULT);
    if (!doc) return {};

    WalkState state(this, isDarkTheme);
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
                std::string line(40, '\xe2');
                // U+2500 box drawing horizontal
                line.clear();
                for (int i = 0; i < 40; i++) line += "\xe2\x94\x80";
                hr->runs.push_back({line, FontStyle::Regular, TextColour(isDarkTheme)});
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
    cmark_node_free(doc);

    return std::move(state.blocks);
}
