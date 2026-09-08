#include "settings_dialog.h"
#include "preferences.h"
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/hyperlink.h>
#include <wx/msgdlg.h>

SettingsDialog::SettingsDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, "Settings",
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {

    auto* outer = new wxBoxSizer(wxVERTICAL);
    const int wrapWidth = FromDIP(440);  // target text width -> compact dialog
    const int margin = FromDIP(12);      // single left/right margin for everything

    auto addHeading = [&](const wxString& text) {
        auto* st = new wxStaticText(this, wxID_ANY, text);
        wxFont f = st->GetFont();
        f.MakeBold();
        st->SetFont(f);
        outer->Add(st, 0, wxLEFT | wxRIGHT | wxTOP, margin);
        return st;
    };
    auto addHint = [&](const wxString& text) {
        auto* st = new wxStaticText(this, wxID_ANY, text);
        wxFont f = st->GetFont();
        f.SetPointSize(f.GetPointSize() - 1);
        st->SetFont(f);
        st->Wrap(wrapWidth);
        outer->Add(st, 0, wxLEFT | wxRIGHT | wxTOP, margin);
        return st;
    };
    auto addSeparator = [&]() {
        outer->Add(new wxStaticLine(this), 0,
                   wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, margin);
    };

    // ---- DeepSeek section ----
    addHeading("DeepSeek");

    outer->Add(new wxStaticText(this, wxID_ANY, "API key:"), 0,
               wxLEFT | wxRIGHT | wxTOP, margin);

    auto* keyRow = new wxBoxSizer(wxHORIZONTAL);
    // Pre-fill with the existing key so the user can see they have one set
    // (masked) and can edit it.  Show toggle reveals plaintext.
    wxString existing = Preferences::GetApiKey(Preferences::Provider::DeepSeek);
    if (existing.IsEmpty()) {
        existing = Preferences::GetApiKeyPlaintext(
            Preferences::Provider::DeepSeek);
    }
    keyCtrl_ = new wxTextCtrl(this, wxID_ANY, existing,
                              wxDefaultPosition, FromDIP(wxSize(300, -1)),
                              wxTE_PASSWORD);
    showCb_ = new wxCheckBox(this, wxID_ANY, "Show");
    keyRow->Add(keyCtrl_, 0, wxALIGN_CENTER_VERTICAL);
    keyRow->Add(showCb_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    keyRow->AddStretchSpacer(1);
    outer->Add(keyRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);

    auto* link = new wxHyperlinkCtrl(this, wxID_ANY,
        "Get an API key at platform.deepseek.com",
        "https://platform.deepseek.com/");
    outer->Add(link, 0, wxLEFT | wxRIGHT | wxTOP, margin);

    // Snapshot the keyring health so the hint is consistent for the
    // lifetime of this dialog instance (even if the daemon state changes
    // mid-session, which it normally shouldn't).
    keyringWasBroken_ = Preferences::IsKeyringBroken();

    if (keyringWasBroken_) {
        hint_ = addHint(
            "System keyring not fully initialized - known issue on some "
            "Debian-based systems on first login. Keys will be stored in the "
            "application settings file (" + Preferences::ConfigFilePath() +
            ") in plaintext.");
    } else {
        hint_ = addHint("Stored securely in your system keyring.");
    }

    // ---- Local models section ----
    addSeparator();
    addHeading("Local models");

    addHint("Connect to an OpenAI-compatible server on your network "
            "(e.g. MLX-VLM or llama.cpp). Its models appear in the "
            "Model dropdown.");

    auto* hostRow = new wxBoxSizer(wxHORIZONTAL);
    hostRow->Add(new wxStaticText(this, wxID_ANY, "IP / host:"), 0,
                 wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    hostCtrl_ = new wxTextCtrl(this, wxID_ANY, Preferences::GetLocalHost(),
                               wxDefaultPosition, FromDIP(wxSize(180, -1)));
    hostRow->Add(hostCtrl_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    hostRow->Add(new wxStaticText(this, wxID_ANY, "Port:"), 0,
                 wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    portCtrl_ = new wxTextCtrl(this, wxID_ANY,
                               wxString::Format("%d", Preferences::GetLocalPort()),
                               wxDefaultPosition, FromDIP(wxSize(64, -1)));
    hostRow->Add(portCtrl_, 0, wxALIGN_CENTER_VERTICAL);
    hostRow->AddStretchSpacer(1);
    outer->Add(hostRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);

    preferLocalCb_ = new wxCheckBox(this, wxID_ANY, "Prefer local models");
    preferLocalCb_->SetValue(Preferences::GetPreferLocal());
    outer->Add(preferLocalCb_, 0, wxLEFT | wxRIGHT | wxTOP, margin);

    addHint("When on, new sessions (or ones with no model picked) select "
            "a local model instead of DeepSeek.");

    // ---- Agent tools section ----
    addSeparator();
    addHeading("Agent tools");

    gritHistoryCb_ = new wxCheckBox(this, wxID_ANY,
        "Enable Grit History tools");
    gritHistoryCb_->SetValue(Preferences::GetEnableGritHistory());
    outer->Add(gritHistoryCb_, 0, wxLEFT | wxRIGHT | wxTOP, margin);

    addHint("Lets the agent search your past gritcode sessions across "
            "projects (grit_history_search/fetch). Turn off for "
            "self-contained, shareable sessions.");

    outer->AddStretchSpacer(1);

    // ---- Buttons ----
    auto* btns = CreateButtonSizer(wxOK | wxCANCEL);
    if (btns) outer->Add(btns, 0, wxEXPAND | wxALL, margin);

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
            "Store the key in gritcode's settings file as plaintext instead?\n(" +
            Preferences::ConfigFilePath() +
            ")\n\n"
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

    // ---- Local models ----
    wxString host = hostCtrl_->GetValue();
    host.Trim().Trim(false);
    wxString rest;
    if (host.StartsWith("https://", &rest)) host = rest;
    else if (host.StartsWith("http://", &rest)) host = rest;
    while (!host.empty() && host.Last() == '/') host.RemoveLast();

    long port = 8080;
    wxString portStr = portCtrl_->GetValue();
    portStr.Trim().Trim(false);
    bool portOk = true;
    if (!portStr.IsEmpty()) {
        portOk = portStr.ToLong(&port) && port >= 1 && port <= 65535;
    }
    if (!portOk) {
        wxMessageBox("Local model port must be a number between 1 and 65535.",
                     "gritcode", wxOK | wxICON_WARNING, this);
        return;  // keep dialog open
    }
    Preferences::SetLocalHost(host);
    Preferences::SetLocalPort((int)port);
    Preferences::SetPreferLocal(preferLocalCb_->IsChecked());

    // Persist the Grit History tools toggle.
    Preferences::SetEnableGritHistory(gritHistoryCb_->IsChecked());

    evt.Skip();  // let default handler close with wxID_OK
}
