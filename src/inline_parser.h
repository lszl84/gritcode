#pragma once
#include "block.h"
#include <wx/string.h>
#include <vector>

// Parse inline markdown markers into styled runs:
//   **bold**, __bold__, *italic*, _italic_, `code`
// Other markdown (links, images, strikethrough) is preserved as plain text.
// Markers themselves are stripped from `text`.
std::vector<InlineRun> ParseInlines(const wxString& src);
