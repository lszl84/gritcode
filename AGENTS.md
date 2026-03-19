# What is this

This is a multi-platform agent harness, working natively on Windows,
Mac, and Linux. Connects to OpenCode Zen. Meant as a fast, small and
lean replacement for bloated, react-based TUI applications.

# Guidelines

- Use CMake and FetchContent
- Try to minimize dependencies
- All dependencies, including curl, should be downloaded with FetchContent
- Use newest wxWidgets 3.3.x
- Use modern C++26 features, focusing on producing fast code.
  Templates, ranges::views, spans, move semantics, etc
- Use ninja for faster builds

# Reference

When in doubt, check opencode implementation. Cloned to ../opencode
for reference.

# License

GPL v3