#pragma once
#include <wx/string.h>
#include <format>
#include <utility>

// wxString::Format routes its const char* format string through wxConvLibc
// (the C locale), so any non-ASCII byte in either the format string or the
// arguments segfaults wxFormatConverterBase::Convert when the runtime locale
// isn't UTF-8. Build the bytes with std::format and wrap once at the boundary
// via FromUTF8, which is locale-agnostic.
template <typename... Args>
inline wxString FormatU8(std::format_string<Args...> fmt, Args&&... args) {
    return wxString::FromUTF8(std::format(fmt, std::forward<Args>(args)...));
}
