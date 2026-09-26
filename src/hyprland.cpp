#include "hyprland.h"

#include <cstdlib>
#include <string>
#include <nlohmann/json.hpp>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace hyprland {

#ifdef __linux__
namespace {

// Send `request` to Hyprland's command socket and return the whole reply, or
// an empty string on any failure.
std::string Query(const char* request) {
    const char* sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!sig || !*sig || !runtime || !*runtime) return {};
    std::string path = std::string(runtime) + "/hypr/" + sig + "/.socket.sock";

    sockaddr_un addr{};
    if (path.size() >= sizeof(addr.sun_path)) return {};
    addr.sun_family = AF_UNIX;
    path.copy(addr.sun_path, path.size());

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return {};
    // Never stall the GUI thread on a wedged compositor.
    timeval timeout{0, 300 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    std::string reply;
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
        write(fd, request, std::char_traits<char>::length(request)) >= 0) {
        char buf[16384];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) reply.append(buf, n);
        if (n < 0) reply.clear();  // timeout or error: don't parse a fragment
    }
    close(fd);
    return reply;
}

}  // namespace

std::optional<bool> IsTiled() {
    std::string reply = Query("j/clients");
    if (reply.empty()) return std::nullopt;
    auto clients = nlohmann::json::parse(reply, nullptr, /*allow_exceptions=*/false);
    if (!clients.is_array()) return std::nullopt;
    // Field types vary across Hyprland versions (fullscreen was a bool before
    // it became a mode number), so read them loosely.
    auto truthy = [](const nlohmann::json& v) {
        return v.is_boolean() ? v.get<bool>()
                              : v.is_number() && v.get<double>() != 0;
    };
    const int pid = getpid();
    for (const auto& c : clients) {
        if (!c.is_object()) continue;
        auto p = c.find("pid");
        if (p == c.end() || !p->is_number() || p->get<int>() != pid) continue;
        auto fl = c.find("floating");
        auto fs = c.find("fullscreen");
        bool floating = fl != c.end() && truthy(*fl);
        bool fullscreen = fs != c.end() && truthy(*fs);
        return !floating || fullscreen;
    }
    return std::nullopt;
}

#else

std::optional<bool> IsTiled() { return std::nullopt; }

#endif

}  // namespace hyprland
