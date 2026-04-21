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

namespace debug {

// Log file path — the AI agent can read this after a crash to diagnose issues.
inline const char* LogPath() { return "/tmp/gritcode-debug.log"; }

// Maximum log file size before truncation (1 MB)
inline constexpr size_t kMaxLogSize = 1 * 1024 * 1024;

// Printf-style logging to /tmp/gritcode-debug.log with timestamp.
// Thread-safe. Auto-truncates when the file exceeds kMaxLogSize.
inline void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void Log(const char* fmt, ...) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);

    FILE* f = fopen(LogPath(), "a");
    if (!f) return;

    // Check file size — if too large, truncate
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz > (long)kMaxLogSize) {
        fclose(f);
        f = fopen(LogPath(), "w");
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
