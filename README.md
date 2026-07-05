# Gritcode

A native desktop AI coding assistant. Built with [wxWidgets](https://www.wxwidgets.org/),
backed by [DeepSeek V4](https://deepseek.com) — or run for free through
[OpenCode Zen](https://opencode.ai/zen).

## Why Gritcode?

Gritcode is a lean native desktop client — a custom-painted `wxScrolledCanvas`
for streaming markdown, a stock `wxTextCtrl` for input, and C++20 talking
directly to an LLM API. No Electron, no browser tab, no 300 MB download.
It starts instantly and stays out of your way.

### Building On Cheap

DeepSeek V4 Flash costs **$0.14/M input** and **$0.28/M output**. Claude Opus 4.7
charges **$5/M input** and **$25/M output**. That's **1/36th the input cost**
and **1/89th the output cost** — for daily coding loops, cents per hour
instead of dollars.

| Model | Provider | API Key | Input $/M | Output $/M |
|---|---|---|---|---|
| **OpenCode Free** | opencode.ai/zen | none | free | free |
| **DeepSeek V4 Flash** | api.deepseek.com | required | $0.14 | $0.28 |
| **DeepSeek V4 Pro** | api.deepseek.com | required | $0.44¹ | $0.87¹ |

¹ 75% promo through 2026-05-31. List price: $1.74 / $3.48.

The **OpenCode Free** tier uses `deepseek-v4-flash-free` with no sign-up —
just launch and go. For heavier work, drop in a DeepSeek API key.

## Features

- Streaming markdown — paragraphs, headings, fenced code blocks,
  inline `**bold**`, `*italic*`, `` `code` ``, tables, and tool-call cards
- Multi-session history with SQLite persistence and FTS5 full-text search
  across projects (`grit_history_search` / `grit_history_fetch`)
- Cross-block character-precise text selection, double-click word selection,
  and per-cell table selection
- Tool calls — the AI can execute bash commands, read/write/edit files,
  list directories, glob, grep, and fetch URLs
- Collapsible tool-call cards showing arguments and results inline
- Thinking/reasoning blocks (collapsed by default)
- Native dark/light theme tracking
- MCP stdio server — external agents can introspect selection, blocks, and
  session state; `grit_history_search` and `grit_history_fetch` are also
  exposed as MCP tools

## Download

Pre-built packages are available on the
[Releases](https://github.com/lszl84/wx_gritcode/releases) page:

| Platform | Format |
|---|---|
| Ubuntu / Debian | `.deb` |
| macOS | `.dmg` |

## Build from source

The build uses CMake with the Ninja generator. nlohmann/json is fetched
automatically. wxWidgets is detected via `find_package`; if not found,
v3.2.6 is built statically from source.

### Linux

```bash
# Install deps (Ubuntu/Debian)
sudo apt install build-essential cmake ninja-build pkg-config \
    libwxgtk3.2-dev libgtk-3-dev libcurl4-openssl-dev \
    libsqlite3-dev libsecret-1-dev

# Arch
sudo pacman -S base-devel cmake ninja git curl gtk3 pkgconf \
    wxwidgets-gtk3 sqlite3 libsecret

# Fedora
sudo dnf install gcc-c++ cmake ninja-build git libcurl-devel \
    gtk3-devel wxGTK-devel sqlite-devel libsecret-devel

# Build
cmake --preset release
cmake --build --preset release
./build/wx_gritcode
```

### macOS

```bash
brew install cmake ninja wxwidgets
cmake --preset release
cmake --build --preset release
open ./build/wx_gritcode.app
```

## API keys

Set your DeepSeek API key in Settings (`Ctrl+,`). The key is stored in your
OS keyring (libsecret on Linux, Keychain on macOS). The OpenCode Free model
needs no key.

## License

GPLv3 — see [LICENSE](LICENSE).
