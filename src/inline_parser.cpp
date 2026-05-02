#include "inline_parser.h"

namespace {

void Push(std::vector<InlineRun>& out, const wxString& text, bool b, bool i, bool c) {
    if (text.IsEmpty()) return;
    if (!out.empty() && out.back().bold == b && out.back().italic == i && out.back().code == c) {
        out.back().text += text;
    } else {
        out.push_back({text, b, i, c});
    }
}

}  // namespace

std::vector<InlineRun> ParseInlines(const wxString& src) {
    std::vector<InlineRun> out;
    bool bold = false, italic = false;
    size_t i = 0;
    const size_t n = src.size();
    wxString buf;

    auto flush = [&]() { Push(out, buf, bold, italic, false); buf.Clear(); };

    while (i < n) {
        wxUniChar ch = src[i];

        // Backslash escape: take the next char literally.
        if (ch == '\\' && i + 1 < n) {
            buf += src[i + 1];
            i += 2;
            continue;
        }

        // Inline code span: ` ... ` (non-nesting; takes precedence).
        if (ch == '`') {
            flush();
            size_t end = src.find('`', i + 1);
            if (end == wxString::npos) {
                buf += ch;
                ++i;
                continue;
            }
            wxString codeText = src.SubString(i + 1, end - 1);
            Push(out, codeText, false, false, true);
            i = end + 1;
            continue;
        }

        // Bold: ** or __
        if ((ch == '*' || ch == '_') && i + 1 < n && src[i + 1] == ch) {
            flush();
            bold = !bold;
            i += 2;
            continue;
        }

        // Italic: single * or _
        if (ch == '*' || ch == '_') {
            flush();
            italic = !italic;
            ++i;
            continue;
        }

        buf += ch;
        ++i;
    }
    flush();
    return out;
}
