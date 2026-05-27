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

DeepSeek V4 Flash costs **$0.20/M input** and **$0.80/M output** — roughly
1/15th of GPT-4.1 and 1/20th of Claude Sonnet 4.5. For daily coding loops,
that translates to cents per hour, not dollars.

| Model | Provider | API Key | Pricing |
|---|---|---|---|
| **OpenCode Free** | opencode.ai/zen | none | free |
| **DeepSeek V4 Flash** | api.deepseek.com | required | $0.20 / $0.80 |
| **DeepSeek V4 Pro** | api.deepseek.com | required | standard DeepSeek rates |

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

Pre-built installers are available on the
[Releases](https://github.com/lszl84/wx_gritcode/releases) page:

| Platform | Format |
|---|---|
| Linux | `.deb`, `.tar.gz`, `.AppImage` |
| macOS | `.dmg` |
| Windows | `.exe` (NSIS installer) |

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

### Windows (MSYS2 CLANG64)

```bash
pacman -S mingw-w64-clang-x86_64-toolchain \
    mingw-w64-clang-x86_64-cmake \
    mingw-w64-clang-x86_64-ninja \
    mingw-w64-clang-x86_64-wxwidgets3.2-msw
cmake --preset release
cmake --build --preset release
./build/wx_gritcode.exe
```

Or open the CMakeLists.txt in Visual Studio 2022 with the C++ CMake tools
component installed.

## API keys

Set your DeepSeek API key in Settings (`Ctrl+,`). The key is stored in your
OS keyring (libsecret on Linux, Keychain on macOS, Credential Manager on
Windows). The OpenCode Free model needs no key.

## License

GPLv3 — see [LICENSE](LICENSE).
