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

Important: at the start of every session, check which hyprland workspace the harness is running on (`hyprctl activeworkspace -j`) and always launch fcn on that same workspace. Never let it spawn on an arbitrary workspace — the user is working elsewhere and shouldn't be interrupted by a window popping up in their face.

## Code style

Clean, simple, long clear functions. No abstraction overload. Like harfbuzzscroll.
