# FastCode Native

## Testing the app

When running the GUI app for testing, always launch it on the **same Hyprland workspace** you are currently on, in **tiled mode** (not floating). Use `hyprctl activewindow` to find your current workspace, then launch with:

```bash
hyprctl dispatch exec "[workspace current silent]" ./build/fcn
```

This prevents the app window from appearing on the user's active workspace.

## Build

```bash
cmake -B build -G Ninja
cmake --build build
```

Binary: `build/fcn`

## MCP testing

- Default to **Zen provider** with **kimi-k2.5** for routine tests
- Only use **Claude ACP** when specifically testing Claude-specific features
- Keep Claude ACP tests minimal to avoid burning API credits
