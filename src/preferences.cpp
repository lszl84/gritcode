#include "preferences.h"
#include <wx/config.h>
#include <wx/fileconf.h>
#if wxUSE_SECRETSTORE
#include <wx/secretstore.h>
#else
#include <libsecret/secret.h>
#include <gio/gio.h>
#endif

namespace {

// Service identifier used in the OS keyring. Per-provider suffix keeps each
// key in its own entry so adding a future provider doesn't churn existing
// stored keys.
const wxString kServicePrefix = "gritcode/";
const wxString kUsername      = "api_key";

const char* kModelIndexKey    = "/UI/LastModelIndex";
const char* kModelExplicitKey = "/UI/ModelExplicit";
const char* kEnableGritKey    = "/UI/EnableGritHistory";

// wxFileConfig key for the plaintext API-key fallback.
const char* kApiKeyPlaintext  = "/ApiKey/DeepSeek";

wxString ServiceFor(Preferences::Provider p) {
    switch (p) {
    case Preferences::Provider::DeepSeek: return kServicePrefix + "deepseek";
    }
    return kServicePrefix + "unknown";
}

#if !wxUSE_SECRETSTORE
// libsecret schema for our keyring entries.
const SecretSchema kSecretSchema = {
    "gritcode.ApiKey",
    SECRET_SCHEMA_NONE,
    {
        { "provider", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { NULL, SECRET_SCHEMA_ATTRIBUTE_STRING }
    }
};
#endif

}  // namespace

void Preferences::Init() {
    if (wxConfigBase::Get(false) != nullptr) return;
    // wxFileConfig path with wxCONFIG_USE_SUBDIR: stores inside
    // wxStandardPaths::GetUserDataDir() as ~/.gritcode/gritcode.conf,
    // sharing the directory with run_configs.json and the memory DB.
    auto* cfg = new wxFileConfig("gritcode", wxEmptyString,
                                 wxEmptyString, wxEmptyString,
                                 wxCONFIG_USE_SUBDIR);
    wxConfigBase::Set(cfg);
}

int Preferences::GetLastModelIndex() {
    auto* cfg = wxConfigBase::Get();
    long explicitChoice = 0;
    cfg->Read(kModelExplicitKey, &explicitChoice, 0L);

    if (!explicitChoice) {
        // User never changed the dropdown — pick the best available model.
        if (HasApiKey(Provider::DeepSeek)) return 2;  // DeepSeek Pro
        return 0;  // OpenCode Free (no key, only option that works)
    }

    long v = 0;
    cfg->Read(kModelIndexKey, &v, 0L);
    if (v < 0 || v > 2) v = 0;
    return (int)v;
}

void Preferences::SetLastModelIndex(int idx) {
    if (idx < 0 || idx > 2) idx = 0;
    auto* cfg = wxConfigBase::Get();
    cfg->Write(kModelIndexKey, (long)idx);
    cfg->Write(kModelExplicitKey, 1L);
    cfg->Flush();
}

bool Preferences::GetEnableGritHistory() {
    auto* cfg = wxConfigBase::Get();
    if (!cfg) return true;  // default on
    bool v = true;
    cfg->Read(kEnableGritKey, &v, true);
    return v;
}

void Preferences::SetEnableGritHistory(bool enabled) {
    auto* cfg = wxConfigBase::Get();
    if (!cfg) return;
    cfg->Write(kEnableGritKey, enabled);
    cfg->Flush();
}

// ---- OS keyring (wxSecretStore / libsecret) ----

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
    if (!GetApiKey(p).IsEmpty()) return true;
    if (!GetApiKeyPlaintext(p).IsEmpty()) return true;
    return false;
}

// ---- Keyring-health probe ----

bool Preferences::IsKeyringBroken() {
#if wxUSE_SECRETSTORE
    // wxSecretStore doesn't expose this level of introspection; assume
    // the platform secret store is functional.
    return false;
#else
    // Known gnome-keyring first-launch bug (Chromium 7a77463):
    //   Debian/XFCE/lightdm — after a fresh install the daemon reports
    //   a path for the default collection via ReadAlias("default"), but
    //   the object at that path doesn't actually exist.  Operations on
    //   the default collection (e.g. secret_password_store_sync) hang
    //   forever waiting for a password-create prompt that never renders.
    //
    //   Detection: call ReadAlias, then try to read the Locked property
    //   on the returned path.  If the object doesn't exist → broken.

    GError* err = nullptr;
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!conn) {
        if (err) g_error_free(err);
        return false;  // no session bus — not our broken-keyring case
    }

    // 1. Read the default-collection alias.
    GVariant* aliasResult = g_dbus_connection_call_sync(
        conn,
        "org.freedesktop.secrets",
        "/org/freedesktop/secrets",
        "org.freedesktop.Secret.Service",
        "ReadAlias",
        g_variant_new("(s)", "default"),
        G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE,
        2000,   // 2 s timeout — more than enough for a local D-Bus call
        nullptr,
        &err);

    if (err) {
        // Service not reachable — the keyring daemon may not be running
        // at all.  That's a different failure mode (SetApiKey will return
        // false quickly via libsecret).  Don't flag as "broken".
        g_error_free(err);
        g_object_unref(conn);
        return false;
    }

    gchar* collectionPath = nullptr;
    g_variant_get(aliasResult, "(o)", &collectionPath);
    g_variant_unref(aliasResult);

    if (!collectionPath || !*collectionPath) {
        g_free(collectionPath);
        g_object_unref(conn);
        return false;  // no default collection at all — not our case
    }

    // 2. Probe the collection object: try to read the Locked property.
    GVariant* lockedResult = g_dbus_connection_call_sync(
        conn,
        "org.freedesktop.secrets",
        collectionPath,
        "org.freedesktop.DBus.Properties",
        "Get",
        g_variant_new("(ss)",
                      "org.freedesktop.Secret.Collection", "Locked"),
        nullptr,
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        nullptr,
        &err);

    g_object_unref(conn);
    g_free(collectionPath);

    if (err) {
        // If the object doesn't exist at the aliased path, we're in the
        // Chromium-documented deadlock scenario.
        bool broken = (strstr(err->message, "does not exist") != nullptr);
        g_error_free(err);
        return broken;
    }

    g_variant_unref(lockedResult);
    return false;  // collection object exists → keyring is healthy
#endif
}

// ---- Plaintext fallback (wxFileConfig) ----

bool Preferences::SetApiKeyPlaintext(Provider p, const wxString& key) {
    auto* cfg = wxConfigBase::Get();
    if (!cfg) return false;

    if (key.IsEmpty()) {
        cfg->DeleteEntry(kApiKeyPlaintext);
    } else {
        cfg->Write(kApiKeyPlaintext, key);
    }
    cfg->Flush();
    return true;
}

wxString Preferences::GetApiKeyPlaintext(Provider p) {
    auto* cfg = wxConfigBase::Get();
    if (!cfg) return wxString();
    return cfg->Read(kApiKeyPlaintext, wxString());
}
