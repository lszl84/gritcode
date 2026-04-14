# FastCode Native

GPU-rendered AI coding harness. HarfBuzz + FreeType + OpenGL + GLFW.

## Build

### Release (the user's daily driver)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=~/.local
cmake --build build
cmake --install build
```

Installs to `~/.local/bin/fcn`.

### Debug (required for agent work — see below)

```bash
cmake -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=~/.local
cmake --build build-debug
```

Run directly from `build-debug/fcn` — don't install it, the user's `~/.local/bin/fcn` should stay Release.

## Dependencies

System: glfw3, freetype2, harfbuzz, fontconfig, libcurl, libsecret-1, OpenGL
Fetched: cmark, nlohmann_json

## Agent rule: always build and run in Debug

**Always use the Debug build for any work, testing, or MCP automation, unless the user explicitly asks for a Release build.** Release builds compile out the MCP server entirely (smaller binary, no listening socket, no attack surface), which means `scripts/mcp_client.py` cannot drive them — every agent-side test loop depends on the Debug build being the one that's actually running.

Concretely: build with `cmake --build build-debug`, launch `build-debug/fcn`, and point MCP clients at it. Don't stomp `~/.local/bin/fcn` (that's the user's own Release binary) unless the user asks.

## Testing

- Default to **Zen provider** with **kimi-k2.5** for routine tests
- Only use **Claude ACP** when specifically testing Claude-specific features
- Launch on your workspace (not the user's) using the title marker trick in the terminal

Important: at the start of every session, check which hyprland workspace the harness is running on (`hyprctl activeworkspace -j`) and always launch fcn on that same workspace. Never let it spawn on an arbitrary workspace — the user is working elsewhere and shouldn't be interrupted by a window popping up in their face.

## Code style

Clean, simple, long clear functions. No abstraction overload. Like harfbuzzscroll.
