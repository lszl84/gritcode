#include "md_parser.h"
#include "inline_parser.h"

namespace {

// Bullet marker: -, *, or + followed by a space.
bool IsBulletMarker(const wxString& trimmed) {
    if (trimmed.size() < 2) return false;
    wxUniChar c0 = trimmed[0];
    if (c0 != '-' && c0 != '*' && c0 != '+') return false;
    return trimmed[1] == ' ';
}

// Ordered marker: one or more digits followed by . or ) then space.
bool IsOrderedMarker(const wxString& trimmed) {
    size_t i = 0;
    while (i < trimmed.size() && trimmed[i] >= '0' && trimmed[i] <= '9') ++i;
    if (i == 0 || i > 9) return false;
    if (i + 1 >= trimmed.size()) return false;
    if (trimmed[i] != '.' && trimmed[i] != ')') return false;
    return trimmed[i + 1] == ' ';
}

bool IsListMarker(const wxString& line) {
    wxString t = line;
    t.Trim().Trim(false);
    return IsBulletMarker(t) || IsOrderedMarker(t);
}

}  // namespace

void MdStream::Feed(const wxString& chunk) {
    buffer_ += chunk;
    while (true) {
        int nl = buffer_.Find('\n');
        if (nl == wxNOT_FOUND) break;
        wxString line = buffer_.Mid(0, nl);
        buffer_ = buffer_.Mid(nl + 1);
        ProcessLine(line);
    }
}

void MdStream::Flush() {
    // Drain a trailing partial line as if newline-terminated.
    if (!buffer_.IsEmpty()) {
        ProcessLine(buffer_);
        buffer_.Clear();
    }
    if (state_ == State::InPara) {
        EmitParagraph();
    } else if (state_ == State::InCode) {
        // Unterminated fence: still emit what we got.
        EmitCode();
    } else if (state_ == State::MaybeTable) {
        // Header-looking line never got a separator — fall back to paragraph.
        current_ = headerLine_;
        headerLine_.Clear();
        EmitParagraph();
    } else if (state_ == State::InTable) {
        EmitTable();
    }
    state_ = State::Top;
}

bool MdStream::ParseRow(const wxString& line, std::vector<wxString>& cells) const {
    wxString s = line;
    s.Trim().Trim(false);
    if (s.IsEmpty() || s[0] != '|') return false;
    // Strip leading/trailing | (trailing optional in some flavours, but require
    // at least the leading pipe).
    s = s.Mid(1);
    if (!s.IsEmpty() && s.Last() == '|') s.RemoveLast();

    cells.clear();
    wxString cur;
    bool prevBackslash = false;
    for (size_t i = 0; i < s.size(); ++i) {
        wxUniChar ch = s[i];
        if (ch == '\\' && !prevBackslash) {
            prevBackslash = true;
            cur += ch;  // keep so inline parser can resolve the escape
            continue;
        }
        if (ch == '|' && !prevBackslash) {
            cells.push_back(cur);
            cur.Clear();
            continue;
        }
        cur += ch;
        prevBackslash = false;
    }
    cells.push_back(cur);
    for (auto& c : cells) { c.Trim().Trim(false); }
    return true;
}

bool MdStream::ParseSeparator(const wxString& line, std::vector<TableAlign>& aligns) const {
    std::vector<wxString> cells;
    if (!ParseRow(line, cells)) return false;
    if (cells.empty()) return false;
    aligns.clear();
    for (const auto& cell : cells) {
        wxString c = cell;
        c.Trim().Trim(false);
        if (c.IsEmpty()) return false;
        bool leftColon = c[0] == ':';
        bool rightColon = c.Last() == ':';
        size_t i = leftColon ? 1 : 0;
        size_t end = rightColon ? c.size() - 1 : c.size();
        if (end <= i) return false;
        // Body must be all dashes.
        for (size_t k = i; k < end; ++k) {
            if (c[k] != '-') return false;
        }
        if (leftColon && rightColon)      aligns.push_back(TableAlign::Center);
        else if (rightColon)               aligns.push_back(TableAlign::Right);
        else                               aligns.push_back(TableAlign::Left);
    }
    return true;
}

void MdStream::ProcessLine(const wxString& line) {
    wxString trimmed = line;
    trimmed.Trim().Trim(false);

    // Inside fenced code: only a closing ``` ends it; everything else is verbatim.
    if (state_ == State::InCode) {
        if (trimmed.StartsWith("```")) {
            EmitCode();
            state_ = State::Top;
        } else {
            current_ += line;
            current_ += "\n";
        }
        return;
    }

    // Resolving a candidate table header.
    if (state_ == State::MaybeTable) {
        std::vector<TableAlign> aligns;
        if (ParseSeparator(line, aligns)) {
            // Confirmed table. Build header row from headerLine_.
            std::vector<wxString> headerCells;
            ParseRow(headerLine_, headerCells);
            // Pad/truncate header to align count.
            headerCells.resize(aligns.size());
            tableAligns_ = std::move(aligns);
            tableCells_.clear();
            tableCells_.push_back(std::move(headerCells));
            headerLine_.Clear();
            state_ = State::InTable;
            return;
        }
        // Not a separator — header line was just a paragraph.
        current_ = headerLine_;
        headerLine_.Clear();
        state_ = State::InPara;
        // Fall through to handle current line at InPara.
    }

    // Inside a confirmed table.
    if (state_ == State::InTable) {
        std::vector<wxString> cells;
        if (ParseRow(line, cells)) {
            cells.resize(tableAligns_.size());
            tableCells_.push_back(std::move(cells));
            return;
        }
        // Non-table line — close the table, then re-process this line.
        EmitTable();
        state_ = State::Top;
        // Fall through.
    }

    // Fence opener.
    if (trimmed.StartsWith("```")) {
        if (state_ == State::InPara) EmitParagraph();
        state_ = State::InCode;
        codeLang_ = trimmed.Mid(3);
        codeLang_.Trim();
        current_.Clear();
        return;
    }

    // Blank line ends an open paragraph.
    if (trimmed.IsEmpty()) {
        if (state_ == State::InPara) {
            EmitParagraph();
            state_ = State::Top;
        }
        return;
    }

    // ATX heading — only when at top level.
    if (state_ == State::Top && trimmed.StartsWith("#")) {
        int lvl = 0;
        while (lvl < (int)trimmed.size() && trimmed[lvl] == '#') ++lvl;
        if (lvl >= 1 && lvl <= 6 && lvl < (int)trimmed.size() && trimmed[lvl] == ' ') {
            EmitHeading(lvl, trimmed.Mid(lvl + 1));
            return;
        }
    }

    // Possible table header: a line starting with | at top level (or closing
    // a paragraph first).
    if (trimmed.StartsWith("|")) {
        if (state_ == State::InPara) {
            EmitParagraph();
            state_ = State::Top;
        }
        if (state_ == State::Top) {
            headerLine_ = line;
            state_ = State::MaybeTable;
            return;
        }
    }

    // List item (bullet or numbered) — emit any open paragraph, then render
    // this line as its own paragraph block. The leading marker stays in the
    // text so the bullet/number renders. List continuation indents and
    // hierarchical nesting aren't tracked yet — each line stands alone, which
    // is enough to give visible newlines (the user's complaint was that
    // adjacent items collapsed into one run-on paragraph).
    if (IsListMarker(line)) {
        if (state_ == State::InPara) {
            EmitParagraph();
        }
        current_ = line;
        current_.Trim(false);  // strip leading indent for clean rendering
        EmitParagraph();
        state_ = State::Top;
        return;
    }

    // Otherwise: accumulate into a paragraph. Soft-wrap newlines are joined
    // by spaces (markdown convention).
    if (state_ == State::Top) {
        state_ = State::InPara;
        current_.Clear();
    }
    if (!current_.IsEmpty()) current_ += " ";
    current_ += line;
}

void MdStream::EmitParagraph() {
    Block b;
    b.type = BlockType::Paragraph;
    b.rawText = current_;
    b.runs = ParseInlines(current_);
    for (auto& r : b.runs) b.visibleText += r.text;
    current_.Clear();
    if (sink_) sink_(std::move(b));
}

void MdStream::EmitHeading(int level, const wxString& text) {
    Block b;
    b.type = BlockType::Heading;
    b.headingLevel = level;
    b.rawText = text;
    b.runs = ParseInlines(text);
    for (auto& r : b.runs) b.visibleText += r.text;
    if (sink_) sink_(std::move(b));
}

void MdStream::EmitCode() {
    Block b;
    b.type = BlockType::CodeBlock;
    b.lang = codeLang_;
    b.rawText = current_;
    b.visibleText = current_;
    // CodeBlock keeps a single monospace run carrying the whole body.
    InlineRun r;
    r.text = current_;
    r.code = true;
    b.runs.push_back(std::move(r));
    current_.Clear();
    codeLang_.Clear();
    if (sink_) sink_(std::move(b));
}

void MdStream::EmitTable() {
    Block b;
    b.type = BlockType::Table;
    b.tableAligns = tableAligns_;
    b.tableRows.reserve(tableCells_.size());
    for (auto& row : tableCells_) {
        std::vector<TableCell> rowCells;
        rowCells.reserve(row.size());
        for (auto& raw : row) {
            TableCell c;
            c.rawText = raw;
            c.runs = ParseInlines(raw);
            for (auto& rr : c.runs) c.visibleText += rr.text;
            rowCells.push_back(std::move(c));
        }
        b.tableRows.push_back(std::move(rowCells));
    }
    // Build visibleText: rows joined by \n, cells by \t (clipboard-friendly).
    for (size_t r = 0; r < b.tableRows.size(); ++r) {
        for (size_t c = 0; c < b.tableRows[r].size(); ++c) {
            if (c > 0) b.visibleText += '\t';
            b.visibleText += b.tableRows[r][c].visibleText;
        }
        if (r + 1 < b.tableRows.size()) b.visibleText += '\n';
    }
    tableCells_.clear();
    tableAligns_.clear();
    if (sink_) sink_(std::move(b));
}
