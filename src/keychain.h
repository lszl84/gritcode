#pragma once
#include <string>

namespace keychain {

// Load API key from system keychain via libsecret.
// Returns empty string if not found.
std::string LoadApiKey();

// Save API key to system keychain.
// Returns true on success.
bool SaveApiKey(const std::string& key);

// Delete API key from system keychain.
bool ClearApiKey();

} // namespace keychain
