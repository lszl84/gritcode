#include "inline_parser.h"

namespace {

void Push(std::vector<InlineRun>& out, const wxString& text, bool b, bool i, bool c,
          const wxString& link = {}) {
    if (text.IsEmpty()) return;
    if (!out.empty() && out.back().bold == b && out.back().italic == i
        && out.back().code == c && out.back().link == link) {
        out.back().text += text;
    } else {
        InlineRun r{text, b, i, c, link};
        out.push_back(r);
    }
}

// Returns true and fills `text` and `url` if `[text](url)` is found at pos i.
// Does NOT consume the input — caller advances i past the closing ')'.
bool TryParseLink(const wxString& src, size_t i, wxString& text, wxString& url) {
    if (src[i] != '[') return false;
    size_t j = i + 1;
    // Find the matching ']' — handle escaped brackets inside the text.
    int depth = 1;
    while (j < src.size() && depth > 0) {
        if (src[j] == '\\' && j + 1 < src.size()) { j += 2; continue; }
        if (src[j] == '[') ++depth;
        if (src[j] == ']') --depth;
        ++j;
    }
    if (depth != 0) return false;  // unmatched [
    size_t closeB = j - 1;
    // Must be immediately followed by '('.
    if (closeB + 1 >= src.size() || src[closeB + 1] != '(') return false;
    // Find the matching ')' — handle nested parens.
    size_t k = closeB + 2;
    int parenDepth = 1;
    while (k < src.size() && parenDepth > 0) {
        if (src[k] == '\\' && k + 1 < src.size()) { k += 2; continue; }
        if (src[k] == '(') ++parenDepth;
        if (src[k] == ')') --parenDepth;
        ++k;
    }
    if (parenDepth != 0) return false;  // unmatched (
    size_t closeP = k - 1;
    text = src.SubString(i + 1, closeB - 1);
    url  = src.SubString(closeB + 2, closeP - 1);
    return true;
}

}  // namespace

std::vector<InlineRun> ParseInlines(const wxString& src) {
    std::vector<InlineRun> out;
    bool bold = false, italic = false;
    size_t i = 0;
    const size_t n = src.size();
    wxString buf;

    auto flushPlain = [&]() { Push(out, buf, bold, italic, false); buf.Clear(); };

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
            flushPlain();
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
            flushPlain();
            bold = !bold;
            i += 2;
            continue;
        }

        // Italic: single * or _
        if (ch == '*' || ch == '_') {
            flushPlain();
            italic = !italic;
            ++i;
            continue;
        }

        // Link: [text](url). Parse first, then inline-parse the text body
        // inside the brackets so **bold** etc. inside link text works.
        wxString linkText, linkUrl;
        if (TryParseLink(src, i, linkText, linkUrl)) {
            flushPlain();
            // Recursively parse the link text for bold/italic/code inside.
            auto inner = ParseInlines(linkText);
            for (auto& r : inner) {
                r.link = linkUrl;
            }
            out.insert(out.end(), inner.begin(), inner.end());
            // Eat the whole [text](url) sequence.
            i += linkText.size() + linkUrl.size() + 4;  // [ ] ( )
            continue;
        }

        buf += ch;
        ++i;
    }
    flushPlain();
    return out;
}
