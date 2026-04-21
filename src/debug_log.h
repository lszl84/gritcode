// Gritcode — GPU-rendered AI coding harness
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

#pragma once

#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>
#include <ctime>
#include <unistd.h>

namespace debug {

// Maximum log file size before truncation (30 MB)
inline constexpr size_t kMaxLogSize = 30 * 1024 * 1024;

// Shared state — deliberately defined once in a single inline function
// so all TUs share the same statics.
inline std::mutex& LogMutex() {
    static std::mutex mu;
    return mu;
}

inline char* LogPathBuf() {
    static char buf[256] = {};
    static bool init = false;
    if (!init) {
        snprintf(buf, sizeof(buf), "/tmp/gritcode-%d.log", (int)getpid());
        init = true;
    }
    return buf;
}

// Set the session ID — call once during App init. The log file path
// becomes /tmp/gritcode-<sessionId>-<pid>.log so multiple instances
// (even for different sessions) get their own log files.
inline void SetSessionId(const std::string& id) {
    std::lock_guard<std::mutex> lock(LogMutex());
    snprintf(LogPathBuf(), 256, "/tmp/gritcode-%s-%d.log",
             id.c_str(), (int)getpid());
}

// Printf-style logging with timestamp. Thread-safe. Auto-truncates.
inline void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(LogMutex());

    const char* path = LogPathBuf();
    FILE* f = fopen(path, "a");
    if (!f) return;

    // Check file size — if too large, truncate
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz > (long)kMaxLogSize) {
        fclose(f);
        f = fopen(path, "w");
        if (!f) return;
    }

    // Timestamp
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm_buf);
    fprintf(f, "%s.%03ld ", timebuf, ts.tv_nsec / 1000000);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fprintf(f, "\n");
    fclose(f);
}

} // namespace debug

// Convenience macro — short to type, easy to grep for
#define DLOG(...) debug::Log(__VA_ARGS__)
