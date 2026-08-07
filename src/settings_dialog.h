#pragma once
#include <wx/dialog.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/stattext.h>

// Modal dialog for editing API keys. Reads the current DeepSeek key from
// wxSecretStore (falling back to the plaintext config file if the system
// keyring is unavailable) on construction; on Save it writes back through
// Preferences. Cancel discards. The key field is masked by default with a
// "Show" toggle.
//
// When the system keyring is in the known broken-first-launch state
// (Debian/XFCE/lightdm — default collection path reported but the object
// doesn't exist), the dialog warns the user and offers to store the key
// in the application's plaintext settings file instead.
//
// Returns wxID_OK if the user saved (key may have changed) or wxID_CANCEL.
class SettingsDialog : public wxDialog {
public:
    SettingsDialog(wxWindow* parent);

private:
    wxTextCtrl*   keyCtrl_ = nullptr;
    wxCheckBox*   showCb_  = nullptr;
    wxStaticText* hint_    = nullptr;

    bool keyringWasBroken_ = false;  // snapshot at dialog-open time

    void OnSave(wxCommandEvent&);
    void OnToggleShow(wxCommandEvent&);
};
