// Simple tree-sitter based syntax highlighting for the editor pane.
//
// The heavy lifting is done by the tree-sitter runtime plus five vendored
// grammars (Python, HTML, CSS, JavaScript, Markdown). We walk the parse tree,
// classify each interesting node into a small colour palette, and apply the
// resulting ranges with wxTextCtrl::SetStyle. Markdown is special: it ships as
// two grammars (block + inline), so block nodes labelled `inline` are re-parsed
// with the inline grammar restricted to that node's range.

#include "syntax.h"

#include <wx/settings.h>
#include <wx/textctrl.h>

#include <tree_sitter/api.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Grammar entry points (defined in the vendored parser.c files).
extern "C" {
const TSLanguage *tree_sitter_python(void);
const TSLanguage *tree_sitter_html(void);
const TSLanguage *tree_sitter_css(void);
const TSLanguage *tree_sitter_javascript(void);
const TSLanguage *tree_sitter_markdown(void);
const TSLanguage *tree_sitter_markdown_inline(void);
}

namespace syntax {

namespace {

// Documents beyond this size are shown without highlighting. wxTextCtrl
// styling costs ~0.1 ms/range on GTK, so a size cap plus a token cap keep a
// full re-highlight (also runs after a typing pause) well under a second.
constexpr size_t kHighlightLimitBytes = 128 * 1024;
constexpr size_t kMaxTokens = 8000;

enum class Cat : uint8_t {
    None = 0,
    Comment,
    String,
    Number,
    Keyword,
    Const,
    Func,
    Type,
    Prop,
    Decor,
    Heading,
    Count,
};

struct Palette {
    wxColour c[static_cast<int>(Cat::Count)];
};

bool IsDark(wxTextCtrl *ctrl) {
    wxColour bg = ctrl->GetBackgroundColour();
    if (!bg.IsOk())
        bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    return bg.Red() * 299 + bg.Green() * 587 + bg.Blue() * 114 < 50000;
}

Palette MakePalette(bool dark) {
    Palette p;
    if (dark) {
        p.c[(int)Cat::Comment] = wxColour(0x6A, 0x99, 0x55);
        p.c[(int)Cat::String] = wxColour(0xCE, 0x91, 0x78);
        p.c[(int)Cat::Number] = wxColour(0xB5, 0xCE, 0xA8);
        p.c[(int)Cat::Keyword] = wxColour(0x56, 0x9C, 0xD6);
        p.c[(int)Cat::Const] = wxColour(0xC5, 0x86, 0xC0);
        p.c[(int)Cat::Func] = wxColour(0xDC, 0xDC, 0xAA);
        p.c[(int)Cat::Type] = wxColour(0x4E, 0xC9, 0xB0);
        p.c[(int)Cat::Prop] = wxColour(0x9C, 0xDC, 0xFE);
        p.c[(int)Cat::Decor] = wxColour(0xD7, 0xBA, 0x7D);
        p.c[(int)Cat::Heading] = wxColour(0x4F, 0xC1, 0xFF);
        p.c[(int)Cat::None] = wxColour(0xD4, 0xD4, 0xD4);
    } else {
        p.c[(int)Cat::Comment] = wxColour(0x00, 0x80, 0x00);
        p.c[(int)Cat::String] = wxColour(0xA3, 0x15, 0x15);
        p.c[(int)Cat::Number] = wxColour(0x09, 0x86, 0x58);
        p.c[(int)Cat::Keyword] = wxColour(0x00, 0x00, 0xFF);
        p.c[(int)Cat::Const] = wxColour(0xAF, 0x00, 0xDB);
        p.c[(int)Cat::Func] = wxColour(0x79, 0x5E, 0x26);
        p.c[(int)Cat::Type] = wxColour(0x26, 0x7F, 0x99);
        p.c[(int)Cat::Prop] = wxColour(0x04, 0x51, 0xA5);
        p.c[(int)Cat::Decor] = wxColour(0x80, 0x00, 0x00);
        p.c[(int)Cat::Heading] = wxColour(0x1F, 0x6F, 0xEB);
        p.c[(int)Cat::None] = wxColour(0x00, 0x00, 0x00);
    }
    return p;
}

wxTextAttr Attr(const Palette &p, Cat cat) {
    wxTextAttr a(p.c[(int)cat]);
    if (cat == Cat::Keyword || cat == Cat::Heading)
        a.SetFontWeight(wxFONTWEIGHT_BOLD);
    return a;
}

wxTextAttr DefaultAttr(const Palette &p) {
    wxTextAttr a(p.c[(int)Cat::None]);
    a.SetFontWeight(wxFONTWEIGHT_NORMAL);
    return a;
}

// Per-language classification tables.
struct Lang {
    const TSLanguage *language = nullptr;
    std::unordered_set<std::string> comment, string, number, keyword, constant,
        func, type, prop, decor, heading;
    // Node types whose whole subtree is one colour (don't recurse into them).
    std::unordered_set<std::string> skip;
    // Node types that can be a definition name (identifier, property_identifier…).
    std::unordered_set<std::string> nameTypes;
    // Parent node type -> category for a name child (def/class names…).
    std::unordered_map<std::string, Cat> nameParent;
    // Identifier names painted as constants (builtins, `self`, …).
    std::unordered_set<std::string> builtins;
};

template <size_t N>
void Add(std::unordered_set<std::string> &s, const char *const (&arr)[N]) {
    for (const char *x : arr)
        s.insert(x);
}

Lang Python() {
    Lang l;
    l.language = tree_sitter_python();
    static const char *const kw[] = {
        "and", "as", "assert", "async", "await", "break", "class", "continue",
        "def", "del", "elif", "else", "except", "exec", "finally", "for",
        "from", "global", "if", "import", "in", "is", "lambda", "nonlocal",
        "not", "or", "pass", "print", "raise", "return", "try", "while",
        "with", "yield", "is not", "not in"};
    static const char *const com[] = {"comment"};
    static const char *const str[] = {"string", "concatenated_string",
                                      "interpolation"};
    static const char *const num[] = {"integer", "float"};
    static const char *const con[] = {"true", "false", "none"};
    static const char *const dec[] = {"decorator"};
    static const char *const skip[] = {"string", "concatenated_string",
                                       "interpolation", "decorator", "comment"};
    static const char *const names[] = {"identifier"};
    static const char *const builtins[] = {
        "print", "len", "range", "str", "int", "float", "bool", "list", "dict",
        "set", "tuple", "type", "isinstance", "issubclass", "super", "object",
        "enumerate", "zip", "map", "filter", "sorted", "reversed", "min", "max",
        "sum", "abs", "open", "input", "repr", "format", "staticmethod",
        "classmethod", "property", "hasattr", "getattr", "setattr", "delattr",
        "iter", "next", "any", "all", "divmod", "round", "pow", "vars", "dir",
        "id", "hash", "bytes", "bytearray", "memoryview", "complex",
        "frozenset", "slice", "chr", "ord", "hex", "oct", "bin", "self",
        "cls"};
    Add(l.keyword, kw);
    Add(l.comment, com);
    Add(l.string, str);
    Add(l.number, num);
    Add(l.constant, con);
    Add(l.decor, dec);
    Add(l.skip, skip);
    Add(l.nameTypes, names);
    Add(l.builtins, builtins);
    l.nameParent["function_definition"] = Cat::Func;
    l.nameParent["class_definition"] = Cat::Type;
    l.nameParent["type"] = Cat::Type;
    l.nameParent["generic_type"] = Cat::Type;
    return l;
}

Lang Html() {
    Lang l;
    l.language = tree_sitter_html();
    static const char *const com[] = {"comment"};
    static const char *const str[] = {"quoted_attribute_value",
                                      "attribute_value"};
    static const char *const con[] = {"entity"};
    static const char *const typ[] = {"tag_name"};
    static const char *const prop[] = {"attribute_name"};
    static const char *const dec[] = {"doctype"};
    static const char *const skip[] = {"comment", "quoted_attribute_value",
                                       "attribute_value", "entity", "raw_text",
                                       "text"};
    Add(l.comment, com);
    Add(l.string, str);
    Add(l.constant, con);
    Add(l.type, typ);
    Add(l.prop, prop);
    Add(l.decor, dec);
    Add(l.skip, skip);
    return l;
}

Lang Css() {
    Lang l;
    l.language = tree_sitter_css();
    static const char *const com[] = {"comment", "js_comment"};
    static const char *const str[] = {"string_value"};
    static const char *const num[] = {"integer_value", "float_value",
                                      "color_value"};
    static const char *const kw[] = {"important", "and", "or", "not", "only",
                                     "of", "from", "to"};
    static const char *const con[] = {"class_name", "id_name",
                                      "pseudo_class_selector",
                                      "pseudo_element_selector"};
    static const char *const typ[] = {"tag_name", "universal_selector",
                                      "nesting_selector"};
    static const char *const prop[] = {"property_name", "attribute_name"};
    static const char *const func[] = {"function_name"};
    static const char *const dec[] = {
        "at_keyword", "at_rule", "charset_statement", "import_statement",
        "media_statement", "namespace_statement", "supports_statement",
        "keyframes_statement", "keyword_query"};
    static const char *const skip[] = {"comment", "js_comment", "string_value",
                                       "integer_value", "float_value",
                                       "color_value"};
    Add(l.comment, com);
    Add(l.string, str);
    Add(l.number, num);
    Add(l.keyword, kw);
    Add(l.constant, con);
    Add(l.type, typ);
    Add(l.prop, prop);
    Add(l.func, func);
    Add(l.decor, dec);
    Add(l.skip, skip);
    return l;
}

Lang Javascript() {
    Lang l;
    l.language = tree_sitter_javascript();
    static const char *const com[] = {"comment", "hash_bang_line"};
    static const char *const str[] = {"string", "template_string", "regex"};
    static const char *const num[] = {"number"};
    static const char *const kw[] = {
        "break", "case", "catch", "class", "const", "continue", "debugger",
        "default", "delete", "do", "else", "export", "extends", "finally",
        "for", "function", "if", "import", "in", "instanceof", "let", "new",
        "of", "return", "static", "super", "switch", "this", "throw", "try",
        "typeof", "var", "void", "while", "with", "yield", "async", "await",
        "get", "set"};
    static const char *const con[] = {"true", "false", "null", "undefined"};
    static const char *const prop[] = {"property_identifier",
                                       "shorthand_property_identifier"};
    static const char *const dec[] = {"decorator"};
    static const char *const skip[] = {"comment", "hash_bang_line", "string",
                                       "template_string", "regex", "number",
                                       "decorator"};
    static const char *const names[] = {"identifier", "property_identifier",
                                        "shorthand_property_identifier"};
    static const char *const builtins[] = {
        "console", "Math", "JSON", "Object", "Array", "String", "Number",
        "Boolean", "Date", "RegExp", "Promise", "Map", "Set", "Symbol",
        "BigInt", "parseInt", "parseFloat", "isNaN", "isFinite",
        "encodeURIComponent", "decodeURIComponent", "setTimeout", "setInterval",
        "clearTimeout", "clearInterval", "require", "module", "exports",
        "process", "window", "document", "self", "this"};
    Add(l.comment, com);
    Add(l.string, str);
    Add(l.number, num);
    Add(l.keyword, kw);
    Add(l.constant, con);
    Add(l.prop, prop);
    Add(l.decor, dec);
    Add(l.skip, skip);
    Add(l.nameTypes, names);
    Add(l.builtins, builtins);
    l.nameParent["function_declaration"] = Cat::Func;
    l.nameParent["generator_function_declaration"] = Cat::Func;
    l.nameParent["method_definition"] = Cat::Func;
    l.nameParent["class_declaration"] = Cat::Type;
    return l;
}

Lang MarkdownBlock() {
    Lang l;
    l.language = tree_sitter_markdown();
    static const char *const com[] = {"minus_metadata", "plus_metadata"};
    static const char *const str[] = {"fenced_code_block", "indented_code_block",
                                      "code_fence_content", "html_block",
                                      "link_destination", "link_title",
                                      "link_label"};
    static const char *const con[] = {"entity_reference",
                                      "numeric_character_reference"};
    static const char *const dec[] = {
        "fenced_code_block_delimiter", "info_string", "atx_h1_marker",
        "atx_h2_marker", "atx_h3_marker", "atx_h4_marker", "atx_h5_marker",
        "atx_h6_marker", "setext_h1_underline", "setext_h2_underline",
        "block_quote_marker", "list_marker_dot", "list_marker_minus",
        "list_marker_parenthesis", "list_marker_plus", "list_marker_star",
        "task_list_marker_checked", "task_list_marker_unchecked",
        "thematic_break"};
    static const char *const head[] = {"atx_heading", "setext_heading"};
    static const char *const skip[] = {
        "fenced_code_block", "indented_code_block", "code_fence_content",
        "html_block", "link_destination", "link_title", "link_label",
        "entity_reference", "numeric_character_reference",
        "fenced_code_block_delimiter", "info_string", "minus_metadata",
        "plus_metadata", "thematic_break"};
    Add(l.comment, com);
    Add(l.string, str);
    Add(l.constant, con);
    Add(l.decor, dec);
    Add(l.heading, head);
    Add(l.skip, skip);
    return l;
}

Lang MarkdownInline() {
    Lang l;
    l.language = tree_sitter_markdown_inline();
    static const char *const str[] = {"code_span", "latex_block",
                                      "email_autolink", "uri_autolink",
                                      "link_destination", "link_title",
                                      "link_label"};
    static const char *const con[] = {"entity_reference",
                                      "numeric_character_reference"};
    static const char *const kw[] = {"strong_emphasis"};
    static const char *const typ[] = {"emphasis"};
    static const char *const com[] = {"strikethrough"};
    static const char *const dec[] = {"html_tag", "image"};
    static const char *const skip[] = {
        "code_span", "latex_block", "email_autolink", "uri_autolink",
        "link_destination", "link_title", "link_label", "entity_reference",
        "numeric_character_reference", "strong_emphasis", "emphasis",
        "strikethrough", "html_tag", "image"};
    Add(l.string, str);
    Add(l.constant, con);
    Add(l.keyword, kw);
    Add(l.type, typ);
    Add(l.comment, com);
    Add(l.decor, dec);
    Add(l.skip, skip);
    return l;
}

const Lang &LangFor(std::string_view path) {
    static const Lang py = Python();
    static const Lang html = Html();
    static const Lang css = Css();
    static const Lang js = Javascript();
    static const Lang md = MarkdownBlock();
    // Lowercased extension match.
    size_t dot = path.find_last_of('.');
    if (dot != std::string_view::npos) {
        std::string ext(path.substr(dot));
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (ext == ".py" || ext == ".pyw")
            return py;
        if (ext == ".html" || ext == ".htm")
            return html;
        if (ext == ".css")
            return css;
        if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs")
            return js;
        if (ext == ".md" || ext == ".markdown")
            return md;
    }
    // Unknown language: return a default-constructed Lang with language ==
    // nullptr so callers can skip parsing.
    static const Lang none;
    return none;
}

Cat BaseCategory(const Lang &l, const std::string &t) {
    if (l.comment.count(t))
        return Cat::Comment;
    if (l.string.count(t))
        return Cat::String;
    if (l.number.count(t))
        return Cat::Number;
    if (l.keyword.count(t))
        return Cat::Keyword;
    if (l.constant.count(t))
        return Cat::Const;
    if (l.func.count(t))
        return Cat::Func;
    if (l.type.count(t))
        return Cat::Type;
    if (l.prop.count(t))
        return Cat::Prop;
    if (l.decor.count(t))
        return Cat::Decor;
    if (l.heading.count(t))
        return Cat::Heading;
    return Cat::None;
}

Cat Classify(const Lang &l, const std::string &t, TSNode node,
             std::string_view text) {
    if (l.nameTypes.count(t)) {
        TSNode p = ts_node_parent(node);
        if (!ts_node_is_null(p)) {
            auto it = l.nameParent.find(ts_node_type(p));
            if (it != l.nameParent.end())
                return it->second;
        }
        Cat base = BaseCategory(l, t);
        if (base != Cat::None)
            return base;
        uint32_t s = ts_node_start_byte(node);
        uint32_t e = ts_node_end_byte(node);
        if (e > s && e <= text.size() &&
            l.builtins.count(std::string(text.substr(s, e - s))))
            return Cat::Const;
        return Cat::None;
    }
    return BaseCategory(l, t);
}

struct Token {
    uint32_t b0, b1;
    Cat cat;
};

void Walk(const Lang &l, TSNode node, std::string_view text,
          std::vector<Token> &out) {
    if (out.size() >= kMaxTokens)
        return;
    const char *tn = ts_node_type(node);
    std::string t = tn ? tn : "";
    Cat cat = Classify(l, t, node, text);
    if (cat != Cat::None) {
        uint32_t s = ts_node_start_byte(node);
        uint32_t e = ts_node_end_byte(node);
        if (e > s && e <= text.size())
            out.push_back({s, e, cat});
    }
    if (l.skip.count(t))
        return;
    // Recurse over ALL children: keywords like `def`/`import`/`return` are
    // anonymous nodes in several grammars and would be missed by a named-only
    // walk.
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i)
        Walk(l, ts_node_child(node, i), text, out);
}

// Markdown block walker: `inline` / `pipe_table_cell` nodes are re-parsed with
// the inline grammar restricted to that node's range.
void WalkMarkdownBlock(const Lang &block, const Lang &il, TSParser *ip,
                       TSNode node, std::string_view text,
                       std::vector<Token> &out) {
    if (out.size() >= kMaxTokens)
        return;
    const char *tn = ts_node_type(node);
    std::string t = tn ? tn : "";
    if (t == "inline" || t == "pipe_table_cell") {
        std::vector<TSRange> ranges;
        uint32_t s = ts_node_start_byte(node);
        uint32_t e = ts_node_end_byte(node);
        TSPoint sp = ts_node_start_point(node);
        TSPoint ep = ts_node_end_point(node);
        uint32_t cur = s;
        TSPoint curp = sp;
        uint32_t n = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < n; ++i) {
            TSNode ch = ts_node_named_child(node, i);
            uint32_t cs = ts_node_start_byte(ch);
            TSPoint csp = ts_node_start_point(ch);
            if (cs > cur)
                ranges.push_back({curp, csp, cur, cs});
            cur = ts_node_end_byte(ch);
            curp = ts_node_end_point(ch);
        }
        if (cur < e)
            ranges.push_back({curp, ep, cur, e});
        if (ranges.empty())
            return;
        ts_parser_set_included_ranges(ip, ranges.data(),
                                      (uint32_t)ranges.size());
        TSTree *tree = ts_parser_parse_string(ip, nullptr, text.data(),
                                              (uint32_t)text.size());
        if (!tree)
            return;
        Walk(il, ts_tree_root_node(tree), text, out);
        ts_tree_delete(tree);
        return;
    }
    Cat cat = Classify(block, t, node, text);
    if (cat != Cat::None) {
        uint32_t s = ts_node_start_byte(node);
        uint32_t e = ts_node_end_byte(node);
        if (e > s && e <= text.size())
            out.push_back({s, e, cat});
    }
    if (block.skip.count(t))
        return;
    uint32_t n = ts_node_child_count(node);
    for (uint32_t i = 0; i < n; ++i)
        WalkMarkdownBlock(block, il, ip, ts_node_child(node, i), text, out);
}

std::vector<uint32_t> BuildCharOffsets(std::string_view text) {
    std::vector<uint32_t> offs(text.size() + 1);
    uint32_t chars = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        offs[i] = chars;
        if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80)
            ++chars;
    }
    offs[text.size()] = chars;
    return offs;
}

void Apply(wxTextCtrl *ctrl, std::string_view text,
           const std::vector<Token> &tokens, const Palette &p) {
    ctrl->SetDefaultStyle(DefaultAttr(p));
    auto offs = BuildCharOffsets(text);
    long last = static_cast<long>(offs[text.size()]);
    ctrl->SetStyle(0, last, DefaultAttr(p));
    for (const Token &t : tokens) {
        long a = static_cast<long>(offs[t.b0]);
        long b = static_cast<long>(offs[t.b1]);
        if (b <= a)
            continue;
        ctrl->SetStyle(a, b, Attr(p, t.cat));
    }
}

}  // namespace

void ClearStyles(wxTextCtrl *ctrl) {
    if (!ctrl)
        return;
    Palette p = MakePalette(IsDark(ctrl));
    ctrl->SetDefaultStyle(DefaultAttr(p));
    ctrl->SetStyle(0, ctrl->GetLastPosition(), DefaultAttr(p));
}

void Highlight(wxTextCtrl *ctrl, const wxString &path,
               std::string_view utf8Text) {
    if (!ctrl)
        return;
    const Lang &l = LangFor(path.ToStdString(wxConvUTF8));
    Palette p = MakePalette(IsDark(ctrl));
    std::vector<Token> tokens;
    if (l.language && utf8Text.size() <= kHighlightLimitBytes) {
        TSParser *parser = ts_parser_new();
        ts_parser_set_language(parser, l.language);
        TSTree *tree = ts_parser_parse_string(parser, nullptr, utf8Text.data(),
                                              (uint32_t)utf8Text.size());
        if (tree) {
            if (l.language == tree_sitter_markdown()) {
                static const Lang il = MarkdownInline();
                TSParser *ip = ts_parser_new();
                ts_parser_set_language(ip, il.language);
                WalkMarkdownBlock(l, il, ip, ts_tree_root_node(tree),
                                  utf8Text, tokens);
                ts_parser_delete(ip);
            } else {
                Walk(l, ts_tree_root_node(tree), utf8Text, tokens);
            }
            ts_tree_delete(tree);
        }
        ts_parser_delete(parser);
    }
    Apply(ctrl, utf8Text, tokens, p);
}

}  // namespace syntax
