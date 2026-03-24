#pragma once

#include "block_manager.h"
#include <wx/wx.h>
#include <vector>
#include <string>

// Parses a markdown string using cmark and produces styled TextBlocks.
class MarkdownRenderer
{
public:
    explicit MarkdownRenderer(const wxFont& baseFont);

    // Parse markdown text and return styled blocks
    // If isDarkTheme is true, uses light text colors suitable for dark backgrounds
    std::vector<std::unique_ptr<TextBlock>> Render(const std::string& markdown, bool isDarkTheme = false) const;

    // Create fonts for different markdown elements
    wxFont GetBoldFont() const;
    wxFont GetItalicFont() const;
    wxFont GetBoldItalicFont() const;
    wxFont GetCodeFont() const;
    wxFont GetHeadingFont(int level) const;
    
    // Get colors for light/dark themes
    wxColour GetTextColour(bool isDarkTheme) const;
    wxColour GetHeadingColour(bool isDarkTheme) const;
    wxColour GetCodeColour(bool isDarkTheme) const;
    wxColour GetBlockquoteColour(bool isDarkTheme) const;
    wxColour GetLinkColour(bool isDarkTheme) const;
    
    // Base font accessor
    const wxFont& GetBaseFont() const { return m_baseFont; }
    
private:
    wxFont m_baseFont;
};
