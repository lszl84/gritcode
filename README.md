# Gritcode

Native desktop AI coding assistant. wxWidgets + C++20. No Electron, no browser tab.

Uses [DeepSeek V4](https://deepseek.com) (API key required) or the free [OpenCode Zen](https://opencode.ai/zen) tier (no key).

## Download

| Platform | Format | Link |
|---|---|---|
| Linux (Ubuntu/Debian) | `.deb` | [Latest release](https://github.com/lszl84/gritcode/releases/latest) |
| macOS | `.dmg` | [Latest release](https://github.com/lszl84/gritcode/releases/latest) |

## Features

- Streaming markdown — paragraphs, headings, fenced code blocks, inline formatting, tables
- ▶ Play button — one click builds and runs your project, no AI roundtrip
- Tool calls — the AI runs bash, reads/writes/edits files, lists directories, greps, fetches URLs
- Multi-session history with SQLite persistence and cross-project full-text search
- Collapsible tool-call cards and thinking blocks
- Native dark/light theme
- ~10 MB binary, ~50 MB RAM at rest — vs hundreds of MB for Electron alternatives (T3 Chat, OpenCode GUI, Claude Code GUI, etc.)

## Build from source

```bash
# Linux
sudo apt install build-essential cmake ninja-build pkg-config \
    libwxgtk3.2-dev libgtk-3-dev libcurl4-openssl-dev \
    libsqlite3-dev libsecret-1-dev
cmake --preset release
cmake --build --preset release
./build/gritcode

# macOS
brew install cmake ninja
cmake --preset release
cmake --build --preset release
open ./build/gritcode.app
```

## API keys

Set your DeepSeek API key in Settings (`Ctrl+,`). Stored in the OS keyring. OpenCode Free needs no key.

## License

GPLv3 — see [LICENSE](LICENSE).
