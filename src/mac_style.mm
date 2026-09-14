#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <wx/wx.h>
#include <wx/anybutton.h>
#include <wx/bmpbuttn.h>
#include <wx/graphics.h>
#include <wx/renderer.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>
#include <wx/treectrl.h>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <set>
#include <vector>
#include "chat_canvas.h"
#include "mac_style.h"

// Thin auto-hiding scroll knob for wx's generic scrolled windows (chat, file
// tree), whose own scrollbars are always-visible legacy NSScrollers with a
// reserved gutter. Scrolls through the same wxScrollWinEvents a real
// scrollbar sends, so the window's own scroll handling runs unchanged.
@interface GritScrollKnob : NSView
@property (nonatomic, assign) wxWindow* win;
@property (nonatomic, assign) wxScrollHelper* helper;
- (void)updateGeometry;
- (void)reveal;
@end

@implementation GritScrollKnob {
    CALayer* _pill;
    BOOL _hovering;
    BOOL _dragging;
    unsigned _gen;
    CGFloat _dragStartY;
    int _dragStartPx;
    int _rangePx;   // scrollable distance, in pixels
    int _travel;    // knob travel along the track, in pixels
    int _ppu;       // scroll pixels per unit
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        _pill = [CALayer layer];
        [self.layer addSublayer:_pill];
        self.alphaValue = 0;
    }
    return self;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* a in [self.trackingAreas copy]) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow |
                     NSTrackingInVisibleRect
               owner:self
            userInfo:nil]];
}

// Pill drawn inside a wider hit area; it widens while hovered or dragged,
// like the system overlay scroller.
- (void)layoutPill {
    const CGFloat w = (_hovering || _dragging) ? 8 : 6;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _pill.frame = CGRectMake(NSWidth(self.bounds) - w - 2, 0, w, NSHeight(self.bounds));
    _pill.cornerRadius = w / 2;
    [CATransaction commit];
}

- (void)reveal {
    CALayer* pill = _pill;
    [self.effectiveAppearance performAsCurrentDrawingAppearance:^{
        pill.backgroundColor = [NSColor.labelColor colorWithAlphaComponent:0.4].CGColor;
    }];
    self.alphaValue = 1;
    const unsigned gen = ++_gen;
    __weak GritScrollKnob* weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.2 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        GritScrollKnob* k = weak;
        if (!k || k->_gen != gen || k->_hovering || k->_dragging) return;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
            ctx.duration = 0.35;
            k.animator.alphaValue = 0;
        }];
    });
}

// Position from the window's scroll state; hidden when nothing scrolls.
- (void)updateGeometry {
    int ppuX = 0, ppuY = 0, vx = 0, vy = 0;
    self.helper->GetScrollPixelsPerUnit(&ppuX, &ppuY);
    self.helper->GetViewStart(&vx, &vy);
    const wxSize client = self.win->GetClientSize();
    const int virtH = self.win->GetVirtualSize().y;
    if (ppuY <= 0 || virtH <= client.y) {
        self.hidden = YES;
        return;
    }
    self.hidden = NO;
    const int inset = 3;
    const int track = client.y - 2 * inset;
    const int knobH = std::max(32, (int)((double)track * client.y / virtH));
    _ppu = ppuY;
    _rangePx = virtH - client.y;
    _travel = std::max(1, track - knobH);
    const double frac = std::clamp((double)(vy * ppuY) / _rangePx, 0.0, 1.0);
    const NSRect f = NSMakeRect(client.x - 12, inset + _travel * frac, 12, knobH);
    if (!NSEqualRects(self.frame, f)) self.frame = f;
    [self layoutPill];
}

- (void)mouseEntered:(NSEvent*)event { _hovering = YES; [self layoutPill]; [self reveal]; }
- (void)mouseExited:(NSEvent*)event { _hovering = NO; [self layoutPill]; [self reveal]; }

- (CGFloat)yInSuperview:(NSEvent*)event {
    return [self.superview convertPoint:event.locationInWindow fromView:nil].y;
}

- (void)sendScroll:(wxEventType)type position:(int)pos {
    wxScrollWinEvent ev(type, pos, wxVERTICAL);
    ev.SetEventObject(self.win);
    self.win->GetEventHandler()->ProcessEvent(ev);
    [self updateGeometry];
}

- (void)mouseDown:(NSEvent*)event {
    _dragging = YES;
    _dragStartY = [self yInSuperview:event];
    int vx = 0, vy = 0;
    self.helper->GetViewStart(&vx, &vy);
    _dragStartPx = vy * std::max(_ppu, 1);
    [self layoutPill];
    [self reveal];
}

- (void)mouseDragged:(NSEvent*)event {
    const CGFloat dy = [self yInSuperview:event] - _dragStartY;
    const int px = std::clamp(_dragStartPx + (int)(dy * _rangePx / _travel), 0, _rangePx);
    [self sendScroll:wxEVT_SCROLLWIN_THUMBTRACK position:px / std::max(_ppu, 1)];
}

- (void)mouseUp:(NSEvent*)event {
    _dragging = NO;
    int vx = 0, vy = 0;
    self.helper->GetViewStart(&vx, &vy);
    [self sendScroll:wxEVT_SCROLLWIN_THUMBRELEASE position:vy];
    [self layoutPill];
    [self reveal];
}
@end

// wx's macOS renderer draws tree expanders with the Carbon HITheme disclosure
// triangle, which comes out black on dark backgrounds. Draw a small vector
// chevron in the secondary label colour instead; every other control keeps
// the native renderer.
class ChevronTreeRenderer : public wxDelegateRendererNative {
public:
    // The default wxDelegateRendererNative forwards to the *generic* renderer.
    ChevronTreeRenderer() : wxDelegateRendererNative(wxRendererNative::GetDefault()) {}

    void DrawTreeItemButton(wxWindow* win, wxDC& dc, const wxRect& rect,
                            int flags) override {
        wxGraphicsContext* gc = dc.GetGraphicsContext();
        if (!gc) {
            wxDelegateRendererNative::DrawTreeItemButton(win, dc, rect, flags);
            return;
        }
        __block CGFloat r = 0, g = 0, b = 0, a = 1;
        NSView* view = (NSView*)win->GetHandle();
        [view.effectiveAppearance performAsCurrentDrawingAppearance:^{
            NSColor* c = [NSColor.secondaryLabelColor
                colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
            r = c.redComponent; g = c.greenComponent; b = c.blueComponent; a = c.alphaComponent;
        }];
        const wxColour colour(r * 255, g * 255, b * 255, a * 255);

        // Proportions of a 9px box (the generic tree's button size).
        const double k = rect.width / 9.0;
        const double cx = rect.x + rect.width / 2.0, cy = rect.y + rect.height / 2.0;
        wxPoint2DDouble pts[3];
        if (flags & wxCONTROL_EXPANDED) {   // pointing down
            pts[0] = {cx - 3 * k, cy - 1.5 * k};
            pts[1] = {cx, cy + 1.5 * k};
            pts[2] = {cx + 3 * k, cy - 1.5 * k};
        } else {                            // pointing right
            pts[0] = {cx - 1.5 * k, cy - 3 * k};
            pts[1] = {cx + 1.5 * k, cy};
            pts[2] = {cx - 1.5 * k, cy + 3 * k};
        }
        gc->SetPen(gc->CreatePen(
            wxGraphicsPenInfo(colour).Width(1.5).Cap(wxCAP_ROUND).Join(wxJOIN_ROUND)));
        gc->StrokeLines(3, pts);
    }
};

namespace {

struct StyleState {
    MacStyleParts p;
    wxColour applied;              // window colour currently applied
    std::set<wxWindow*> coloured;  // windows we coloured (vs. ChatFrame's own)
    std::vector<__weak GritScrollKnob*> knobs;
};

NSColor* ToNS(const wxColour& c) {
    return [NSColor colorWithSRGBRed:c.Red() / 255.0 green:c.Green() / 255.0
                                blue:c.Blue() / 255.0 alpha:1.0];
}

void ForEachDescendant(wxWindow* w, const std::function<void(wxWindow*)>& fn) {
    for (wxWindow* c : w->GetChildren()) {
        fn(c);
        ForEachDescendant(c, fn);
    }
}

// Titlebar blends into the window: no separate strip colour, no separator
// line. The title itself stays (ChatFrame::UpdateWindowTitle keeps it in sync
// with the open file, as on other platforms), and the native titlebar keeps
// dragging and double-click behaving as usual.
void ApplyTitlebar(NSWindow* win) {
    win.titlebarAppearsTransparent = YES;
    win.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
}

// One window colour: every container surface takes the chat canvas's
// background so the window reads as a single surface. Re-run on theme change.
void ApplyWindowColour(StyleState& s) {
    const MacStyleParts& p = s.p;
    const wxColour bg = p.canvas->GetBackgroundColour();
    NSWindow* win = (NSWindow*)p.frame->MacGetTopLevelWindowRef();
    win.backgroundColor = ToNS(bg);  // what the transparent titlebar shows
    p.frame->SetBackgroundColour(bg);

    // The session-reference pane stays a distinct surface: its whole area
    // takes the system window colour its canvas already uses.
    if (p.distinctPane) {
        p.distinctPane->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
        p.distinctPane->Refresh();
    }

    ForEachDescendant(p.frame, [&](wxWindow* w) {
        if (dynamic_cast<ChatCanvas*>(w)) return;  // canvases paint their own palette
        if (p.distinctPane && (w == p.distinctPane || p.distinctPane->IsDescendant(w)))
            return;
        if (!wxDynamicCast(w, wxPanel) && !wxDynamicCast(w, wxSplitterWindow) &&
            !wxDynamicCast(w, wxTreeCtrl))
            return;
        // Leave surfaces ChatFrame coloured on purpose (e.g. queue chips).
        const bool ours = s.coloured.count(w) && w->GetBackgroundColour() == s.applied;
        if (w->UseBgCol() && !ours) return;
        w->SetBackgroundColour(bg);
        w->Refresh();
        s.coloured.insert(w);
    });
    s.applied = bg;
    p.frame->Refresh();
}

// Send: accent-filled capsule. Icon buttons: plain until hovered, then the
// standard toolbar bezel — no glass.
void ApplyButtons(const MacStyleParts& p) {
    // Legacy bezel names (Rounded = Push, TexturedRounded = Toolbar) so this
    // also compiles against the older SDK the CI runner builds with.
    NSButton* send = (NSButton*)p.primaryButton->GetHandle();
    if ([send isKindOfClass:NSButton.class]) {
        send.bezelStyle = NSBezelStyleRounded;
        send.bezelColor = NSColor.controlAccentColor;
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        if (@available(macOS 26.0, *)) {
            send.borderShape = NSControlBorderShapeCapsule;
            send.tintProminence = NSTintProminencePrimary;
        }
#endif
    }
    ForEachDescendant(p.frame, [](wxWindow* w) {
        if (!wxDynamicCast(w, wxBitmapButton)) return;
        NSButton* b = (NSButton*)w->GetHandle();
        if (![b isKindOfClass:NSButton.class]) return;
        b.bordered = YES;
        b.bezelStyle = NSBezelStyleTexturedRounded;
        b.showsBorderOnlyWhileMouseInside = YES;
    });
}

// Native scroll views (input, editor): thin overlay scrollers that hide when
// idle. AppKit resets the style when the system preference changes, so this
// is re-applied from that notification too.
void ApplyNativeOverlayScrollers(wxWindow* frame) {
    ForEachDescendant(frame, [](wxWindow* w) {
        NSView* v = (NSView*)w->GetHandle();
        if (![v isKindOfClass:NSScrollView.class]) return;
        NSScrollView* sv = (NSScrollView*)v;
        sv.scrollerStyle = NSScrollerStyleOverlay;
        sv.autohidesScrollers = YES;
    });
}

// wx's generic scrolled windows: drop the legacy scroller (and its gutter)
// and overlay a GritScrollKnob. Found by type, so scrolled views added to the
// shared layout later get the same treatment.
void ApplyScrollKnobs(StyleState& s) {
    ForEachDescendant(s.p.frame, [&](wxWindow* w) {
        auto* sh = dynamic_cast<wxScrollHelper*>(w);
        if (!sh) return;
        // Scrolling is driven by the helper's position, not the scroller, so
        // it keeps working (wheel, trackpad, keyboard, our knob) with none.
        sh->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
        GritScrollKnob* k = [[GritScrollKnob alloc] initWithFrame:NSZeroRect];
        k.win = w;
        k.helper = sh;
        [(NSView*)w->GetHandle() addSubview:k];
        s.knobs.push_back(k);

        // User-initiated scrolling shows the knob; programmatic scrolling
        // (following streamed output) only moves it.
        __weak GritScrollKnob* weak = k;
        auto userScrolled = [w, weak](wxEvent& e) {
            e.Skip();
            w->CallAfter([weak] {
                if (GritScrollKnob* kk = weak) { [kk updateGeometry]; [kk reveal]; }
            });
        };
        w->Bind(wxEVT_MOUSEWHEEL, userScrolled);
        for (auto t : {wxEVT_SCROLLWIN_LINEUP, wxEVT_SCROLLWIN_LINEDOWN,
                       wxEVT_SCROLLWIN_PAGEUP, wxEVT_SCROLLWIN_PAGEDOWN,
                       wxEVT_SCROLLWIN_TOP, wxEVT_SCROLLWIN_BOTTOM})
            w->Bind(t, userScrolled);
        // Pointer near the edge shows the knob so it can be grabbed.
        w->Bind(wxEVT_MOTION, [w, weak](wxMouseEvent& e) {
            e.Skip();
            if (e.GetX() >= w->GetClientSize().x - 16)
                if (GritScrollKnob* kk = weak) [kk reveal];
        });
    });
}

}  // namespace

wxFont MacMonospaceFont(int pointSize) {
    return wxFont([NSFont monospacedSystemFontOfSize:pointSize weight:NSFontWeightRegular]);
}

void ApplyMacStyle(const MacStyleParts& parts) {
    if (const char* e = std::getenv("GRITCODE_TAHOE"); e && e[0] == '0') return;
    auto s = std::make_shared<StyleState>();
    s->p = parts;

    ApplyTitlebar((NSWindow*)parts.frame->MacGetTopLevelWindowRef());
    ApplyWindowColour(*s);
    ApplyButtons(parts);
    ApplyNativeOverlayScrollers(parts.frame);
    ApplyScrollKnobs(*s);

    // App-wide and only once: Set() takes ownership, returns any previous one.
    static bool chevronsInstalled = false;
    if (!chevronsInstalled) {
        delete wxRendererNative::Set(new ChevronTreeRenderer);
        chevronsInstalled = true;
    }

    // Keep knobs in step with content/size changes (cheap: a few views).
    parts.frame->Bind(wxEVT_IDLE, [s](wxIdleEvent& e) {
        e.Skip();
        for (GritScrollKnob* k : s->knobs) [k updateGeometry];
    });

    wxFrame* frame = parts.frame;
    [NSNotificationCenter.defaultCenter
        addObserverForName:NSPreferredScrollerStyleDidChangeNotification
                    object:nil
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification*) {
        dispatch_async(dispatch_get_main_queue(), ^{ ApplyNativeOverlayScrollers(frame); });
    }];

    // The canvas rebuilds its palette on theme change; follow it afterwards.
    parts.frame->Bind(wxEVT_SYS_COLOUR_CHANGED, [s](wxSysColourChangedEvent& e) {
        e.Skip();
        s->p.frame->CallAfter([s] { ApplyWindowColour(*s); });
    });
}
