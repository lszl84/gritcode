#pragma once
#include "types.h"
#include <vector>
#include <memory>
#include <string>

class MarkdownRenderer {
public:
    explicit MarkdownRenderer(int baseSizePt = 14);

    std::vector<std::unique_ptr<TextBlock>> Render(const std::string& markdown,
                                                    bool isDarkTheme = false) const;

    static Color TextColour(bool dark);
    static Color HeadingColour(bool dark);
    static Color CodeColour(bool dark);
    static Color BlockquoteColour(bool dark);
    static Color LinkColour(bool dark);

private:
    int baseSizePt_;
};
