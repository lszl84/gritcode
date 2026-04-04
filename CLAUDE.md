# FastCode Native

## Testing the app

When running the GUI app for testing, always launch it on **your own Hyprland workspace** (not the user's), in **tiled mode**. This prevents the window from popping up on the user's active workspace.

Find your workspace by looking for the "Claude Code" window title:

```bash
MY_WS=$(hyprctl clients -j | python3 -c "import sys,json;[print(c['workspace']['id']) for c in json.load(sys.stdin) if 'Claude' in c.get('title','')]" | head -1)
```

Then launch:

```bash
hyprctl dispatch exec "[workspace $MY_WS silent]" ./build/fcn
```

For MCP stdin-pipe tests, the window still opens — use the same workspace rule to keep it off the user's screen.

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
