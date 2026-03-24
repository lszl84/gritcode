#include "markdown_renderer.h"
#include <cmark.h>
#include <stack>

MarkdownRenderer::MarkdownRenderer(const wxFont& baseFont)
    : m_baseFont(baseFont)
{
}

wxFont MarkdownRenderer::GetBoldFont() const
{
    return wxFont(wxFontInfo(m_baseFont.GetPointSize()).Bold().Family(wxFONTFAMILY_DEFAULT));
}

wxFont MarkdownRenderer::GetItalicFont() const
{
    return wxFont(wxFontInfo(m_baseFont.GetPointSize()).Italic().Family(wxFONTFAMILY_DEFAULT));
}

wxFont MarkdownRenderer::GetBoldItalicFont() const
{
    return wxFont(wxFontInfo(m_baseFont.GetPointSize()).Bold().Italic().Family(wxFONTFAMILY_DEFAULT));
}

wxFont MarkdownRenderer::GetCodeFont() const
{
    return wxFont(wxFontInfo(m_baseFont.GetPointSize()).Family(wxFONTFAMILY_TELETYPE));
}

wxFont MarkdownRenderer::GetHeadingFont(int level) const
{
    int sizeIncrease = (7 - std::min(level, 6)) * 2; // H1: +12, H2: +10, etc.
    return wxFont(wxFontInfo(m_baseFont.GetPointSize() + sizeIncrease)
                  .Bold()
                  .Family(wxFONTFAMILY_DEFAULT));
}

wxColour MarkdownRenderer::GetTextColour(bool isDarkTheme) const
{
    return isDarkTheme ? wxColour(220, 220, 220) : wxColour(0, 0, 0);
}

wxColour MarkdownRenderer::GetHeadingColour(bool isDarkTheme) const
{
    return isDarkTheme ? wxColour(100, 150, 255) : wxColour(30, 60, 120);
}

wxColour MarkdownRenderer::GetCodeColour(bool isDarkTheme) const
{
    return isDarkTheme ? wxColour(255, 120, 120) : wxColour(180, 50, 30);
}

wxColour MarkdownRenderer::GetBlockquoteColour(bool isDarkTheme) const
{
    return isDarkTheme ? wxColour(160, 160, 160) : wxColour(100, 100, 100);
}

wxColour MarkdownRenderer::GetLinkColour(bool isDarkTheme) const
{
    return isDarkTheme ? wxColour(100, 180, 255) : wxColour(30, 90, 180);
}

// Internal state used while walking the cmark AST
struct MarkdownWalkState {
    const MarkdownRenderer* renderer;
    bool isDarkTheme;
    std::vector<std::unique_ptr<TextBlock>> blocks;
    TextBlock* currentBlock = nullptr;
    
    // Style state
    bool bold = false;
    bool italic = false;
    bool inCode = false;
    bool inCodeBlock = false;
    int headingLevel = 0;
    int blockquoteDepth = 0;
    int listDepth = 0;
    
    struct ListInfo {
        cmark_list_type type;
        int start;
        int itemIndex;
    };
    std::stack<ListInfo> listStack;
    
    MarkdownWalkState(const MarkdownRenderer* r, bool dark) 
        : renderer(r), isDarkTheme(dark) {}
    
    wxFont GetCurrentFont() const
    {
        if (inCode || inCodeBlock)
            return renderer->GetCodeFont();
        if (headingLevel > 0)
            return renderer->GetHeadingFont(headingLevel);
        if (bold && italic)
            return renderer->GetBoldItalicFont();
        if (bold)
            return renderer->GetBoldFont();
        if (italic)
            return renderer->GetItalicFont();
        return renderer->GetBaseFont();
    }
    
    wxColour GetCurrentColour() const
    {
        if (inCode || inCodeBlock)
            return renderer->GetCodeColour(isDarkTheme);
        if (headingLevel > 0)
            return renderer->GetHeadingColour(isDarkTheme);
        if (blockquoteDepth > 0)
            return renderer->GetBlockquoteColour(isDarkTheme);
        return renderer->GetTextColour(isDarkTheme);
    }
    
    void FlushBlock()
    {
        if (currentBlock && !currentBlock->runs.empty()) {
            currentBlock->text = currentBlock->GetFullText();
            blocks.push_back(std::unique_ptr<TextBlock>(currentBlock));
            currentBlock = nullptr;
        }
    }
    
    void EnsureBlock(BlockType type = BlockType::NORMAL)
    {
        if (!currentBlock) {
            currentBlock = new TextBlock(type, wxString());
            currentBlock->leftIndent = blockquoteDepth * 20 + listDepth * 24;
        }
    }
    
    void AddText(const wxString& text)
    {
        if (text.IsEmpty()) return;
        EnsureBlock();
        currentBlock->runs.emplace_back(text, GetCurrentFont(), GetCurrentColour());
    }
};

std::vector<std::unique_ptr<TextBlock>> MarkdownRenderer::Render(const std::string& markdown, bool isDarkTheme) const
{
    cmark_node* doc = cmark_parse_document(markdown.c_str(), markdown.size(), CMARK_OPT_DEFAULT);
    if (!doc)
        return {};
    
    MarkdownWalkState state(this, isDarkTheme);
    
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
            if (entering) {
                state.EnsureBlock();
            } else {
                state.FlushBlock();
            }
            break;
            
        case CMARK_NODE_HEADING:
            if (entering) {
                state.headingLevel = cmark_node_get_heading_level(node);
                state.EnsureBlock();
                state.currentBlock->topSpacing = (state.headingLevel <= 2) ? 12 : 6;
                state.currentBlock->bottomSpacing = 4;
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
                MarkdownWalkState::ListInfo li;
                li.type = cmark_node_get_list_type(node);
                li.start = cmark_node_get_list_start(node);
                li.itemIndex = li.start;
                state.listStack.push(li);
                state.listDepth++;
            } else {
                state.FlushBlock();
                if (!state.listStack.empty())
                    state.listStack.pop();
                state.listDepth--;
            }
            break;
            
        case CMARK_NODE_ITEM:
            if (entering) {
                state.FlushBlock();
                state.EnsureBlock();
                if (!state.listStack.empty()) {
                    auto& li = state.listStack.top();
                    wxString prefix;
                    if (li.type == CMARK_ORDERED_LIST) {
                        prefix = wxString::Format("%d. ", li.itemIndex++);
                    } else {
                        prefix = wxString::FromUTF8("\xe2\x80\xa2 "); // bullet
                    }
                    state.AddText(prefix);
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
                    wxString codeText = wxString::FromUTF8(content);
                    if (!codeText.IsEmpty() && codeText.Last() == '\n')
                        codeText.RemoveLast();
                    state.AddText(codeText);
                }
                state.FlushBlock();
                state.inCodeBlock = false;
            }
            break;
            
        case CMARK_NODE_THEMATIC_BREAK:
            if (entering) {
                state.FlushBlock();
                auto* hrBlock = new TextBlock(BlockType::NORMAL, wxString());
                hrBlock->runs.emplace_back(wxString(L'\x2500', 40), m_baseFont, 
                                           GetTextColour(isDarkTheme));
                state.blocks.push_back(std::unique_ptr<TextBlock>(hrBlock));
            }
            break;
            
        case CMARK_NODE_TEXT: {
            const char* content = cmark_node_get_literal(node);
            if (content)
                state.AddText(wxString::FromUTF8(content));
            break;
        }
            
        case CMARK_NODE_SOFTBREAK:
            state.AddText(" ");
            break;
            
        case CMARK_NODE_LINEBREAK:
            state.AddText("\n");
            break;
            
        case CMARK_NODE_CODE:
            if (entering) {
                state.inCode = true;
                const char* content = cmark_node_get_literal(node);
                if (content)
                    state.AddText(wxString::FromUTF8(content));
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
            // Links just render their text content with link color
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
                state.AddText(wxString::FromUTF8(content));
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
