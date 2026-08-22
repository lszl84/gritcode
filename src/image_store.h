#pragma once
#include <string>

// Hash-addressed image blob store. Attached images are copied into
// <data root>/images/<hash>.<ext>, and session messages reference them by
// hash, so the original file can be deleted without affecting the session.
//
// The hash is a stable 128-bit FNV-1a (not cryptographic, but the bytes are
// user-attached images, not adversarial input; collision probability is
// negligible). Same image bytes map to the same hash, giving dedup for free.
class ImageStore {
public:
    // Copy `bytes` into the store if not already present. Returns the
    // 32-hex-char content hash, or an empty string on write failure.
    static std::string Save(const std::string& bytes, const std::string& mime);

    // Absolute path of the stored blob for (hash, mime), or empty if absent.
    static std::string PathFor(const std::string& hash, const std::string& mime);

    // Raw blob bytes, or empty if absent.
    static std::string Load(const std::string& hash, const std::string& mime);

    // Directory where blobs live (created on demand).
    static std::string Dir();

    // Content hash of arbitrary bytes (used to key the store).
    static std::string Hash(const std::string& bytes);

private:
    static std::string DataRoot();
    static std::string ExtForMime(const std::string& mime);
};
