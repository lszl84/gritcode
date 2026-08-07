#pragma once
#include <wx/string.h>

// Facade over wxConfig (plaintext app prefs) and wxSecretStore (OS-backed
// secrets). Non-secret state — last selected model, future UI toggles — goes
// through wxConfig at ~/.gritcode/gritcode.conf. API keys go through
// wxSecretStore, which on Linux talks to libsecret (gnome-keyring/kwallet).
//
// A known gnome-keyring bug on Debian/XFCE/lightdm (first login after a fresh
// install) leaves the daemon in a state where it reports a default collection
// path but the object doesn't exist.  libsecret's sync store call hangs
// forever in that case.  We detect this ("keyring broken") via a quick D-Bus
// probe and fall back to storing the API key in the app's wxFileConfig as
// plaintext, with an explicit warning to the user.
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
    // unavailable.  Checks the OS keyring only — does NOT fall back to
    // the plaintext store.  Use HasApiKey() for a combined check.
    static wxString GetApiKey(Provider p);

    // Stores `key` for the provider in the OS keyring.  An empty key
    // deletes the entry.  Returns false if the secret store is
    // unavailable or the operation failed; the settings dialog surfaces
    // this to the user.
    //
    // IMPORTANT: this call blocks the calling thread.  If the keyring is
    // in the broken-first-launch state it will hang forever.  Callers
    // MUST check IsKeyringBroken() first and avoid this path when true.
    static bool SetApiKey(Provider p, const wxString& key);

    // Combined check: true if a key exists in the OS keyring *or* the
    // plaintext fallback store.
    static bool HasApiKey(Provider p);

    // ---- keyring-health probe (libsecret / GDBus) ----

    // Returns true when the gnome-keyring daemon is in the known
    // broken-first-launch state: the default-collection alias resolves
    // to an object path, but the object itself doesn't exist.
    // On healthy systems (or after a reboot) this returns false.
    // Always returns false on non-Linux or when wxUSE_SECRETSTORE is 1.
    static bool IsKeyringBroken();

    // ---- plaintext fallback (wxFileConfig) ----

    // Store/retrieve the API key in the app's plaintext config file.
    // Used when the system keyring is unavailable or broken, with the
    // user's explicit consent.
    static bool     SetApiKeyPlaintext(Provider p, const wxString& key);
    static wxString GetApiKeyPlaintext(Provider p);
};
