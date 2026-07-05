#pragma once
#include <wx/dialog.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>

// Modal dialog for editing API keys. Reads the current DeepSeek key from
// wxSecretStore on construction; on Save it writes back through Preferences.
// Cancel discards. The key field is masked by default with a "Show" toggle.
//
// Returns wxID_OK if the user saved (key may have changed) or wxID_CANCEL.
class SettingsDialog : public wxDialog {
public:
    SettingsDialog(wxWindow* parent);

private:
    wxTextCtrl* keyCtrl_ = nullptr;
    wxCheckBox* showCb_  = nullptr;

    void OnSave(wxCommandEvent&);
    void OnToggleShow(wxCommandEvent&);
};
