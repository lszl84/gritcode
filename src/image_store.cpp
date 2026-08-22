#include "image_store.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

std::string ImageStore::DataRoot() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        if (xdg[0] != '\0') return std::string(xdg) + "/gritcode";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/.local/share/gritcode";
    }
    return ".gritcode";  // fallback (shouldn't happen on Linux/macOS)
}

std::string ImageStore::ExtForMime(const std::string& mime) {
    if (mime == "image/png") return ".png";
    if (mime == "image/jpeg") return ".jpg";
    if (mime == "image/gif") return ".gif";
    if (mime == "image/webp") return ".webp";
    return ".img";
}

std::string ImageStore::Hash(const std::string& bytes) {
    // Two FNV-1a passes with different seeds -> 128 bits -> 32 hex chars.
    uint64_t h1 = 0xcbf29ce484222325ULL;
    uint64_t h2 = 0x84222325cbf29ce4ULL;
    for (unsigned char c : bytes) {
        h1 ^= c;
        h1 *= 0x100000001b3ULL;
        h2 ^= c;
        h2 *= 0x100000001b3ULL;
    }
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  (unsigned long long)h1, (unsigned long long)h2);
    return std::string(buf, 32);
}

std::string ImageStore::Dir() {
    std::string d = DataRoot() + "/images";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

std::string ImageStore::Save(const std::string& bytes, const std::string& mime) {
    std::string hash = Hash(bytes);
    std::string path = Dir() + "/" + hash + ExtForMime(mime);
    std::error_code ec;
    if (fs::exists(path, ec)) return hash;

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return {};
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.close();
    return f ? hash : std::string{};
}

std::string ImageStore::PathFor(const std::string& hash, const std::string& mime) {
    if (hash.empty()) return {};
    std::string path = Dir() + "/" + hash + ExtForMime(mime);
    std::error_code ec;
    if (fs::exists(path, ec)) return path;
    return {};
}

std::string ImageStore::Load(const std::string& hash, const std::string& mime) {
    std::string path = PathFor(hash, mime);
    if (path.empty()) return {};
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
