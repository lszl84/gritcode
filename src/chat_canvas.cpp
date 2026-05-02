#include "chat_canvas.h"
#include "perf_log.h"
#include <wx/dcbuffer.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/settings.h>
#include <algorithm>
#include <cmath>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

namespace {

constexpr int kSideMargin    = 24;
constexpr int kTopMargin     = 16;
constexpr int kBottomMargin  = 24;
constexpr int kBlockSpacing  = 10;
constexpr int kHeadingSpacing = 18;
constexpr int kCodePadding   = 12;
constexpr int kUserBubblePad = 12;
constexpr int kMaxContentW   = 720;  // chat-style center column
constexpr int kTableCellPadX = 8;
constexpr int kTableCellPadY = 4;
constexpr int kTableMinColW  = 30;   // floor on per-column text width
constexpr int kToolPadX      = 10;
constexpr int kToolPadY      = 6;
constexpr int kToolGap       = 4;    // space between header and body (when expanded)

bool IsDarkMode() {
    // wxWidgets 3.2: explicit dark-mode signal where the platform exposes one.
    auto appearance = wxSystemSettings::GetAppearance();
    if (appearance.IsDark()) return true;
    if (appearance.IsUsingDarkBackground()) return true;
    // Fallback: compare luminance of the system window background.
    wxColour winBg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    int lum = (winBg.Red() * 299 + winBg.Green() * 587 + winBg.Blue() * 114) / 1000;
    return lum < 128;
}

Palette MakePalette() {
    Palette p;
    if (IsDarkMode()) {
        p.bg            = wxColour( 34,  34,  38);
        p.text          = wxColour(255, 255, 255);
        p.codeBg        = wxColour( 44,  44,  50);
        p.codeFg        = wxColour(210, 215, 225);
        p.userBubbleBg  = wxColour( 50,  72, 110);
        p.selectionBg   = wxColour( 60,  90, 150);
        p.thinkingDot   = wxColour(170, 170, 180);
        p.tableBorder   = wxColour( 90,  90, 100);
        p.tableHeaderBg = wxColour( 44,  44,  52);
        p.toolHeaderBg  = wxColour( 50,  56,  70);
        p.toolBodyBg    = wxColour( 40,  42,  50);
        p.toolAccent    = wxColour(140, 200, 255);
        p.toolDim       = wxColour(140, 145, 158);
    } else {
        p.bg            = wxColour(248, 248, 248);
        p.text          = wxColour( 28,  28,  32);
        p.codeBg        = wxColour(238, 238, 240);
        p.codeFg        = wxColour( 48,  48,  60);
        p.userBubbleBg  = wxColour(220, 232, 255);
        p.selectionBg   = wxColour(170, 200, 255);
        p.thinkingDot   = wxColour(120, 120, 128);
        p.tableBorder   = wxColour(180, 180, 188);
        p.tableHeaderBg = wxColour(232, 232, 236);
        p.toolHeaderBg  = wxColour(228, 235, 245);
        p.toolBodyBg    = wxColour(244, 246, 250);
        p.toolAccent    = wxColour( 30,  90, 170);
        p.toolDim       = wxColour(120, 130, 145);
    }
    return p;
}

inline int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// Tokenize runs into atomic words and single-space tokens, tagging each with
// its style and starting offset in `visibleText` (the concatenated run text).
struct Tok {
    wxString text;
    bool isSpace = false;
    bool isNewline = false;
    InlineRun style;
    int srcStart = 0;
};

std::vector<Tok> Tokenize(const std::vector<InlineRun>& runs) {
    std::vector<Tok> toks;
    int off = 0;
    for (const auto& r : runs) {
        size_t i = 0;
        while (i < r.text.size()) {
            if (r.text[i] == '\n') {
                Tok t;
                t.isNewline = true;
                t.style = r;
                t.style.text.Clear();
                t.srcStart = off + (int)i;
                toks.push_back(t);
                ++i;
            } else if (r.text[i] == ' ') {
                Tok t;
                t.text = " ";
                t.isSpace = true;
                t.style = r;
                t.style.text.Clear();
                t.srcStart = off + (int)i;
                toks.push_back(t);
                ++i;
            } else {
                size_t j = i;
                while (j < r.text.size() && r.text[j] != ' ' && r.text[j] != '\n') ++j;
                Tok t;
                t.text = r.text.SubString(i, j - 1);
                t.isSpace = false;
                t.style = r;
                t.style.text.Clear();
                t.srcStart = off + (int)i;
                toks.push_back(t);
                i = j;
            }
        }
        off += (int)r.text.size();
    }
    return toks;
}

// True if two run-styles match in formatting (text content ignored).
bool SameStyle(const InlineRun& a, const InlineRun& b) {
    return a.bold == b.bold && a.italic == b.italic && a.code == b.code;
}

}  // namespace

wxBEGIN_EVENT_TABLE(ChatCanvas, wxScrolledCanvas)
    EVT_PAINT(ChatCanvas::OnPaint)
    EVT_SIZE(ChatCanvas::OnSize)
    EVT_LEFT_DOWN(ChatCanvas::OnLeftDown)
    EVT_LEFT_UP(ChatCanvas::OnLeftUp)
    EVT_MOTION(ChatCanvas::OnMotion)
    EVT_KEY_DOWN(ChatCanvas::OnKeyDown)
    EVT_TIMER(wxID_ANY, ChatCanvas::OnAnimTick)
    EVT_SYS_COLOUR_CHANGED(ChatCanvas::OnSysColourChanged)
    EVT_SCROLLWIN(ChatCanvas::OnScrollWin)
    EVT_MOUSEWHEEL(ChatCanvas::OnMouseWheel)
wxEND_EVENT_TABLE()

ChatCanvas::ChatCanvas(wxWindow* parent)
    : wxScrolledCanvas(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxBORDER_NONE | wxVSCROLL),
      animTimer_(this) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    RebuildPalette();
    SetScrollRate(0, 16);
    EnableScrolling(false, true);

#ifdef __WXGTK__
    // Disable GTK theme animations on this widget. Some GTK themes drive
    // continuous frame-clock invalidations on widgets that are merely under
    // the mouse cursor (CSS hover transitions, focus pulse, etc.), which
    // shows up as a 20–40 Hz idle paint storm on a custom-painted canvas.
    if (GtkWidget* w = GetHandle()) {
        if (GtkSettings* s = gtk_widget_get_settings(w)) {
            g_object_set(s, "gtk-enable-animations", FALSE, NULL);
        }
    }
#endif
}

void ChatCanvas::RebuildPalette() {
    palette_ = MakePalette();
    SetBackgroundColour(palette_.bg);
}

void ChatCanvas::EnsureFonts() {
    if (fontsReady_) return;
    fontBody_ = wxFont(wxFontInfo(11).Family(wxFONTFAMILY_DEFAULT));
    fontBodyBold_ = wxFont(wxFontInfo(11).Family(wxFONTFAMILY_DEFAULT).Bold());
    fontBodyItalic_ = wxFont(wxFontInfo(11).Family(wxFONTFAMILY_DEFAULT).Italic());
    fontBodyBoldItalic_ = wxFont(wxFontInfo(11).Family(wxFONTFAMILY_DEFAULT).Bold().Italic());
    fontCode_ = wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));

    static const int hSizes[6] = {20, 17, 14, 13, 12, 11};
    for (int i = 0; i < 6; ++i) {
        fontH_[i]  = wxFont(wxFontInfo(hSizes[i]).Family(wxFONTFAMILY_DEFAULT).Bold());
        fontHB_[i] = wxFont(wxFontInfo(hSizes[i]).Family(wxFONTFAMILY_DEFAULT).Bold());
    }
    fontsReady_ = true;
}

const wxFont& ChatCanvas::FontFor(const InlineRun& r, BlockType bt, int hLvl) const {
    if (r.code) return fontCode_;
    if (bt == BlockType::Heading) {
        int idx = ClampInt(hLvl - 1, 0, 5);
        return fontH_[idx];
    }
    if (r.bold && r.italic) return fontBodyBoldItalic_;
    if (r.bold) return fontBodyBold_;
    if (r.italic) return fontBodyItalic_;
    return fontBody_;
}

void ChatCanvas::ToggleToolCall(int blockIdx) {
    if (blockIdx < 0 || blockIdx >= (int)blocks_.size()) return;
    Block& b = blocks_[blockIdx];
    if (b.type != BlockType::ToolCall) return;
    b.toolExpanded = !b.toolExpanded;
    // Only this one block's height changed. Invalidate it and let Relayout
    // patch the offset table — every other block keeps its layout cache.
    b.cachedWidth = -1;
    layoutDirty_ = true;
    Relayout(GetClientSize().x);
    Refresh();
}

void ChatCanvas::AddBlock(Block b) {
    // The new block hasn't been laid out yet; the existing blocks are already
    // laid out at the current width. Relayout's per-block guard makes this
    // O(1) instead of O(N) per AddBlock — important during streaming where
    // blocks land in rapid succession.
    blocks_.push_back(std::move(b));
    layoutDirty_ = true;
    Relayout(GetClientSize().x);
    if (stickToBottom_) {
        int xu, yu;
        GetScrollPixelsPerUnit(&xu, &yu);
        if (yu > 0) Scroll(0, contentHeight_ / yu);
    }
    Refresh();
}

void ChatCanvas::UpdateStickToBottom() {
    int xu, yu;
    GetScrollPixelsPerUnit(&xu, &yu);
    if (yu <= 0) { stickToBottom_ = true; return; }
    int vx, vy;
    GetViewStart(&vx, &vy);
    int viewY = vy * yu;
    int clientH = GetClientSize().y;
    int virtualH = GetVirtualSize().y;
    // ~2 lines of slack so the user doesn't have to land pixel-perfect.
    const int kThreshold = 40;
    stickToBottom_ = (viewY + clientH >= virtualH - kThreshold);
}

void ChatCanvas::OnScrollWin(wxScrollWinEvent& e) {
    e.Skip();
    CallAfter([this]{ UpdateStickToBottom(); });
}

void ChatCanvas::OnMouseWheel(wxMouseEvent& e) {
    e.Skip();
    CallAfter([this]{ UpdateStickToBottom(); });
}

void ChatCanvas::SetThinking(bool on) {
    if (thinking_ == on) return;
    thinking_ = on;
    if (on) {
        animTimer_.Start(80);
    } else {
        animTimer_.Stop();
    }
    // Thinking dots are positioned below the last block; toggling them only
    // changes total content height, not any block's layout. Bump the virtual
    // height directly instead of forcing a full relayout.
    int extra = on ? (kBlockSpacing + 24) : -(kBlockSpacing + 24);
    contentHeight_ += extra;
    if (layoutWidth_ > 0) SetVirtualSize(layoutWidth_, contentHeight_);
    Refresh();
}

void ChatCanvas::Clear() {
    blocks_.clear();
    selAnchor_ = selCaret_ = {};
    layoutWidth_ = -1;
    contentHeight_ = 0;
    layoutDirty_ = true;
    blockTops_.clear();
    Refresh();
}

void ChatCanvas::OnAnimTick(wxTimerEvent&) {
    animPhase_ += 0.12;
    if (dotsRectValid_) {
        // Only the 3 dots change between ticks — invalidate just that rect
        // (converted from canvas to client coords) instead of the whole window.
        int sx, sy;
        CalcScrolledPosition(dotsRect_.x, dotsRect_.y, &sx, &sy);
        RefreshRect(wxRect(sx, sy, dotsRect_.width, dotsRect_.height));
    } else {
        Refresh();
    }
}

void ChatCanvas::OnSize(wxSizeEvent& e) {
    layoutWidth_ = -1;
    layoutDirty_ = true;
    Refresh();
    e.Skip();
}

// ---------- Layout ----------

void ChatCanvas::LayoutBlock(wxDC& dc, Block& b, int contentWidth, int /*topSpacing*/) const {
    PERF_SCOPE_T("LayoutBlock", 2000);
    b.lines.clear();
    b.cachedWidth = contentWidth;

    const int hLvl = b.headingLevel;

    if (b.type == BlockType::CodeBlock) {
        // Source text already excludes the fences themselves (md_parser strips
        // them). Trim trailing newline so we don't render an empty last line.
        wxString src = b.rawText;
        if (!src.IsEmpty() && src.Last() == '\n') src.RemoveLast();
        int maxW = contentWidth - 2 * kCodePadding;
        if (maxW < 50) maxW = 50;
        WrapMonospace(dc, src, maxW, b.lines, 0);
        int h = kCodePadding * 2;
        for (const auto& wl : b.lines) h += wl.height;
        b.cachedHeight = h;
        return;
    }

    if (b.type == BlockType::Table) {
        const int N = (int)b.tableAligns.size();
        if (N == 0 || b.tableRows.empty()) {
            b.cachedHeight = 0;
            return;
        }

        // Per-column natural (single-line) widths — max across all rows.
        std::vector<int> prefCol(N, 0);
        auto cellNatural = [&](const TableCell& c, bool header) {
            // Tokenize directly off cell runs and sum widths. Bold for header.
            int w = 0;
            std::vector<InlineRun> tmp = c.runs;
            if (header) for (auto& r : tmp) r.bold = true;
            auto toks = Tokenize(tmp);
            for (auto& t : toks) {
                const wxFont& f = FontFor(t.style, BlockType::Paragraph, 0);
                dc.SetFont(f);
                wxCoord ww = 0, hh = 0;
                dc.GetTextExtent(t.text, &ww, &hh);
                w += ww;
            }
            return w;
        };
        for (size_t r = 0; r < b.tableRows.size(); ++r) {
            for (int c = 0; c < N && c < (int)b.tableRows[r].size(); ++c) {
                int w = cellNatural(b.tableRows[r][c], r == 0);
                if (w > prefCol[c]) prefCol[c] = w;
            }
        }

        // Allocate text widths proportionally. Floor each at kTableMinColW.
        const int gridOverhead = (N + 1) + 2 * kTableCellPadX * N;
        int availTextW = contentWidth - gridOverhead;
        if (availTextW < kTableMinColW * N) availTextW = kTableMinColW * N;

        long sumPref = 0;
        for (int c = 0; c < N; ++c) sumPref += std::max(1, prefCol[c]);

        std::vector<int> textW(N);
        int used = 0;
        for (int c = 0; c < N - 1; ++c) {
            int w = (int)((double)availTextW * std::max(1, prefCol[c]) / (double)sumPref);
            if (w < kTableMinColW) w = kTableMinColW;
            textW[c] = w;
            used += w;
        }
        textW[N - 1] = std::max(kTableMinColW, availTextW - used);

        // Wrap each cell at its column's textW; collect row heights.
        std::vector<int> rowH(b.tableRows.size(), 0);
        for (size_t r = 0; r < b.tableRows.size(); ++r) {
            for (int c = 0; c < N && c < (int)b.tableRows[r].size(); ++c) {
                TableCell& cell = b.tableRows[r][c];
                cell.lines.clear();
                std::vector<InlineRun> runs = cell.runs;
                if (r == 0) for (auto& rr : runs) rr.bold = true;
                WrapRuns(dc, runs, BlockType::Paragraph, 0, textW[c], cell.lines);
                int h = 0;
                for (const auto& wl : cell.lines) h += wl.height;
                if (h == 0) {
                    // Empty cell still needs a single line of height for the row.
                    dc.SetFont(fontBody_);
                    wxCoord ww = 0, hh = 0;
                    dc.GetTextExtent("Hg", &ww, &hh);
                    h = hh;
                }
                cell.height = h;
                if (h > rowH[r]) rowH[r] = h;
            }
        }

        // Build column X edges and row Y edges (block-local).
        b.tableColX.assign(N + 1, 0);
        for (int c = 0; c < N; ++c) {
            b.tableColX[c + 1] = b.tableColX[c] + 1 + kTableCellPadX + textW[c] + kTableCellPadX;
        }
        b.tableRowY.assign(b.tableRows.size() + 1, 0);
        for (size_t r = 0; r < b.tableRows.size(); ++r) {
            b.tableRowY[r + 1] = b.tableRowY[r] + 1 + kTableCellPadY + rowH[r] + kTableCellPadY;
        }
        // Total block height = last row's bottom edge + 1px bottom border.
        b.cachedHeight = b.tableRowY.back() + 1;
        return;
    }

    if (b.type == BlockType::ToolCall) {
        dc.SetFont(fontCode_);
        int charH = dc.GetCharHeight();
        b.toolHeaderH = charH + kToolPadY * 2;
        b.lines.clear();

        // Pre-measure the static parts of the header so PaintBlock has no
        // GetTextExtent calls. The big perf win here is the args truncation —
        // for write_file/edit_file calls toolArgs can be 1000+ chars, and
        // calling GetPartialTextExtents on that every paint costs ~35ms.
        b.toolChevStr = wxString(b.toolExpanded ? L'▾' : L'▸') + " ";
        wxCoord cw = 0, ch = 0;
        dc.GetTextExtent(b.toolChevStr, &cw, &ch);
        b.toolChevW = cw;
        wxCoord nw = 0, nh = 0;
        dc.GetTextExtent(b.toolName, &nw, &nh);
        b.toolNameW = nw;

        // Hint (collapsed only): "· N lines" or "· ok".
        b.toolHint.clear();
        b.toolHintW = 0;
        if (!b.toolExpanded) {
            int nlines = 1;
            for (size_t i = 0; i < b.toolResult.size(); ++i)
                if (b.toolResult[i] == '\n') ++nlines;
            if (b.toolResult.IsEmpty()) nlines = 0;
            if (nlines > 1) {
                b.toolHint = wxString::FromUTF8(" · ");
                b.toolHint << nlines << " lines";
            } else if (!b.toolResult.IsEmpty()) {
                b.toolHint = wxString::FromUTF8(" · ok");
            }
            if (!b.toolHint.IsEmpty()) {
                wxCoord hw = 0, hh = 0;
                dc.GetTextExtent(b.toolHint, &hw, &hh);
                b.toolHintW = hw;
            }
        }

        // Truncate args to fit. Only the first ~maxChars are ever visible; for
        // 1000+ char args we used to feed the whole string to GetPartialTextExtents
        // which is the dominant paint cost. Cap by character count first using a
        // generous estimate of avg char width, then measure only the candidate
        // prefix.
        const int argsX = kToolPadX + b.toolChevW + b.toolNameW;
        int rightLimit = contentWidth - kToolPadX;
        int argsAvail = rightLimit - argsX - b.toolHintW;
        if (argsAvail < 20) argsAvail = 20;
        b.toolArgsX = argsX;

        // Estimate the worst-case char width with two reference glyphs and
        // pick the smaller (so we don't undercount narrow chars).
        wxCoord refW = 0, refH = 0;
        dc.GetTextExtent("M", &refW, &refH);
        int minCharW = refW > 0 ? refW : 8;
        // Hard cap: at minimum char width, this many chars *could* fit. Add
        // generous margin for narrow chars + the leading "(" + trailing ")".
        size_t maxChars = (size_t)(argsAvail / std::max(1, minCharW / 2)) + 4;
        if (maxChars > b.toolArgs.size() + 2) maxChars = b.toolArgs.size() + 2;

        wxString argsDisplay = "(" + b.toolArgs.Left(maxChars) + ")";
        wxCoord aw = 0, ah = 0;
        dc.GetTextExtent(argsDisplay, &aw, &ah);
        if (aw <= argsAvail && b.toolArgs.size() <= maxChars) {
            // Whole args fit — no truncation needed.
            b.toolArgsFit = "(" + b.toolArgs + ")";
        } else {
            // Need to truncate. Measure only the capped candidate.
            const wxString ell = "…)";
            wxCoord ellW = 0, ellH = 0;
            dc.GetTextExtent(ell, &ellW, &ellH);
            int budget = argsAvail - ellW;
            if (budget < 1) budget = 1;
            wxArrayInt parts;
            dc.GetPartialTextExtents(argsDisplay, parts);
            size_t cut = argsDisplay.size();
            for (size_t i = 0; i < parts.size(); ++i) {
                if (parts[i] > budget) { cut = i; break; }
            }
            b.toolArgsFit = argsDisplay.Left(cut) + ell;
        }

        if (b.toolExpanded) {
            // Body is the full args + result, char-wrapped at the inner width.
            int innerW = contentWidth - kToolPadX * 2;
            if (innerW < 50) innerW = 50;
            wxString bodyText;
            if (!b.toolArgs.IsEmpty()) {
                bodyText = "args: " + b.toolArgs + "\n\n";
            }
            bodyText += b.toolResult;
            WrapMonospace(dc, bodyText, innerW, b.lines);

            int bodyH = 0;
            for (const auto& wl : b.lines) bodyH += wl.height;
            bodyH += kToolPadY * 2;
            b.cachedHeight = b.toolHeaderH + kToolGap + bodyH;
        } else {
            b.cachedHeight = b.toolHeaderH;
        }
        return;
    }

    // Word-wrap path for Paragraph / Heading / UserPrompt.
    int maxW = contentWidth;
    if (b.type == BlockType::UserPrompt) {
        maxW = contentWidth - kUserBubblePad * 2;
    }

    b.lines.clear();
    WrapRuns(dc, b.runs, b.type, hLvl, maxW, b.lines);

    int totalH = 0;
    for (const auto& wl : b.lines) totalH += wl.height;
    if (b.lines.empty()) {
        const wxFont& f = FontFor({}, b.type, hLvl);
        dc.SetFont(f);
        wxCoord ww = 0, hh = 0;
        dc.GetTextExtent("Hg", &ww, &hh);
        totalH = hh;
    }
    if (b.type == BlockType::UserPrompt) totalH += kUserBubblePad * 2;
    b.cachedHeight = totalH;
}

void ChatCanvas::WrapRuns(wxDC& dc, const std::vector<InlineRun>& runs,
                          BlockType bt, int hLvl, int maxW,
                          std::vector<WrappedLine>& outLines) const {
    auto toks = Tokenize(runs);

    WrappedLine cur;
    cur.textStart = 0;
    int curX = 0;
    int lineHeight = 0;

    auto styleFontHeight = [&]() {
        const wxFont& f = FontFor({}, bt, hLvl);
        wxCoord ww = 0, hh = 0;
        dc.SetFont(f);
        dc.GetTextExtent("Hg", &ww, &hh);
        return hh;
    };

    auto measureToken = [&](const Tok& t) {
        const wxFont& f = FontFor(t.style, bt, hLvl);
        dc.SetFont(f);
        wxCoord ww = 0, hh = 0;
        dc.GetTextExtent(t.text, &ww, &hh);
        return std::pair<int, int>{ww, hh};
    };

    auto appendToLine = [&](const Tok& t, int tw, int th) {
        if (cur.runs.empty() || !SameStyle(cur.runs.back(), t.style)) {
            InlineRun r = t.style;
            r.text = t.text;
            cur.runs.push_back(r);
            cur.runX.push_back(curX);
        } else {
            cur.runs.back().text += t.text;
        }
        cur.text += t.text;
        curX += tw;
        if (th > lineHeight) lineHeight = th;
    };

    auto finalizeLine = [&]() {
        if (cur.runs.empty() && cur.text.IsEmpty()) return;
        cur.glyphX.clear();
        cur.glyphX.push_back(0);
        int xoff = 0;
        for (const auto& r : cur.runs) {
            const wxFont& f = FontFor(r, bt, hLvl);
            dc.SetFont(f);
            wxArrayInt parts;
            dc.GetPartialTextExtents(r.text, parts);
            for (auto p : parts) cur.glyphX.push_back(xoff + p);
            xoff += parts.empty() ? 0 : parts.back();
        }
        cur.textEnd = cur.textStart + (int)cur.text.size();
        cur.height = lineHeight > 0 ? lineHeight : styleFontHeight();
        outLines.push_back(std::move(cur));
        cur = WrappedLine();
        cur.textStart = 0;
        curX = 0;
        lineHeight = 0;
    };

    for (size_t i = 0; i < toks.size(); ++i) {
        const Tok& t = toks[i];

        if (t.isNewline) {
            // Hard line break. If the current visual line is empty, emit a
            // blank line at the default font height so consecutive newlines
            // render as visible empty rows.
            if (cur.text.IsEmpty() && cur.runs.empty()) {
                cur.height = styleFontHeight();
                cur.textEnd = cur.textStart;
                cur.glyphX.clear();
                cur.glyphX.push_back(0);
                outLines.push_back(std::move(cur));
                cur = WrappedLine();
            } else {
                finalizeLine();
            }
            cur.textStart = t.srcStart + 1;
            curX = 0;
            lineHeight = 0;
            continue;
        }

        auto [tw, th] = measureToken(t);

        if (t.isSpace) {
            if (curX == 0) {
                cur.textStart = t.srcStart + 1;
                continue;
            }
            if (curX + tw > maxW) {
                finalizeLine();
                cur.textStart = t.srcStart + 1;
                continue;
            }
            appendToLine(t, tw, th);
        } else if (tw <= maxW) {
            if (curX > 0 && curX + tw > maxW) {
                finalizeLine();
            }
            if (curX == 0) cur.textStart = t.srcStart;
            appendToLine(t, tw, th);
        } else {
            // Single token wider than maxW (long URL, path, base64 blob, …).
            // Split it at character boundaries so the line wraps instead of
            // overflowing the content column.
            const wxFont& f = FontFor(t.style, bt, hLvl);
            dc.SetFont(f);
            wxArrayInt parts;
            dc.GetPartialTextExtents(t.text, parts);
            size_t pos = 0;
            while (pos < t.text.size()) {
                int startW = (pos > 0) ? parts[pos - 1] : 0;
                int avail = maxW - curX;
                if (avail < 1) { finalizeLine(); avail = maxW; }
                size_t j = pos;
                while (j < t.text.size() && (parts[j] - startW) <= avail) ++j;
                if (j == pos) {
                    if (curX > 0) { finalizeLine(); continue; }
                    j = pos + 1;  // ensure forward progress
                }
                Tok piece = t;
                piece.text = t.text.SubString(pos, j - 1);
                piece.srcStart = t.srcStart + (int)pos;
                wxCoord pw = 0, ph = 0;
                dc.GetTextExtent(piece.text, &pw, &ph);
                if (curX == 0) cur.textStart = piece.srcStart;
                appendToLine(piece, pw, ph);
                pos = j;
                if (pos < t.text.size()) finalizeLine();
            }
        }
    }
    finalizeLine();
}

void ChatCanvas::WrapMonospace(wxDC& dc, const wxString& text, int maxW,
                               std::vector<WrappedLine>& outLines,
                               int textOffBase) const {
    dc.SetFont(fontCode_);
    int charH = dc.GetCharHeight();

    auto emitLine = [&](const wxString& seg, int srcStart) {
        WrappedLine wl;
        wl.text = seg;
        wl.height = charH;
        wl.textStart = textOffBase + srcStart;
        wl.textEnd = wl.textStart + (int)seg.size();
        InlineRun r;
        r.text = seg;
        r.code = true;
        wl.runs.push_back(r);
        wl.runX.push_back(0);
        wl.glyphX.push_back(0);
        if (!seg.IsEmpty()) {
            wxArrayInt parts;
            dc.GetPartialTextExtents(seg, parts);
            for (auto p : parts) wl.glyphX.push_back(p);
        }
        outLines.push_back(std::move(wl));
    };

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        wxString srcLine = (nl == wxString::npos) ? text.Mid(pos)
                                                  : text.Mid(pos, nl - pos);
        const int lineSrcStart = (int)pos;
        if (srcLine.IsEmpty()) {
            emitLine(wxString{}, lineSrcStart);
        } else {
            wxArrayInt parts;
            dc.GetPartialTextExtents(srcLine, parts);
            // parts[i] = pixel width of srcLine[0..i+1]

            size_t i = 0;
            while (i < srcLine.size()) {
                int startW = (i > 0) ? parts[i - 1] : 0;
                // Greedy: largest j where srcLine[i..j] fits in maxW.
                size_t j = i;
                while (j < srcLine.size() && (parts[j] - startW) <= maxW) ++j;
                if (j == i) j = i + 1;  // ensure forward progress

                size_t breakAt = j;
                if (j < srcLine.size()) {
                    // Soft-break preference: backtrack to last space in (i, j].
                    for (size_t k = j; k > i + 1; --k) {
                        if (srcLine[k - 1] == ' ') { breakAt = k; break; }
                    }
                }
                emitLine(srcLine.SubString(i, breakAt - 1), lineSrcStart + (int)i);
                i = breakAt;
            }
        }

        if (nl == wxString::npos) break;
        pos = nl + 1;
    }
}

void ChatCanvas::Relayout(int width) {
    PERF_SCOPE_T("Relayout", 1000);
    EnsureFonts();

    // O(1) fast path. Mutators flip layoutDirty_; nothing dirty + same width =
    // nothing to do. Avoids the per-block cache scan on every paint/scroll.
    if (!layoutDirty_ && width == layoutWidth_) return;

    int contentW = std::min(width - 2 * kSideMargin, kMaxContentW);
    if (contentW < 100) contentW = 100;

    wxClientDC dc(this);
    int y = kTopMargin;
    int relaidCount = 0;
    blockTops_.assign(blocks_.size() + 1, 0);
    for (size_t i = 0; i < blocks_.size(); ++i) {
        auto& b = blocks_[i];
        int spacing = (b.type == BlockType::Heading) ? kHeadingSpacing : kBlockSpacing;
        y += spacing;
        // Per-block guard: skip blocks already laid out at this width.
        // AddBlock and ToggleToolCall set b.cachedWidth = -1 on just the
        // affected block; everyone else keeps its cached lines/heights.
        if (b.cachedWidth != contentW) {
            LayoutBlock(dc, b, contentW, spacing);
            ++relaidCount;
        }
        blockTops_[i] = y;
        y += b.cachedHeight;
    }
    if (!blockTops_.empty()) blockTops_.back() = y;
    if (thinking_) y += kBlockSpacing + 24;
    y += kBottomMargin;

    contentHeight_ = y;
    layoutWidth_ = width;
    layoutDirty_ = false;

    PERF_LOG("Relayout n=%d w=%d h=%d", relaidCount, width, contentHeight_);
    SetVirtualSize(width, contentHeight_);
}

// ---------- Paint ----------

void ChatCanvas::RenderViewport(wxDC& dc, int viewY, int width, int height,
                                BlockPos selStart, BlockPos selEnd) const {
    // Paints into `dc` in client coords (0..height). `viewY` is the canvas-coord
    // y of the topmost visible pixel. Caller arranges scroll offset translation.
    const Palette& pal = palette_;
    dc.SetBackground(wxBrush(pal.bg));
    dc.Clear();

    int contentW = std::min(width - 2 * kSideMargin, kMaxContentW);
    if (contentW < 100) contentW = 100;
    int xLeft = (width - contentW) / 2;
    if (xLeft < kSideMargin) xLeft = kSideMargin;

    const int dirtyTop    = viewY;
    const int dirtyBottom = viewY + height;

    // Find the first block whose bottom edge is at or below dirtyTop. Use the
    // cumulative-Y prefix so this is O(log N) instead of an O(N) walk.
    size_t firstVisible = 0;
    if (!blockTops_.empty() && (int)blocks_.size() + 1 == (int)blockTops_.size()) {
        // blockTops_[i] = top of blocks_[i]; bottom = blockTops_[i] + cachedHeight.
        // We need the smallest i such that blockTops_[i] + h(i) >= dirtyTop.
        // Equivalently: skip blocks whose top + cachedHeight < dirtyTop.
        // Binary-search on `blockTops_[i+1]` (which is the next block's top) >= dirtyTop.
        auto it = std::lower_bound(blockTops_.begin() + 1, blockTops_.end(), dirtyTop);
        if (it != blockTops_.end()) {
            firstVisible = (size_t)(it - blockTops_.begin()) - 1;
        } else {
            firstVisible = blocks_.size();
        }
    }

    int painted = 0;
    for (size_t i = firstVisible; i < blocks_.size(); ++i) {
        const Block& b = blocks_[i];
        const int blockTop = blockTops_[i];
        if (blockTop > dirtyBottom) break;
        const int blockBottom = blockTop + b.cachedHeight;
        // Translate canvas-coord block top to client coords for PaintBlock.
        const int yTopClient = blockTop - viewY;
        PaintBlock(dc, b, yTopClient, selStart, selEnd, (int)i);
        (void)blockBottom;
        ++painted;
    }
    PERF_LOG("Render firstVis=%zu painted=%d", firstVisible, painted);
}

void ChatCanvas::OnPaint(wxPaintEvent&) {
    PERF_SCOPE_T("OnPaint", 50);

    EnsureFonts();
    wxSize sz = GetClientSize();
    Relayout(sz.x);

    // Read scroll offset.
    int xu, yu;
    GetScrollPixelsPerUnit(&xu, &yu);
    int vx, vy;
    GetViewStart(&vx, &vy);
    const int viewY = (yu > 0) ? vy * yu : 0;

    BlockPos selStart = selAnchor_, selEnd = selCaret_;
    if (selEnd < selStart) std::swap(selStart, selEnd);

    // wxAutoBufferedPaintDC manages a DPI-correct backing buffer per platform.
    // Painting a custom wxBitmap and blitting ourselves was wrong on HiDPI:
    // the bitmap was sized in logical pixels but the screen has more physical
    // pixels, so the blit was upscaled and text came out pixelated.
    wxAutoBufferedPaintDC dc(this);

    RenderViewport(dc, viewY, sz.x, sz.y, selStart, selEnd);

    if (thinking_) {
        int contentW = std::min(sz.x - 2 * kSideMargin, kMaxContentW);
        if (contentW < 100) contentW = 100;
        int xLeft = (sz.x - contentW) / 2;
        if (xLeft < kSideMargin) xLeft = kSideMargin;
        int dotsCanvasY = (blockTops_.empty() ? kTopMargin : blockTops_.back()) + 8;
        int dotsClientY = dotsCanvasY - viewY;
        dotsRect_ = wxRect(xLeft, dotsCanvasY, 16 * 3 + 8, 16);
        dotsRectValid_ = true;
        PaintThinkingDots(dc, xLeft, dotsClientY);
    } else {
        dotsRectValid_ = false;
    }

    PERF_LOG("Paint viewY=%d", viewY);
}

void ChatCanvas::PaintBlock(wxDC& dc, const Block& b, int yTop, BlockPos selStart, BlockPos selEnd, int blockIdx) const {
    const char* tag = "PaintBlock";
    switch (b.type) {
        case BlockType::Paragraph:  tag = "PaintBlock:Paragraph"; break;
        case BlockType::Heading:    tag = "PaintBlock:Heading";   break;
        case BlockType::CodeBlock:  tag = "PaintBlock:Code";      break;
        case BlockType::UserPrompt: tag = "PaintBlock:User";      break;
        case BlockType::Table:      tag = "PaintBlock:Table";     break;
        case BlockType::ToolCall:   tag = "PaintBlock:Tool";      break;
    }
    PERF_SCOPE_T(tag, 200);
    const Palette& pal = palette_;
    wxSize sz = GetClientSize();
    int contentW = std::min(sz.x - 2 * kSideMargin, kMaxContentW);
    if (contentW < 100) contentW = 100;
    int xLeft = (sz.x - contentW) / 2;
    if (xLeft < kSideMargin) xLeft = kSideMargin;

    // Compute the per-block selection char range.
    int bSelStart = -1, bSelEnd = -1;
    bool blockSelected = false;
    if (selStart.IsValid() && selEnd.IsValid() && !(selStart == selEnd)) {
        if (blockIdx >= selStart.block && blockIdx <= selEnd.block) {
            blockSelected = true;
            bSelStart = (blockIdx == selStart.block) ? selStart.offset : 0;
            bSelEnd   = (blockIdx == selEnd.block)   ? selEnd.offset   : (int)b.visibleText.size();
        }
    }

    int blockX = xLeft;
    int blockW = contentW;

    if (b.type == BlockType::Table) {
        const int N = (int)b.tableAligns.size();
        const int R = (int)b.tableRows.size();
        if (N == 0 || R == 0 || b.tableColX.empty() || b.tableRowY.empty()) return;

        const int tableW = b.tableColX.back() + 1;

        // Selection tint behind the whole table when selected.
        if (blockSelected) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(pal.selectionBg));
            dc.DrawRectangle(blockX, yTop, tableW, b.cachedHeight);
        }

        // Header row background (subtle tint, only if no selection overlay).
        if (!blockSelected) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(pal.tableHeaderBg));
            int hy0 = yTop + b.tableRowY[0] + 1;
            int hy1 = yTop + b.tableRowY[1];
            int hx0 = blockX + 1;
            int hx1 = blockX + b.tableColX.back();
            dc.DrawRectangle(hx0, hy0, hx1 - hx0, hy1 - hy0);
        }

        // Grid lines.
        dc.SetPen(wxPen(pal.tableBorder, 1));
        // Vertical: at each column edge.
        for (int c = 0; c <= N; ++c) {
            int x = blockX + b.tableColX[c];
            dc.DrawLine(x, yTop, x, yTop + b.cachedHeight);
        }
        // Horizontal: at each row edge.
        for (int r = 0; r <= R; ++r) {
            int y = yTop + b.tableRowY[r];
            dc.DrawLine(blockX, y, blockX + tableW, y);
        }

        // Cell text.
        for (int r = 0; r < R; ++r) {
            const int rowTextY0 = yTop + b.tableRowY[r] + 1 + kTableCellPadY;
            const int rowH = b.tableRowY[r + 1] - b.tableRowY[r] - 2 - 2 * kTableCellPadY;
            for (int c = 0; c < N && c < (int)b.tableRows[r].size(); ++c) {
                const TableCell& cell = b.tableRows[r][c];
                const int colTextX0 = blockX + b.tableColX[c] + 1 + kTableCellPadX;
                const int colTextW  = b.tableColX[c + 1] - b.tableColX[c] - 1 - 2 * kTableCellPadX;

                // Vertical centering of cell content within the row.
                int innerH = 0;
                for (const auto& wl : cell.lines) innerH += wl.height;
                int yLine = rowTextY0 + std::max(0, (rowH - innerH) / 2);

                for (const auto& wl : cell.lines) {
                    int lineW = wl.glyphX.empty() ? 0 : wl.glyphX.back();
                    int xBase = colTextX0;
                    TableAlign al = b.tableAligns[c];
                    if (al == TableAlign::Center)      xBase = colTextX0 + (colTextW - lineW) / 2;
                    else if (al == TableAlign::Right)  xBase = colTextX0 + (colTextW - lineW);

                    for (size_t ri = 0; ri < wl.runs.size(); ++ri) {
                        const auto& run = wl.runs[ri];
                        const wxFont& f = FontFor(run, BlockType::Paragraph, 0);
                        dc.SetFont(f);
                        dc.SetTextForeground(run.code ? pal.codeFg : pal.text);
                        int xr = xBase + (ri < wl.runX.size() ? wl.runX[ri] : 0);
                        dc.DrawText(run.text, xr, yLine);
                    }
                    yLine += wl.height;
                }
            }
        }
        return;
    }

    if (b.type == BlockType::ToolCall) {
        const int radius = 6;

        // Whole-block selection tint when the user has Ctrl+A'd or selected
        // across this block. Tool blocks don't support per-character mouse
        // selection, so it's all-or-nothing.
        if (blockSelected) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(pal.selectionBg));
            dc.DrawRoundedRectangle(blockX, yTop, blockW, b.cachedHeight, radius);
        }

        // Header background.
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(pal.toolHeaderBg));
        if (b.toolExpanded) {
            // Header rounded only on top corners — fake by drawing a round
            // rect spanning the whole block then a bottom-half flat rect.
            dc.DrawRoundedRectangle(blockX, yTop, blockW, b.toolHeaderH + radius, radius);
            dc.DrawRectangle(blockX, yTop + b.toolHeaderH - 1, blockW, 1);
        } else {
            dc.DrawRoundedRectangle(blockX, yTop, blockW, b.toolHeaderH, radius);
        }

        // Header text — all measurements precomputed in LayoutBlock.
        dc.SetFont(fontCode_);
        const int textY = yTop + kToolPadY;
        const int xChev = blockX + kToolPadX;
        const int xName = xChev + b.toolChevW;
        const int xArgs = blockX + b.toolArgsX;
        const int rightLimit = blockX + blockW - kToolPadX;

        dc.SetTextForeground(pal.toolAccent);
        dc.DrawText(b.toolChevStr, xChev, textY);
        dc.DrawText(b.toolName, xName, textY);

        dc.SetTextForeground(pal.codeFg);
        dc.DrawText(b.toolArgsFit, xArgs, textY);

        if (!b.toolHint.IsEmpty()) {
            dc.SetTextForeground(pal.toolDim);
            dc.DrawText(b.toolHint, rightLimit - b.toolHintW, textY);
        }

        // Body (when expanded).
        if (b.toolExpanded) {
            const int bodyTop = yTop + b.toolHeaderH + kToolGap;
            const int bodyH = b.cachedHeight - b.toolHeaderH - kToolGap;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(pal.toolBodyBg));
            dc.DrawRoundedRectangle(blockX, bodyTop, blockW, bodyH, radius);
            // Square the top corners (continuous with header).
            dc.DrawRectangle(blockX, bodyTop, blockW, radius);

            int yLine = bodyTop + kToolPadY;
            const int textXLeft = blockX + kToolPadX;
            dc.SetFont(fontCode_);
            dc.SetTextForeground(pal.codeFg);
            for (const auto& wl : b.lines) {
                if (!wl.text.IsEmpty()) dc.DrawText(wl.text, textXLeft, yLine);
                yLine += wl.height;
            }
        }
        return;
    }

    if (b.type == BlockType::CodeBlock) {
        // Background fill.
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(pal.codeBg));
        dc.DrawRoundedRectangle(blockX, yTop, blockW, b.cachedHeight, 6);

        int yLine = yTop + kCodePadding;
        dc.SetFont(fontCode_);
        dc.SetTextForeground(pal.codeFg);
        for (const auto& wl : b.lines) {
            // Selection highlight.
            if (blockSelected) {
                int lineSelStart = std::max(bSelStart - wl.textStart, 0);
                int lineSelEnd = std::min(bSelEnd - wl.textStart, (int)wl.text.size());
                if (lineSelEnd > lineSelStart && lineSelStart < (int)wl.text.size() && lineSelEnd > 0) {
                    int x1 = blockX + kCodePadding + wl.glyphX[lineSelStart];
                    int x2 = blockX + kCodePadding + wl.glyphX[lineSelEnd];
                    dc.SetBrush(wxBrush(pal.selectionBg));
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.DrawRectangle(x1, yLine, x2 - x1, wl.height);
                }
            }
            dc.DrawText(wl.text, blockX + kCodePadding, yLine);
            yLine += wl.height;
        }
        return;
    }

    // Paragraph / Heading / UserPrompt
    int textXLeft = blockX;
    int textYTop = yTop;
    if (b.type == BlockType::UserPrompt) {
        // Right-aligned bubble — width fits the longest line content.
        int bubbleW = 0;
        for (const auto& wl : b.lines) {
            if (!wl.glyphX.empty()) bubbleW = std::max<int>(bubbleW, wl.glyphX.back());
        }
        bubbleW += kUserBubblePad * 2;
        if (bubbleW > blockW) bubbleW = blockW;
        int bubbleX = blockX + blockW - bubbleW;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(pal.userBubbleBg));
        dc.DrawRoundedRectangle(bubbleX, yTop, bubbleW, b.cachedHeight, 8);
        textXLeft = bubbleX + kUserBubblePad;
        textYTop  = yTop + kUserBubblePad;
    }

    int yLine = textYTop;
    for (const auto& wl : b.lines) {
        if (blockSelected) {
            int lineSelStart = std::max(bSelStart - wl.textStart, 0);
            int lineSelEnd = std::min(bSelEnd - wl.textStart, (int)wl.text.size());
            if (lineSelEnd > lineSelStart && lineSelStart < (int)wl.text.size() && lineSelEnd > 0
                && wl.textStart < bSelEnd && wl.textEnd > bSelStart) {
                int x1 = textXLeft + wl.glyphX[lineSelStart];
                int x2 = textXLeft + wl.glyphX[lineSelEnd];
                dc.SetBrush(wxBrush(pal.selectionBg));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRectangle(x1, yLine, x2 - x1, wl.height);
            }
        }
        // Draw runs.
        for (size_t ri = 0; ri < wl.runs.size(); ++ri) {
            const auto& r = wl.runs[ri];
            const wxFont& f = FontFor(r, b.type, b.headingLevel);
            dc.SetFont(f);
            dc.SetTextForeground(r.code ? pal.codeFg : pal.text);
            int xr = textXLeft + (ri < wl.runX.size() ? wl.runX[ri] : 0);
            dc.DrawText(r.text, xr, yLine);
        }
        yLine += wl.height;
    }
}

void ChatCanvas::PaintThinkingDots(wxDC& dc, int xLeft, int yTop) const {
    dc.SetPen(*wxTRANSPARENT_PEN);
    for (int i = 0; i < 3; ++i) {
        double phase = animPhase_ - i * 0.4;
        double s = (std::sin(phase) + 1.0) * 0.5;  // 0..1
        int alpha = (int)(60 + s * 180);
        wxColour c(palette_.thinkingDot.Red(), palette_.thinkingDot.Green(), palette_.thinkingDot.Blue(), alpha);
        dc.SetBrush(wxBrush(c));
        dc.DrawCircle(xLeft + 8 + i * 16, yTop + 8, 4);
    }
}

// ---------- Hit-test & selection ----------

BlockPos ChatCanvas::HitTest(const wxPoint& canvasPt) const {
    wxSize sz = GetClientSize();
    int contentW = std::min(sz.x - 2 * kSideMargin, kMaxContentW);
    if (contentW < 100) contentW = 100;
    int xLeft = (sz.x - contentW) / 2;
    if (xLeft < kSideMargin) xLeft = kSideMargin;

    int y = kTopMargin;
    for (size_t i = 0; i < blocks_.size(); ++i) {
        const Block& b = blocks_[i];
        int spacing = (b.type == BlockType::Heading) ? kHeadingSpacing : kBlockSpacing;
        y += spacing;
        int blockTop = y;
        int blockBottom = y + b.cachedHeight;
        if (canvasPt.y >= blockTop - spacing / 2 && canvasPt.y < blockBottom) {
            // Tables and tool-call blocks: snap to start/end based on click Y
            // midpoint. Per-character selection isn't supported.
            if (b.type == BlockType::Table || b.type == BlockType::ToolCall) {
                int mid = blockTop + b.cachedHeight / 2;
                if (canvasPt.y < mid) return {(int)i, 0};
                return {(int)i, (int)b.visibleText.size()};
            }
            // Find line.
            int textXLeft = xLeft;
            int yLine = blockTop;
            if (b.type == BlockType::UserPrompt) {
                int bubbleW = 0;
                for (const auto& wl : b.lines)
                    if (!wl.glyphX.empty()) bubbleW = std::max<int>(bubbleW, wl.glyphX.back());
                bubbleW += kUserBubblePad * 2;
                if (bubbleW > contentW) bubbleW = contentW;
                textXLeft = xLeft + contentW - bubbleW + kUserBubblePad;
                yLine += kUserBubblePad;
            } else if (b.type == BlockType::CodeBlock) {
                textXLeft = xLeft + kCodePadding;
                yLine += kCodePadding;
            }

            for (const auto& wl : b.lines) {
                if (canvasPt.y >= yLine && canvasPt.y < yLine + wl.height) {
                    int xRel = canvasPt.x - textXLeft;
                    if (xRel < 0) return {(int)i, wl.textStart};
                    // Find char by binary scan on glyphX.
                    int charIdx = (int)wl.text.size();
                    for (size_t k = 0; k + 1 < wl.glyphX.size(); ++k) {
                        int mid = (wl.glyphX[k] + wl.glyphX[k + 1]) / 2;
                        if (xRel < mid) { charIdx = (int)k; break; }
                    }
                    return {(int)i, wl.textStart + charIdx};
                }
                yLine += wl.height;
            }
            // Below all lines of this block: return end.
            return {(int)i, (int)b.visibleText.size()};
        }
        y = blockBottom;
    }
    if (!blocks_.empty()) {
        return {(int)blocks_.size() - 1, (int)blocks_.back().visibleText.size()};
    }
    return {};
}

void ChatCanvas::OnLeftDown(wxMouseEvent& e) {
    SetFocus();
    wxPoint p = e.GetPosition();
    CalcUnscrolledPosition(p.x, p.y, &p.x, &p.y);
    BlockPos hp = HitTest(p);
    if (!hp.IsValid()) return;

    // Tool-call blocks consume clicks for expansion toggle. They don't
    // support per-character selection; whole-block selection still works
    // via Ctrl+A or by drag-selecting through them from another block.
    if (hp.block >= 0 && hp.block < (int)blocks_.size()
        && blocks_[hp.block].type == BlockType::ToolCall) {
        // Clear any prior selection so the tinted overlay doesn't obscure
        // the block while the user is just toggling.
        selAnchor_ = selCaret_ = {};
        ToggleToolCall(hp.block);
        return;
    }

    selAnchor_ = hp;
    selCaret_ = hp;
    selecting_ = true;
    CaptureMouse();
    Refresh();
}

void ChatCanvas::OnLeftUp(wxMouseEvent& /*e*/) {
    if (selecting_ && HasCapture()) ReleaseMouse();
    selecting_ = false;
}

void ChatCanvas::OnMotion(wxMouseEvent& e) {
    if (!selecting_) return;
    wxPoint p = e.GetPosition();
    CalcUnscrolledPosition(p.x, p.y, &p.x, &p.y);
    BlockPos hp = HitTest(p);
    if (!hp.IsValid()) return;
    if (hp == selCaret_) return;
    selCaret_ = hp;
    Refresh();
}

void ChatCanvas::OnKeyDown(wxKeyEvent& e) {
    if (e.ControlDown() && e.GetKeyCode() == 'A') {
        SelectAll();
        return;
    }
    if (e.ControlDown() && e.GetKeyCode() == 'C') {
        wxString sel = GetSelectedText();
        if (!sel.IsEmpty() && wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(sel));
            wxTheClipboard->Close();
        }
        return;
    }
    e.Skip();
}

void ChatCanvas::OnSysColourChanged(wxSysColourChangedEvent& e) {
    RebuildPalette();
    Refresh();
    e.Skip();
}

void ChatCanvas::SelectAll() {
    if (blocks_.empty()) return;
    selAnchor_ = {0, 0};
    selCaret_  = {(int)blocks_.size() - 1, (int)blocks_.back().visibleText.size()};
    Refresh();
}

wxString ChatCanvas::GetSelectedText() const {
    if (!selAnchor_.IsValid() || !selCaret_.IsValid() || selAnchor_ == selCaret_) return {};
    BlockPos a = selAnchor_, b = selCaret_;
    if (b < a) std::swap(a, b);
    wxString out;
    for (int i = a.block; i <= b.block; ++i) {
        const Block& blk = blocks_[i];
        int s = (i == a.block) ? a.offset : 0;
        int e = (i == b.block) ? b.offset : (int)blk.visibleText.size();
        s = ClampInt(s, 0, (int)blk.visibleText.size());
        e = ClampInt(e, 0, (int)blk.visibleText.size());
        if (e > s) out += blk.visibleText.SubString(s, e - 1);
        if (i < b.block) out += "\n\n";
    }
    return out;
}
