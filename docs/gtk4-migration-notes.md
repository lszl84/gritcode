# GTK4 migration notes (gtk4-wip)

Current status:
- Added `grit-gtk4` executable target.
- Added GTK4 shell window with native headerbar.
- Transcript view uses `GtkTextView/GtkTextBuffer` (native text rendering + native selection).
- Message controls use modern GTK4 widgets (`GtkDropDown`, `GtkEntry`, `GtkButton`).
- Added role-specific text tags for transcript blocks.
- Added Ctrl+Enter send shortcut.
- Session manager is wired: workspace switch/load/save works in GTK4 path.
- Streaming backend is wired to `CurlHttpClient` with live chunk updates.
- API key management dialog is wired to keychain + model refresh.
- Model list fetches from backend `/models` and repopulates dropdown.

Next major milestones:
1. Port chooser mode UX from GL path to GTK4 list UI parity.
2. Port tool-call rendering blocks and tool result formatting parity.
3. Port markdown/code-block rendering semantics (or replace with GTK source view).
4. Parity pass for keybindings, scrolling feel, and status/detail fields.
5. Keep original `grit` executable intact until parity is validated.
