#pragma once

// Imports a small set of environment variables (PATH and common toolchain
// helpers) from the user's interactive login shell into the current process,
// so that subsequently-forked children (the bash tool, web requests through
// system curl, etc.) see the same PATH the user sees in their terminal.
//
// Needed because GUI apps launched from a .desktop file or DE menu inherit a
// minimal env from the session manager — nvm/pyenv/cargo/linuxbrew shims that
// rely on rc-file `eval` lines aren't on PATH otherwise.
//
// Synchronous: runs `$SHELL -ilc 'printf ...'` with a 5s timeout. If the
// shell takes longer than that or the markers can't be parsed, the call is a
// no-op and the existing env is left alone.
void ImportShellEnv();
