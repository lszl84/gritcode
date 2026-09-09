#pragma once
// Pure (GUI-free) indentation helpers for the editor's Tab / Enter / Backspace
// handling. Kept in a header so they can be unit-tested without a wxTextCtrl.

#include <wx/string.h>

namespace editor_indent {

// Number of spaces per indentation level in the editor.
constexpr size_t kIndentWidth = 4;

// Leading whitespace of the line containing `pos` in `text`. The result is
// capped at `pos`, so it reflects the indentation the cursor sits in rather
// than the whole line's indentation.
inline wxString LineLeadingIndent(const wxString& text, long pos) {
    if (pos < 0) pos = 0;
    if (pos > (long)text.length()) pos = (long)text.length();
    long start = pos;
    while (start > 0 && text[start - 1] != '\n') start--;
    long i = start;
    while (i < pos && (text[i] == ' ' || text[i] == '\t')) i++;
    return text.Mid(start, i - start);
}

// Number of leading spaces to delete when Backspace is pressed at `pos`.
// Returns 0 when `pos` is not inside a plain-space indent (i.e. the line has
// text or tabs before the cursor), in which case the control's normal
// single-character backspace should run instead.
inline long BackspaceIndentCount(const wxString& text, long pos) {
    if (pos <= 0 || pos > (long)text.length()) return 0;
    long start = pos;
    while (start > 0 && text[start - 1] != '\n') start--;
    for (long j = start; j < pos; ++j) {
        if (text[j] != ' ') return 0;
    }
    long len = pos - start;
    if (len == 0) return 0;
    long n = len % (long)kIndentWidth;
    return n == 0 ? (long)kIndentWidth : n;
}

}  // namespace editor_indent
