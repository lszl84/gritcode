#pragma once

#include <string_view>
#include <wx/string.h>

class wxTextCtrl;

namespace syntax {

// Re-apply syntax highlighting to `ctrl` for the document whose bytes are
// `utf8Text`. `path` selects the language by file extension. Unknown
// extensions and oversized documents are reset to the default style.
void Highlight(wxTextCtrl* ctrl, const wxString& path,
               std::string_view utf8Text);

// Reset the whole control to the default text style (no colouring).
void ClearStyles(wxTextCtrl* ctrl);

}  // namespace syntax
