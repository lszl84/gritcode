#include "thread_list.h"
#include "omarchy_theme.h"
#include <algorithm>
#include <wx/control.h>
#include <wx/dc.h>
#include <wx/settings.h>

ThreadList::ThreadList(wxWindow* parent)
    : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE) {
    // wxVListBox pins the list background it was created with, while the text
    // keeps following the theme: after a light/dark switch it would draw the
    // new theme's text on the old background. Take the current one again.
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent& e) {
        SetBackgroundColour(GetDefaultAttributes().colBg);
        e.Skip();  // the base handler repaints
    });
}

void ThreadList::SetRows(std::vector<ThreadRow> rows, const std::string& activeId) {
    rows_ = std::move(rows);
    SetItemCount(rows_.size());
    auto active = std::ranges::find(rows_, activeId, &ThreadRow::id);
    SetSelection(active != rows_.end() ? (int)(active - rows_.begin()) : wxNOT_FOUND);
    RefreshAll();
}

wxCoord ThreadList::OnMeasureItem(size_t) const {
    return 2 * GetCharHeight() + FromDIP(10);
}

void ThreadList::OnDrawItem(wxDC& dc, const wxRect& rect, size_t n) const {
    using enum ThreadRow::State;
    const ThreadRow& r = rows_[n];
    const bool selected = IsSelected(n);
    const wxColour bg = selected ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT)
                                 : GetBackgroundColour();
    const wxColour fg = selected ? wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHTTEXT)
                                 : GetForegroundColour();
    const wxColour dim = omarchy::Mix(fg, bg, 0.4);

    // Running is the theme's accent, a failure its red; both readable on bg.
    const omarchy::Theme* theme = omarchy::Current();
    wxColour accent = theme ? theme->Get("accent") : wxColour();
    if (!accent.IsOk()) accent = wxColour(60, 140, 230);
    wxColour red = theme ? theme->Get("red") : wxColour();
    if (!red.IsOk()) red = wxColour(215, 75, 60);

    dc.SetFont(GetFont());
    const int pad = FromDIP(6);
    const int markW = dc.GetTextExtent(wxString::FromUTF8("\xE2\x97\x8F ")).x;  // "● "
    int x = rect.x + pad + (r.child ? FromDIP(14) : 0);
    const int lineH = GetCharHeight();
    const int y = rect.y + (rect.height - 2 * lineH) / 2;

    if (r.state == Failed) {
        dc.SetTextForeground(selected ? fg : omarchy::Readable(red, bg, 3.0));
        dc.DrawText(wxString::FromUTF8("\xE2\x9A\xA0"), x, y);  // "⚠"
    } else if (r.state == Running || r.state == Unseen) {
        dc.SetTextForeground(selected ? fg : omarchy::Readable(accent, bg, 3.0));
        dc.DrawText(wxString::FromUTF8("\xE2\x97\x8F"), x, y);  // "●"
    }
    x += markW;
    const int textW = rect.GetRight() - pad - x;
    dc.SetTextForeground(fg);
    dc.DrawText(wxControl::Ellipsize(r.title, dc, wxELLIPSIZE_END, textW), x, y);
    dc.SetTextForeground(dim);
    dc.DrawText(wxControl::Ellipsize(r.detail, dc, wxELLIPSIZE_END, textW), x, y + lineH);
}
