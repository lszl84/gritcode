#pragma once
#include "block.h"
#include <wx/string.h>
#include <functional>

// Streaming markdown parser. Buffers incoming text, emits a Block via onBlock_
// only when the block CLOSES — never mutates a previously emitted block.
//
// Recognized block types: paragraph, ATX heading (# .. ######), fenced code
// block (```). Lists/tables/blockquotes are not classified specially in this
// prototype — they fall through as paragraphs (still readable).
class MdStream {
public:
    using BlockSink = std::function<void(Block)>;

    explicit MdStream(BlockSink sink) : sink_(std::move(sink)) {}

    void Feed(const wxString& chunk);
    void Flush();  // emit any in-progress block at end of stream

private:
    enum class State { Top, InCode, InPara, MaybeTable, InTable };
    State state_ = State::Top;
    wxString buffer_;
    wxString current_;   // accumulated raw markdown of current block
    wxString codeLang_;
    BlockSink sink_;

    // Table assembly state. When MaybeTable, headerLine_ holds the candidate
    // header line — emitted as a paragraph if the next line isn't a separator.
    wxString headerLine_;
    std::vector<std::vector<wxString>> tableCells_;  // raw cell text per row
    std::vector<TableAlign> tableAligns_;

    void ProcessLine(const wxString& line);
    void EmitParagraph();
    void EmitHeading(int level, const wxString& text);
    void EmitCode();
    void EmitTable();

    // Returns true and fills `cells` if `line` is a table row (starts/ends with |).
    bool ParseRow(const wxString& line, std::vector<wxString>& cells) const;
    // Returns true and fills `aligns` if `line` is a separator like |---|:--:|---:|.
    bool ParseSeparator(const wxString& line, std::vector<TableAlign>& aligns) const;
};
