#include "settings_dialog.h"
#include "claude_agent.h"
#include "preferences.h"
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/hyperlink.h>
#include <wx/msgdlg.h>
#include <cstdlib>
#include <mutex>
#include <thread>

// Handoff between the dialog and its background Claude Code lookup. The
// lookup can outlive the dialog, so it reports through `target`, which the
// dialog clears (under `mu`) when it closes.
struct ClaudeProbeState {
    std::mutex mu;
    SettingsDialog* target = nullptr;
};

namespace {

// Claude Code effort levels, in dropdown order. "" is Claude Code's own
// default and passes no --effort flag.
struct EffortLevel {
    const char* value;
    const char* label;
};
const EffortLevel kClaudeEfforts[] = {
    {"", "Default"}, {"low", "Low"}, {"medium", "Medium"},
    {"high", "High"}, {"xhigh", "Extra high"}, {"max", "Max"},
};

}  // namespace

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

    auto* effortRow = new wxBoxSizer(wxHORIZONTAL);
    auto* effortLabel = new wxStaticText(this, wxID_ANY, "Reasoning effort:");
    effortChoice_ = new wxChoice(this, wxID_ANY);
    effortChoice_->Append("High");
    effortChoice_->Append("Max");
    // The best size of a choice with short labels is too narrow on macOS
    // (the popup chevron eats the text); give it room like sessionChoice_.
    effortChoice_->SetMinSize(FromDIP(wxSize(110, -1)));
    effortChoice_->SetSelection(
        Preferences::GetReasoningEffort() == "max" ? 1 : 0);
    effortChoice_->SetToolTip(
        "How hard DeepSeek thinks before answering. High is DeepSeek's "
        "default. Max can do better on hard coding tasks but is slower and "
        "uses more reasoning tokens.");
    effortRow->Add(effortLabel, 0, wxALIGN_CENTER_VERTICAL);
    effortRow->Add(effortChoice_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    outer->Add(effortRow, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    // ---- Claude section ----
    auto* claudeHeading = new wxStaticText(this, wxID_ANY, "Claude");
    wxFont chf = claudeHeading->GetFont();
    chf.MakeBold();
    claudeHeading->SetFont(chf);
    outer->Add(claudeHeading, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    // Gritcode drives the user's own Claude Code install and never handles
    // Claude credentials, so sign-in happens in Claude Code itself.
    auto* claudeInfo = new wxStaticText(this, wxID_ANY,
        "Uses your installed Claude Code (the claude command).\n"
        "Sign in by running claude in a terminal first.");
    claudeInfo->SetFont(smaller);
    outer->Add(claudeInfo, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    // Filled in by OnClaudeProbed. Ellipsized so a long install path can't
    // widen the dialog after it is already on screen.
    claudeStatus_ = new wxStaticText(this, wxID_ANY,
        wxString::FromUTF8("Looking for Claude Code\xE2\x80\xA6"),
        wxDefaultPosition, wxDefaultSize,
        wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_MIDDLE);
    claudeStatus_->SetFont(smaller);
    claudeInstallLink_ = new wxHyperlinkCtrl(this, wxID_ANY,
        "Claude Code not found - install it",
        "https://code.claude.com/docs/en/setup");
    claudeInstallLink_->Hide();
    outer->AddSpacer(4);
    outer->Add(claudeStatus_, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    outer->Add(claudeInstallLink_, 0, wxLEFT | wxRIGHT, 12);

    auto* claudeEffortRow = new wxBoxSizer(wxHORIZONTAL);
    auto* claudeEffortLabel = new wxStaticText(this, wxID_ANY, "Effort:");
    // Same label width as DeepSeek's, so the two effort dropdowns line up.
    claudeEffortLabel->SetMinSize(effortLabel->GetBestSize());
    claudeEffortChoice_ = new wxChoice(this, wxID_ANY);
    const wxString currentEffort = Preferences::GetClaudeEffort();
    int effortSel = 0;
    for (int i = 0; i < (int)(sizeof(kClaudeEfforts) / sizeof(kClaudeEfforts[0])); ++i) {
        claudeEffortChoice_->Append(kClaudeEfforts[i].label);
        if (currentEffort == kClaudeEfforts[i].value) effortSel = i;
    }
    claudeEffortChoice_->SetMinSize(FromDIP(wxSize(110, -1)));
    claudeEffortChoice_->SetSelection(effortSel);
    claudeEffortChoice_->SetToolTip(
        "How hard Claude thinks before answering. Default uses Claude Code's "
        "own setting for the model. Higher levels can do better on hard tasks "
        "but are slower and use more of your Claude usage.");
    claudeEffortRow->Add(claudeEffortLabel, 0, wxALIGN_CENTER_VERTICAL);
    claudeEffortRow->Add(claudeEffortChoice_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    outer->Add(claudeEffortRow, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    // ---- Agent tools section ----
    auto* toolsHeading = new wxStaticText(this, wxID_ANY, "Agent tools");
    wxFont thf = toolsHeading->GetFont();
    thf.MakeBold();
    toolsHeading->SetFont(thf);
    outer->Add(toolsHeading, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    gritHistoryCb_ = new wxCheckBox(this, wxID_ANY,
        "Enable Grit History tools");
    gritHistoryCb_->SetValue(Preferences::GetEnableGritHistory());
    // The explanation lives in a tooltip: as a static label it set the
    // dialog's minimum width and didn't reflow on resize.
    gritHistoryCb_->SetToolTip(
        "Lets the agent search your past gritcode sessions across projects "
        "(grit_history_search/fetch). Turn off for self-contained sessions "
        "that can be exported and shared without referencing your other work.");
    outer->Add(gritHistoryCb_, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    outer->AddStretchSpacer(1);

    // ---- Buttons ----
    auto* btns = CreateButtonSizer(wxOK | wxCANCEL);
    if (btns) outer->Add(btns, 0, wxEXPAND | wxALL, 12);

    SetSizerAndFit(outer);
    SetMinSize(GetSize());

    Bind(wxEVT_BUTTON, &SettingsDialog::OnSave, this, wxID_OK);
    showCb_->Bind(wxEVT_CHECKBOX, &SettingsDialog::OnToggleShow, this);

    StartClaudeProbe();
}

SettingsDialog::~SettingsDialog() {
    // A lookup still running must not call back into a dead dialog. Any
    // callback it already queued is dropped along with this window's events.
    std::lock_guard<std::mutex> lk(claudeProbe_->mu);
    claudeProbe_->target = nullptr;
}

void SettingsDialog::StartClaudeProbe() {
    claudeProbe_ = std::make_shared<ClaudeProbeState>();
    claudeProbe_->target = this;
    // `claude --version` usually takes milliseconds but can take seconds
    // (macOS checks a freshly updated binary on its first run), so it never
    // runs on the GUI thread. Detached: the dialog may close before it ends.
    std::thread([probe = claudeProbe_]() {
        const std::string exe = FindClaudeExecutable();
        const std::string version = ClaudeVersion(exe);
        std::lock_guard<std::mutex> lk(probe->mu);
        if (SettingsDialog* dlg = probe->target) {
            dlg->CallAfter([dlg, exe, version]() {
                dlg->OnClaudeProbed(exe, version);
            });
        }
    }).detach();
}

void SettingsDialog::OnClaudeProbed(const std::string& exe,
                                    const std::string& version) {
    if (exe.empty()) {
        claudeStatus_->Hide();
        claudeInstallLink_->Show();
        Layout();
        return;
    }
    wxString where = wxString::FromUTF8(exe);
    if (const char* home = std::getenv("HOME")) {
        wxString h = wxString::FromUTF8(home);
        if (!h.IsEmpty() && where.StartsWith(h)) where = "~" + where.Mid(h.length());
    }
    // `claude --version` prints e.g. "2.1.281 (Claude Code)".
    const std::string v = version.substr(0, version.find(' '));
    claudeStatus_->SetLabel(v.empty()
        ? "Found Claude Code at " + where
        : "Found Claude Code " + wxString::FromUTF8(v) + " at " + where);
    Layout();
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

    // Persist the Grit History tools toggle and the reasoning efforts.
    Preferences::SetEnableGritHistory(gritHistoryCb_->IsChecked());
    Preferences::SetReasoningEffort(
        effortChoice_->GetSelection() == 1 ? "max" : "high");
    int ce = claudeEffortChoice_->GetSelection();
    if (ce >= 0 && ce < (int)(sizeof(kClaudeEfforts) / sizeof(kClaudeEfforts[0])))
        Preferences::SetClaudeEffort(kClaudeEfforts[ce].value);

    evt.Skip();  // let default handler close with wxID_OK
}
