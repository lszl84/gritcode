#include "debug_window.h"
#include <wx/sizer.h>

DebugWindow::DebugWindow(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, "gritcode - debug log",
              wxDefaultPosition, wxSize(780, 560)) {
    log_ = new wxTextCtrl(this, wxID_ANY, "",
                          wxDefaultPosition, wxDefaultSize,
                          wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH);
    log_->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(log_, 1, wxEXPAND);
    SetSizer(sizer);

    Bind(wxEVT_CLOSE_WINDOW, &DebugWindow::OnClose, this);
}

void DebugWindow::SetText(const wxString& text) {
    log_->SetValue(text);
    log_->ShowPosition(log_->GetLastPosition());
}

void DebugWindow::Append(const wxString& text) {
    log_->AppendText(text);
    log_->AppendText("\n");
    log_->ShowPosition(log_->GetLastPosition());
}

void DebugWindow::OnClose(wxCloseEvent& e) {
    Hide();
}
