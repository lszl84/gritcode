# FastCode Native

## Testing the app

When running the GUI app for testing, always launch it on **your own Hyprland workspace** (not the user's), in **tiled mode**. This prevents the window from popping up on the user's active workspace.

Find your workspace by setting a temporary unique title on your terminal and matching it in Hyprland (works even with multiple Claude Code terminals):

```bash
_PTY=$(readlink /proc/$PPID/fd/0)
_M="fcn-detect-$$"
printf '\033]2;%s\007' "$_M" > "$_PTY"; sleep 0.2
MY_WS=$(hyprctl clients -j | python3 -c "import sys,json;[print(c['workspace']['id']) for c in json.load(sys.stdin) if c.get('title','')=='"$_M"']" | head -1)
printf '\033]2;Claude Code\007' > "$_PTY"
```

Then launch for visual testing (standalone, on your workspace):

```bash
hyprctl dispatch exec "[workspace $MY_WS silent]" $PWD/build/fcn
```

For MCP stdin-pipe tests, use `GDK_BACKEND=x11 xvfb-run` to avoid any visible window, or simply run headless and check JSON output only (no screenshots needed for functional tests).

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
