# Gritcode

Native desktop AI coding assistant. wxWidgets + C++20. No Electron, no browser tab.

Uses [DeepSeek V4](https://deepseek.com) or the free [OpenCode Zen](https://opencode.ai/zen) tier (no API key needed).

## Download

| Platform | Format | Link |
|---|---|---|
| Linux (Ubuntu/Debian) | `.deb` | [Latest release](https://github.com/lszl84/gritcode/releases/latest) |
| macOS | `.dmg` | [Latest release](https://github.com/lszl84/gritcode/releases/latest) |

## Features

- Streaming markdown — paragraphs, headings, fenced code blocks, inline formatting, tables
- Tool calls — the AI runs bash, reads/writes/edits files, lists directories, greps, fetches URLs
- Multi-session history with SQLite persistence and cross-project full-text search
- Collapsible tool-call cards and thinking blocks
- Native dark/light theme

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
