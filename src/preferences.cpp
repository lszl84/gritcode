#include "preferences.h"
#include <wx/config.h>
#include <wx/fileconf.h>
#if wxUSE_SECRETSTORE
#include <wx/secretstore.h>
#else
#include <libsecret/secret.h>
#endif

namespace {

// Service identifier used in the OS keyring. Per-provider suffix keeps each
// key in its own entry so adding a future provider doesn't churn existing
// stored keys.
const wxString kServicePrefix = "wx_gritcode/";
const wxString kUsername      = "api_key";

const char* kModelIndexKey = "/UI/LastModelIndex";

wxString ServiceFor(Preferences::Provider p) {
    switch (p) {
    case Preferences::Provider::DeepSeek: return kServicePrefix + "deepseek";
    }
    return kServicePrefix + "unknown";
}

#if !wxUSE_SECRETSTORE
// libsecret schema for our keyring entries.
const SecretSchema kSecretSchema = {
    "wx_gritcode.ApiKey",
    SECRET_SCHEMA_NONE,
    {
        { "provider", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { NULL, 0 }
    }
};
#endif

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
    GError* err = nullptr;
    gchar* secret = secret_password_lookup_sync(
        &kSecretSchema, nullptr, &err,
        "provider", ServiceFor(p).utf8_str().data(), nullptr);
    if (err != nullptr) {
        g_error_free(err);
        return wxString();
    }
    if (secret == nullptr) return wxString();
    wxString result = wxString::FromUTF8(secret);
    secret_password_free(secret);
    return result;
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
    GError* err = nullptr;
    if (key.IsEmpty()) {
        secret_password_clear_sync(
            &kSecretSchema, nullptr, &err,
            "provider", ServiceFor(p).utf8_str().data(), nullptr);
    } else {
        secret_password_store_sync(
            &kSecretSchema, SECRET_COLLECTION_DEFAULT,
            ServiceFor(p).utf8_str().data(),
            key.utf8_str().data(),
            nullptr, &err,
            "provider", ServiceFor(p).utf8_str().data(), nullptr);
    }
    if (err != nullptr) {
        g_error_free(err);
        return false;
    }
    return true;
#endif
}

bool Preferences::HasApiKey(Provider p) {
    return !GetApiKey(p).IsEmpty();
}
