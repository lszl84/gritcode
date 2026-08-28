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
    // (masked) and can edit it.  Show toggle reveals plaintext.
    wxString existing = Preferences::GetApiKey(Preferences::Provider::DeepSeek);
    if (existing.IsEmpty()) {
        existing = Preferences::GetApiKeyPlaintext(
            Preferences::Provider::DeepSeek);
    }
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

    // Snapshot the keyring health so the hint is consistent for the
    // lifetime of this dialog instance (even if the daemon state changes
    // mid-session, which it normally shouldn't).
    keyringWasBroken_ = Preferences::IsKeyringBroken();

    hint_ = new wxStaticText(this, wxID_ANY, "");
    wxFont smaller = hint_->GetFont();
    smaller.SetPointSize(smaller.GetPointSize() - 1);
    hint_->SetFont(smaller);

    if (keyringWasBroken_) {
        hint_->SetLabel(
            "System keyring not fully initialized - "
            "known issue on some Debian-based systems on first login.\n"
            "Keys will be stored in the application settings file\n"
            "(~/.gritcode/gritcode.conf) in plaintext.");
    } else {
        hint_->SetLabel("Stored securely in your system keyring.");
    }
    outer->Add(hint_, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    // ---- Agent tools section ----
    auto* toolsHeading = new wxStaticText(this, wxID_ANY, "Agent tools");
    wxFont thf = toolsHeading->GetFont();
    thf.MakeBold();
    toolsHeading->SetFont(thf);
    outer->Add(toolsHeading, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    gritHistoryCb_ = new wxCheckBox(this, wxID_ANY,
        "Enable Grit History tools");
    gritHistoryCb_->SetValue(Preferences::GetEnableGritHistory());
    outer->Add(gritHistoryCb_, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    auto* toolsHint = new wxStaticText(this, wxID_ANY,
        "When enabled, the agent can search your past gritcode sessions "
        "across projects (grit_history_search/fetch).\n"
        "Turn this off to create self-contained sessions that can be "
        "exported and shared without referencing your other work.");
    wxFont th = toolsHint->GetFont();
    th.SetPointSize(th.GetPointSize() - 1);
    toolsHint->SetFont(th);
    outer->Add(toolsHint, 0, wxLEFT | wxRIGHT | wxTOP, 4);

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

    auto provider = Preferences::Provider::DeepSeek;

    if (keyringWasBroken_ && !key.IsEmpty()) {
        // The keyring daemon is in the known broken-first-launch state.
        // Warn the user and offer to store in plaintext instead.
        int answer = wxMessageBox(
            "The system keyring (gnome-keyring) is not fully initialized.\n\n"
            "This is a known issue on some Debian-based systems after a fresh "
            "install -\n"
            "the first login doesn't unlock the keyring, and the unlock "
            "prompt never appears.\n\n"
            "A system reboot fixes it, but until then the API key cannot "
            "be stored\n"
            "in the keyring.\n\n"
            "Store the key in gritcode's settings file as plaintext instead?\n"
            "(~/.gritcode/gritcode.conf)\n\n"
            "Alternatively, cancel and reboot your system to clear the issue "
            "at\n"
            "the source - after a reboot the keyring will work normally.",
            "Keyring not available",
            wxYES_NO | wxICON_WARNING, this);
        if (answer != wxYES) return;  // keep dialog open

        // User accepted the plaintext fallback.
        if (!Preferences::SetApiKeyPlaintext(provider, key)) {
            wxMessageBox(
                "Could not write the API key to the settings file.",
                "gritcode", wxOK | wxICON_ERROR, this);
            return;
        }
        // Update the hint to reflect where the key is now stored.
        hint_->SetLabel("Stored in application settings (plaintext).");
    } else {
        // Normal path: store in the OS keyring.
        if (!Preferences::SetApiKey(provider, key)) {
            if (key.IsEmpty()) {
                // Deleting the key - also clear any plaintext copy so
                // HasApiKey is consistent.
                Preferences::SetApiKeyPlaintext(provider, wxString());
            }
            wxMessageBox(
                "Could not save the API key to the system keyring.\n\n"
                "If you're running headless or without a keyring daemon, the "
                "secret store may be unavailable.",
                "gritcode", wxOK | wxICON_ERROR, this);
            return;  // keep dialog open
        }
        // Also clear any stale plaintext copy so the keyring is the
        // single source of truth.
        Preferences::SetApiKeyPlaintext(provider, wxString());
    }

    // Persist the Grit History tools toggle.
    Preferences::SetEnableGritHistory(gritHistoryCb_->IsChecked());

    evt.Skip();  // let default handler close with wxID_OK
}
