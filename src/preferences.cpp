#include "preferences.h"
#include <wx/config.h>
#include <wx/fileconf.h>
#if wxUSE_SECRETSTORE
#include <wx/secretstore.h>
#endif

namespace {

// Service identifier used in the OS keyring. Per-provider suffix keeps each
// key in its own entry so adding a future provider doesn't churn existing
// stored keys.
const wxString kServicePrefix = "wx_gritcode/";
const wxString kUsername      = "api_key";

const char* kModelIndexKey = "/UI/LastModelIndex";

// Fallback config-path keys when wxSecretStore is unavailable.
const char* kApiKeyFallbackKey = "/Secrets/ApiKey";

wxString ServiceFor(Preferences::Provider p) {
    switch (p) {
    case Preferences::Provider::DeepSeek: return kServicePrefix + "deepseek";
    }
    return kServicePrefix + "unknown";
}

}  // namespace

void Preferences::Init() {
    if (wxConfigBase::Get(false) != nullptr) return;
    // wxFileConfig path: ~/.config/wx_gritcode.conf on Linux (XDG-respecting
    // when wxCONFIG_USE_SUBDIR is not set; the default file lives next to
    // other dotconfigs). Vendor name kept empty so the path is short.
    auto* cfg = new wxFileConfig("wx_gritcode", wxEmptyString,
                                 wxEmptyString, wxEmptyString,
                                 wxCONFIG_USE_LOCAL_FILE);
    wxConfigBase::Set(cfg);
}

int Preferences::GetLastModelIndex() {
    auto* cfg = wxConfigBase::Get();
    long v = 0;
    cfg->Read(kModelIndexKey, &v, 0L);
    if (v < 0 || v > 2) v = 0;
    return (int)v;
}

void Preferences::SetLastModelIndex(int idx) {
    if (idx < 0 || idx > 2) idx = 0;
    auto* cfg = wxConfigBase::Get();
    cfg->Write(kModelIndexKey, (long)idx);
    cfg->Flush();
}

wxString Preferences::GetApiKey(Provider p) {
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk()) return wxString();
    wxString username;
    wxSecretValue value;
    if (!store.Load(ServiceFor(p), username, value)) return wxString();
    if (!value.IsOk()) return wxString();
    return value.GetAsString();
#else
    // Fallback: read from plaintext config file.
    auto* cfg = wxConfigBase::Get();
    return cfg->Read(kApiKeyFallbackKey, wxString());
#endif
}

bool Preferences::SetApiKey(Provider p, const wxString& key) {
#if wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk()) return false;
    if (key.IsEmpty()) {
        store.Delete(ServiceFor(p));
        return true;
    }
    return store.Save(ServiceFor(p), kUsername, wxSecretValue(key));
#else
    // Fallback: store in plaintext config file.
    auto* cfg = wxConfigBase::Get();
    if (key.IsEmpty()) {
        cfg->DeleteEntry(kApiKeyFallbackKey);
    } else {
        cfg->Write(kApiKeyFallbackKey, key);
    }
    cfg->Flush();
    return true;
#endif
}

bool Preferences::HasApiKey(Provider p) {
    return !GetApiKey(p).IsEmpty();
}
