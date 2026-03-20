# What is this

FastCode Native -- a multi-platform native AI coding harness, working
natively on Windows, Mac, and Linux. Connects to OpenCode Zen. Built as
a fast, small, lean replacement for bloated Electron/web-based TUI apps.

# Guidelines

- Use CMake and FetchContent
- Try to minimize dependencies
- All dependencies should be downloaded with FetchContent
- Use newest wxWidgets 3.3.x
- Use modern C++23 features, focusing on producing fast code.
  Templates, ranges::views, spans, move semantics, etc
- Use ninja for faster builds
- Flat source file layout in project root (no src/ subdirectories)

# Reference

When in doubt, check opencode implementation. Cloned to ../opencode
for reference.

# License

GPL v3
