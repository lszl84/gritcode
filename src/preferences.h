#pragma once
#include <wx/string.h>

// Facade over wxConfig (plaintext app prefs) and wxSecretStore (OS-backed
// secrets). Non-secret state — last selected model, future UI toggles — goes
// through wxConfig at ~/.config/gritcode.conf. API keys go through
// wxSecretStore, which on Linux talks to libsecret (gnome-keyring/kwallet).
//
// All methods are safe to call before Init(); the wxConfig calls just
// auto-create the singleton on first use. Init() exists so we can override
// the app name once at startup and so the secret store handle is checked
// up front (its IsOk() can be false if the user has no keyring daemon).
class Preferences {
public:
    // Install the global wxConfig singleton with our app name. Idempotent.
    static void Init();

    // ---- non-secret prefs (wxConfig) ----

    // Resolves the effective model index. If the user has never explicitly
    // picked a model, defaults to DeepSeek Pro (2) when an API key is stored,
    // otherwise OpenCode Free (0). Once the user changes the dropdown, that
    // choice sticks permanently.
    static int  GetLastModelIndex();
    static void SetLastModelIndex(int idx);

    // ---- API keys (wxSecretStore) ----

    enum class Provider {
        DeepSeek,
    };

    // Returns empty wxString if no key is stored or the secret store is
    // unavailable. Callers treat empty as "not configured".
    static wxString GetApiKey(Provider p);

    // Stores `key` for the provider. An empty key deletes the entry. Returns
    // false if the secret store is unavailable or the operation failed; the
    // settings dialog surfaces this to the user.
    static bool SetApiKey(Provider p, const wxString& key);

    static bool HasApiKey(Provider p);
};
