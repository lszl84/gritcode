#pragma once

class wxFrame;
class wxWindow;
class wxAnyButton;
class ChatCanvas;

// macOS Tahoe look for the main window: transparent titlebar, one window
// colour, modern (non-glass) buttons. Touches only native styling — the wx
// layout is shared with GTK/MSW and never changed here. Widgets are found by
// type, so controls added to the shared layout pick the style up
// automatically. Set GRITCODE_TAHOE=0 to launch with the stock look.
struct MacStyleParts {
    wxFrame* frame;
    ChatCanvas* canvas;           // source of the window colour
    wxAnyButton* primaryButton;   // Send
    wxWindow* distinctPane;       // keeps the system window colour (session reference)
};

void ApplyMacStyle(const MacStyleParts& parts);

class wxFont;
// The system monospaced font (SF Mono) — what Terminal and Xcode use. wx's
// wxFONTFAMILY_TELETYPE maps to Courier on macOS.
wxFont MacMonospaceFont(int pointSize);
