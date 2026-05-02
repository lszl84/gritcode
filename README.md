# wx_gritcode

A minimal cross-platform chat client for [opencode.ai/zen](https://opencode.ai/zen),
written in C++20 with [wxWidgets](https://www.wxwidgets.org/).

It is a much-simplified port of the rendering ideas from
[gritcode](https://github.com/lszl84/gritcode), built on top of native widgets:
a single custom-painted `wxScrolledCanvas` for the transcript, and stock
`wxTextCtrl` / `wxButton` for input. Markdown is parsed in a streaming fashion
but only emitted to the view at block boundaries, so finalized blocks are
immutable and the per-token cost is essentially zero. While a block is in flight
the canvas shows an animated thinking-dots indicator instead of mutating text.

The default model is `minimax-m2.5-free` over an anonymous connection — no API
key required.

## Features

- Streaming markdown rendering (paragraphs, headings, fenced code, inline
  `**bold**`, `*italic*`, `` `code` ``)
- Cross-block character-precise text selection and copy
- Native dark/light theme tracking via `wxSystemSettings` (recomputed only on
  `wxEVT_SYS_COLOUR_CHANGED`, not per paint)
- Update-region-aware repaint — scrolling only repaints the newly exposed strip
- Tiny-rect refresh for the thinking-dots animation (no full-canvas invalidate
  during streaming)

## What it doesn't do

This is a deliberately small prototype. There is no session chooser, no API-key
handling, no Claude ACP, no model picker, no tool calls, no syntax highlighting,
no list/table/blockquote rendering, no soft-wrap inside code blocks, and no
caret (selection-only, read-only).

## Build

The project uses CMake + Ninja. nlohmann/json v3.11.3 is always fetched via
`FetchContent`. wxWidgets is detected via `find_package` first; if no system
install is found, v3.2.6 is fetched and built statically. On Linux libcurl is
required (used by `wxWebRequest`'s backend) and is taken from the system.

If wxWidgets is fetched from source, the first configure takes **5–15 minutes**
depending on your machine. Installing the system wxWidgets package skips that
step entirely.

The build and install commands below are identical on every platform — that's
the whole point of using CMake. Only the dependency-install step differs.

### Install dependencies

The `wx*` package on each platform is optional but recommended — install it and
the build will use it directly instead of compiling wxWidgets from source.

#### Arch / Manjaro

```bash
sudo pacman -S --needed base-devel cmake ninja git curl gtk3 pkgconf wxwidgets-gtk3
```

#### Fedora / RHEL / Rocky

```bash
sudo dnf install gcc-c++ cmake ninja-build git libcurl-devel gtk3-devel wxGTK-devel
```

#### Debian / Ubuntu / Pop!\_OS / Mint

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git pkg-config \
    libcurl4-openssl-dev libgtk-3-dev libwxgtk3.2-dev
```

#### macOS

Requires the Xcode command-line tools (`xcode-select --install`) and
[Homebrew](https://brew.sh/).

```bash
brew install cmake ninja curl
```

The build produces a proper `.app` bundle thanks to the `MACOSX_BUNDLE` target
flag in `CMakeLists.txt`.

#### Windows

Requires [Visual Studio 2022](https://visualstudio.microsoft.com/) with the
"Desktop development with C++" workload (which includes MSVC, the Windows SDK,
and CMake), plus [Git for Windows](https://git-scm.com/download/win) and
[Ninja](https://github.com/ninja-build/ninja/releases) on `PATH`.

The simplest way to get libcurl is via [vcpkg](https://vcpkg.io/) in manifest
mode, or you can rely on the curl that ships with recent Windows 10/11. Run the
build from the **"x64 Native Tools Command Prompt for VS 2022"**.

If you use vcpkg, append
`-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake` to the
`cmake -S . -B build` line below. Otherwise point CMake at a libcurl install
via `-DCURL_ROOT=C:\path\to\curl`.

### Configure, build, install

```bash
git clone https://github.com/lszl84/wx_gritcode.git
cd wx_gritcode
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To install to a per-user prefix (Linux/macOS):

```bash
cmake --install build --prefix ~/.local
```

That puts the binary at `~/.local/bin/wx_gritcode` (or the `.app` bundle at
`~/.local/wx_gritcode.app` on macOS — pass `--prefix ~/Applications` instead if
you want it discoverable by Spotlight/Finder).

On Windows, install to a user-writable prefix and add its `bin\` to `%PATH%`:

```bat
cmake --install build --prefix %LOCALAPPDATA%\Programs\wx_gritcode
```

To run without installing:

```bash
cmake --build build --target wx_gritcode
./build/wx_gritcode          # Linux / Windows (use build\wx_gritcode.exe on cmd)
open ./build/wx_gritcode.app # macOS
```

## License

GPLv3 — see [LICENSE](LICENSE).
