# gritcode vs OpenCode — DeepSeek token-usage comparison

This document records a controlled, like-for-like comparison of per-request token
usage between **gritcode** and **OpenCode** when driving **DeepSeek V4 Pro**
(`deepseek-v4-pro`). It describes the methodology, the instrumentation required
to run the test in gritcode, and the measured results.

Result in one line: **on an identical workload, gritcode uses ~45% fewer tokens
(~24% cheaper) than OpenCode. The entire difference is fixed per-request
overhead (system prompt + tool schemas), not token-management strategy.**

---

## Methodology

### Applications under test

| | gritcode | OpenCode |
|---|---|---|
| Version | 0.5.12 (built from source, instrumented) | v2.0.10 (`opencode run --standalone`) |
| Stack | C++20 / wxWidgets / libcurl | TypeScript / Bun / AI SDK |

### Controlled variables

- **Model:** `deepseek-v4-pro` for both.
- **Reasoning effort:** `high` for both. gritcode sends `reasoning_effort: "high"`
  by default; OpenCode does **not** send the field at all by default, so OpenCode
  was run with the explicit variant `deepseek/deepseek-v4-pro#high` to match.
- **Tool surface:** gritcode's cross-project memory tools
  (`grit_history_search` / `grit_history_fetch`) were disabled so its tool set
  matches OpenCode's. This is a one-line test-only change (see below).
- **Workload:** an identical, scripted **20-turn session** — create a C program
  (`hello.c`), then build it up and fix it: date/time greeting, a `square()`
  loop, a forward-declaration bug forced to error with `gcc -Wall -Werror`,
  `sum_array()`, `factorial()` (fixing `factorial(0)`), `divide()` (fixing a
  divide-by-zero crash), a `Makefile`, `make clean`/`make`, and a final
  read-and-summarize. This exercises ~12 shell/compile runs, ~5 file edits, and
  three genuine error-fix loops.
- **Starting state:** each app got a fresh, empty working directory and a fresh
  session, so neither inherited history from the other.

### Measurement

The primary metric is DeepSeek's per-request `usage` block:

```json
{
  "prompt_tokens": 6429,
  "prompt_cache_hit_tokens": 384,
  "prompt_cache_miss_tokens": 6045,
  "completion_tokens": 91,
  "completion_tokens_details": { "reasoning_tokens": 41 },
  "total_tokens": 6531
}
```

`total_tokens = prompt_tokens + completion_tokens`; `reasoning_tokens` is a
subset of `completion_tokens` and is billed at the output rate.

- **gritcode** was instrumented (see below) to request
  `stream_options: {"include_usage": true}` and log each request's `usage` to
  stderr.
- **OpenCode** already sends `stream_options: {include_usage: true}`; its usage
  was captured with a small Python CONNECT MITM proxy that logged each response's
  `usage` block (the proxy only *observed* OpenCode's traffic — it did not modify
  requests, and OpenCode was pointed at it with `NODE_EXTRA_CA_CERTS` +
  `HTTPS_PROXY`).

---

## Changes required to run the test in gritcode

Three small, uncommitted source changes were added to make gritcode report the
same per-request usage that OpenCode reports natively. (These were removed again
after the test; they are documented here so the experiment is reproducible.)

### 1. Request per-request usage from DeepSeek

`src/chat_frame.cpp` — `ChatFrame::DoSendActualRequest()`, alongside the existing
request body:

```cpp
req["stream"] = true;
// Ask DeepSeek to include per-request "usage" in the final streamed chunk.
req["stream_options"] = {{"include_usage", true}};
```

### 2. Capture the `usage` block from the stream

`src/chat_frame.cpp` — `ChatFrame::OnStreamData()`, at the top of the SSE parse:

```cpp
auto j = nlohmann::json::parse(payload);
// Capture the final chunk's usage block (present when include_usage is set).
if (j.contains("usage") && j["usage"].is_object())
    lastUsage_ = j["usage"];
```

### 3. Log per-request + cumulative usage to stderr

`src/chat_frame.cpp` / `src/chat_frame.h` — a `ChatFrame::LogUsage(const
nlohmann::json&)` helper called from `OnStreamDone()`, printing:

```
=== USAGE prompt=1780 cache_hit=256 cache_miss=1524 completion=115 reasoning=29 total=1895 | cum req=1 ...
```

It accumulates `prompt_tokens`, `prompt_cache_hit_tokens`,
`prompt_cache_miss_tokens`, `completion_tokens`, and
`completion_tokens_details.reasoning_tokens` across the session. `lastUsage_` is
reset at the start of every completion so a failed/short stream can't leak a
stale block into the next turn.

### 4. (Test-only) disable grit-history tools

`src/preferences.cpp` — `Preferences::GetEnableGritHistory()` default was flipped
`true` → `false` so gritcode's tool definitions match OpenCode's tool surface.
Reverted after the test.

---

## Results

### Apples-to-apples run (20 turns, `reasoning_effort=high`, history tools off)

| Metric | OpenCode | gritcode | |
|---|---|---|---|
| Requests | 62 | 61 | tie |
| Prompt tokens | 713,038 | **391,214** | OpenCode 1.82× |
| Cache hit rate | 98.1% | 97.6% | tie |
| Cache-miss (full-price) tokens | 13,902 | **9,390** | OpenCode 1.48× |
| Completion tokens | 7,657 | 6,903 | ~tie |
| Reasoning tokens | 678 | 913 | ~tie (variance) |
| **Total tokens** | 720,695 | **398,117** | **gritcode 45% fewer** |

Estimated cost (DeepSeek V4 Pro list prices — input $0.435/M, cache read
$0.003625/M, output/reasoning $0.87/M):

- OpenCode ≈ **$0.0152**
- gritcode ≈ **$0.0115** (~24% cheaper)

### Key findings

1. **The entire gap is fixed per-request overhead.** OpenCode's system prompt +
   ~30 tool schemas cost ~6,400 tokens per request; gritcode's system prompt +
   7 tools cost ~1,780. That ~4,600-token tax is paid on *every* request and
   compounds as the session grows (neither app compacts below ~1M tokens; they
   use the same compaction constants).

2. **Prefix caching is identical (~98% hit).** gritcode's request prefix is
   byte-stable and benefits from DeepSeek's implicit prefix cache exactly as
   OpenCode does.

3. **Reasoning/completion are equal once effort is matched.** OpenCode sends no
   `reasoning_effort` by default; gritcode sends `"high"`. This is a real config
   difference, but its observed effect is small and swamped by sampling variance
   on open-ended "fix it" turns — gritcode's reasoning tokens varied 3,985 → 913
   across two otherwise-identical runs, OpenCode's 1,566 → 678.

### Warmup run (4 turns, short session)

For a short session the fixed overhead dominates even harder: gritcode used
16,193 prompt tokens vs OpenCode's 54,179 (3.3×).

---

## Caveats

- n=1 per app; completion and especially reasoning volumes have large sampling
  variance on open-ended tasks. The prompt-token and cache-hit conclusions are
  deterministic (fixed overhead + stable prefix), but the completion/reasoning
  columns should be read as "equal within noise".
- These are short sessions (20 turns); neither app triggers compaction below
  ~1M tokens, so multi-hour sessions should scale identically — with gritcode
  keeping its smaller fixed overhead.
- Cost figures use models.dev list prices and assume DeepSeek's implicit prefix
  cache behaves identically for both clients.
