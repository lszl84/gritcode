# FastCode Native

GPU-rendered AI coding harness. HarfBuzz + FreeType + OpenGL + GLFW.

## Build

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=~/.local
cmake --build build
cmake --install build
```

Binary: `~/.local/bin/fastcode-native`

## Dependencies

System: glfw3, freetype2, harfbuzz, fontconfig, libcurl, libsecret-1, OpenGL
Fetched: cmark, nlohmann_json

## Testing

- Default to **Zen provider** with **kimi-k2.5** for routine tests
- Only use **Claude ACP** when specifically testing Claude-specific features
- Launch on your workspace (not the user's) using the title marker trick in the terminal

## Code style

Clean, simple, long clear functions. No abstraction overload. Like harfbuzzscroll.
