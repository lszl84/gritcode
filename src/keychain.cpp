// FastCode Native — GPU-rendered AI coding harness
// Copyright (C) 2026 luke@devmindscape.com
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

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
        {NULL, SECRET_SCHEMA_ATTRIBUTE_STRING}
    },
    0, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static const char* SERVICE = "FastCodeNative/OpenCodeZen";

std::string LoadApiKey() {
    GError* error = nullptr;
    gchar* password = secret_password_lookup_sync(
        &SCHEMA, nullptr, &error,
        "service", SERVICE,
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
        NULL);

    if (error) {
        g_error_free(error);
        return false;
    }
    return ok;
}

} // namespace keychain
