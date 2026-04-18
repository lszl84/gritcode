# GTK4 migration notes (gtk4-wip)

Current status:
- Added `grit-gtk4` executable target.
- Added GTK4 shell window with native headerbar.
- Transcript view uses `GtkTextView/GtkTextBuffer` (native text rendering + native selection).
- Message controls use modern GTK4 widgets (`GtkDropDown`, `GtkEntry`, `GtkButton`).
- Added role-specific text tags for transcript blocks.
- Added Ctrl+Enter send shortcut.

Next major milestones:
1. Bridge backend App/Session/Curl pipeline into GTK4 frontend.
2. Replace placeholder send/reply with real request lifecycle.
3. Port chooser mode to GTK4 list model/view.
4. Port fine-grained markdown/code-block rendering semantics.
5. Parity pass for keybindings, selection behavior, scrolling, and status fields.
6. Keep original `grit` executable intact until parity is validated.
