#include "settings_dialog.h"
#include "preferences.h"
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/hyperlink.h>
#include <wx/msgdlg.h>

SettingsDialog::SettingsDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Settings",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {

    auto* outer = new wxBoxSizer(wxVERTICAL);

    // ---- DeepSeek section ----
    auto* heading = new wxStaticText(this, wxID_ANY, "DeepSeek");
    wxFont hf = heading->GetFont();
    hf.MakeBold();
    heading->SetFont(hf);
    outer->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    auto* keyLabel = new wxStaticText(this, wxID_ANY, "API key:");
    outer->Add(keyLabel, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    auto* keyRow = new wxBoxSizer(wxHORIZONTAL);
    // Pre-fill with the existing key so the user can see they have one set
    // (masked) and can edit it. Show toggle reveals plaintext.
    wxString existing = Preferences::GetApiKey(Preferences::Provider::DeepSeek);
    keyCtrl_ = new wxTextCtrl(this, wxID_ANY, existing,
                              wxDefaultPosition, FromDIP(wxSize(380, -1)),
                              wxTE_PASSWORD);
    showCb_ = new wxCheckBox(this, wxID_ANY, "Show");
    keyRow->Add(keyCtrl_, 1, wxALIGN_CENTER_VERTICAL);
    keyRow->Add(showCb_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    outer->Add(keyRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    auto* link = new wxHyperlinkCtrl(this, wxID_ANY,
        "Get an API key at platform.deepseek.com",
        "https://platform.deepseek.com/");
    outer->Add(link, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    auto* hint = new wxStaticText(this, wxID_ANY,
        "Stored securely in your system keyring.");
    wxFont smaller = hint->GetFont();
    smaller.SetPointSize(smaller.GetPointSize() - 1);
    hint->SetFont(smaller);
    outer->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    outer->AddStretchSpacer(1);

    // ---- Buttons ----
    auto* btns = CreateButtonSizer(wxOK | wxCANCEL);
    if (btns) outer->Add(btns, 0, wxEXPAND | wxALL, 12);

    SetSizerAndFit(outer);
    SetMinSize(GetSize());

    Bind(wxEVT_BUTTON, &SettingsDialog::OnSave, this, wxID_OK);
    showCb_->Bind(wxEVT_CHECKBOX, &SettingsDialog::OnToggleShow, this);
}

void SettingsDialog::OnToggleShow(wxCommandEvent&) {
    // wx 3.2 doesn't have a runtime toggle for wxTE_PASSWORD; recreate the
    // control with the new style and copy the value across.
    wxString cur = keyCtrl_->GetValue();
    long style = wxTE_PROCESS_ENTER;
    if (!showCb_->IsChecked()) style |= wxTE_PASSWORD;

    auto* sizer = keyCtrl_->GetContainingSizer();
    auto* newCtrl = new wxTextCtrl(this, wxID_ANY, cur,
                                   wxDefaultPosition, keyCtrl_->GetSize(),
                                   style);
    sizer->Replace(keyCtrl_, newCtrl);
    keyCtrl_->Destroy();
    keyCtrl_ = newCtrl;
    Layout();
}

void SettingsDialog::OnSave(wxCommandEvent& evt) {
    wxString key = keyCtrl_->GetValue();
    key.Trim().Trim(false);
    if (!Preferences::SetApiKey(Preferences::Provider::DeepSeek, key)) {
        wxMessageBox(
            "Could not save the API key to the system keyring.\n\n"
            "If you're running headless or without a keyring daemon, the "
            "secret store may be unavailable.",
            "wx_gritcode", wxOK | wxICON_ERROR, this);
        return;  // keep dialog open
    }
    evt.Skip();  // let default handler close with wxID_OK
}
