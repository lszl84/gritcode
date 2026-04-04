#include "keychain.h"
#include <libsecret/secret.h>

namespace keychain {

// Use the same schema and attributes as wxWidgets' wxSecretStore
// so we read the same keychain entry without re-entering the key.
static const SecretSchema SCHEMA = {
    "org.freedesktop.Secret.Generic",
    SECRET_SCHEMA_DONT_MATCH_NAME,
    {
        {"service", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {NULL, SECRET_SCHEMA_ATTRIBUTE_STRING}
    },
    0, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static const char* SERVICE = "FastCodeNative/OpenCodeZen";
static const char* ACCOUNT = "apikey";

std::string LoadApiKey() {
    GError* error = nullptr;
    gchar* password = secret_password_lookup_sync(
        &SCHEMA, nullptr, &error,
        "service", SERVICE,
        "account", ACCOUNT,
        NULL);

    if (error) {
        g_error_free(error);
        return {};
    }

    std::string result;
    if (password) {
        result = password;
        secret_password_free(password);
    }
    return result;
}

bool SaveApiKey(const std::string& key) {
    GError* error = nullptr;
    gboolean ok = secret_password_store_sync(
        &SCHEMA,
        SECRET_COLLECTION_DEFAULT,
        SERVICE,  // label
        key.c_str(),
        nullptr, &error,
        "service", SERVICE,
        "account", ACCOUNT,
        NULL);

    if (error) {
        g_error_free(error);
        return false;
    }
    return ok;
}

bool ClearApiKey() {
    GError* error = nullptr;
    gboolean ok = secret_password_clear_sync(
        &SCHEMA, nullptr, &error,
        "service", SERVICE,
        "account", ACCOUNT,
        NULL);

    if (error) {
        g_error_free(error);
        return false;
    }
    return ok;
}

} // namespace keychain
