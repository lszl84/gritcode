#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>

// Chrono-based scoped timer for hot-path instrumentation. Enabled at runtime
// via WX_GRITCODE_PROF=1 (set once at startup, queried lazily). Logs to stderr
// when the scope's elapsed time exceeds a threshold so we don't spam normal
// runs.
//
// Usage:
//   PERF_SCOPE("OnPaint");                         // logs if > kPerfThreshUs
//   PERF_SCOPE_T("OnPaint", 500);                  // custom threshold (us)
//   PERF_LOG("blocks=%zu", blocks_.size());        // unconditional log
//
// All compiled in even when disabled — the env-var check is one byte load.
namespace perf_log {
inline bool Enabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* v = std::getenv("WX_GRITCODE_PROF");
        cached = (v && *v && *v != '0') ? 1 : 0;
        if (cached) {
            // Make stderr unbuffered so log entries reach the file in order
            // even when the test driver tails it concurrently.
            std::setvbuf(stderr, nullptr, _IONBF, 0);
        }
    }
    return cached != 0;
}

class Scope {
public:
    Scope(const char* name, long thresholdUs)
        : name_(name), thresholdUs_(thresholdUs),
          start_(std::chrono::steady_clock::now()) {}
    ~Scope() {
        if (!Enabled()) return;
        auto d = std::chrono::steady_clock::now() - start_;
        long us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
        if (us >= thresholdUs_) {
            std::fprintf(stderr, "[perf] %s %ld us\n", name_, us);
        }
    }
private:
    const char* name_;
    long thresholdUs_;
    std::chrono::steady_clock::time_point start_;
};
}  // namespace perf_log

#define PERF_CONCAT_(a, b) a##b
#define PERF_CONCAT(a, b)  PERF_CONCAT_(a, b)
#define PERF_SCOPE(name)        ::perf_log::Scope PERF_CONCAT(perf_, __LINE__)((name), 500)
#define PERF_SCOPE_T(name, us)  ::perf_log::Scope PERF_CONCAT(perf_, __LINE__)((name), (us))
#define PERF_LOG(fmt, ...)      do { if (::perf_log::Enabled()) std::fprintf(stderr, "[perf] " fmt "\n", ##__VA_ARGS__); } while (0)
