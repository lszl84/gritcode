#pragma once
#include <wx/frame.h>
#include <wx/textctrl.h>

// Read-only log window for debugging. Shows the raw request bodies and
// compaction activity. Hides on close instead of destroying so the
// ChatFrame pointer stays valid.
class DebugWindow : public wxFrame {
public:
    DebugWindow(wxWindow* parent);

    void SetText(const wxString& text);
    void Append(const wxString& text);

private:
    wxTextCtrl* log_ = nullptr;
    void OnClose(wxCloseEvent& e);
};
