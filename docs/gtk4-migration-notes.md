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
- API key management modal is wired to keychain + model refresh.
- Model list fetches from backend `/models` and repopulates dropdown.
- Streaming cancel button is implemented.
- Basic fenced code-block styling and tool-call transcript rendering are implemented.
- Workspace chooser dialog is implemented (session list + open on activation).
- New session action and key shortcuts (`Ctrl+Enter`, `Ctrl+O`, `Ctrl+K`) are wired.
- Expandable status/details panel is implemented for request/debug visibility.
- Markdown pass now includes headings, bullets, inline code, and fenced blocks.
- Tool result messages now render as styled code blocks.

Next major milestones:
1. Port chooser mode details and interactions closer to GL behavior.
2. Port full tool-call execution/result UX parity (including action affordances).
3. Parity pass for keybindings, scrolling feel, and status/detail fields.
4. Keep original `grit` executable intact until parity is validated.
