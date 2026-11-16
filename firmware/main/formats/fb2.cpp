// FB2 (FictionBook 2) reader.
//
// A deliberately tolerant, dependency-free XML scanner. Real FB2 files contain
// undeclared namespaces, stray ampersands, unclosed tags and multi-megabyte
// base64 <binary> blocks, so the parser never throws, never loops on a hostile
// input and always makes forward progress. The whole document is parsed once at
// open time into a flat paragraph vector; seeking then becomes a binary search.
#include "formats/formats.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/byte_source.h"
#include "core/encoding.h"
#include "core/utf8.h"
#include "text/doc.h"
#include "text/types.h"

namespace reader {
namespace {

constexpr size_t kMaxSourceBytes = 6u * 1024u * 1024u;
constexpr size_t kMaxDecodedBytes = 6u * 1024u * 1024u;
constexpr size_t kEncodingProbeBytes = 4u * 1024u;
constexpr int kMaxBlockDepth = 64;
constexpr int kMaxInlineDepth = 64;
constexpr int kMaxHeadingLevel = 6;

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

bool is_ascii_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool is_name_start(char c) { return is_ascii_alpha(c) || c == '_' || c == ':'; }

bool is_name_char(char c) {
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

std::string ascii_lower(const std::string& text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool same_style(const Style& a, const Style& b) {
    return a.face == b.face && a.small == b.small;
}

Style bold_style(Style style) {
    if (style.face == Face::Regular) style.face = Face::Bold;
    else if (style.face == Face::Italic) style.face = Face::BoldItalic;
    return style;
}

Style italic_style(Style style) {
    if (style.face == Face::Regular) style.face = Face::Italic;
    else if (style.face == Face::Bold) style.face = Face::BoldItalic;
    return style;
}

Style mono_style(Style style) {
    style.face = Face::Mono;
    return style;
}

uint8_t heading_level(int section_level) {
    int level = section_level < 1 ? 1 : section_level;
    if (level > kMaxHeadingLevel) level = kMaxHeadingLevel;
    return static_cast<uint8_t>(level);
}

// Decodes the entity starting at `pos` ('&'). Returns false when there is no
// terminating ';' close by, in which case the caller keeps a literal '&'.
// Recognized but unknown entities are consumed and dropped.
bool decode_entity(const std::string& text, size_t pos, size_t end, size_t& consumed,
                   std::string& replacement) {
    replacement.clear();
    const size_t scan_end = pos + 12 < end ? pos + 12 : end;
    size_t semi = std::string::npos;
    for (size_t i = pos + 1; i < scan_end; ++i) {
        if (text[i] == ';') {
            semi = i;
            break;
        }
    }
    if (semi == std::string::npos) {
        consumed = 1;
        return false;
    }
    const std::string body = text.substr(pos + 1, semi - pos - 1);
    consumed = semi - pos + 1;
    if (body.empty()) return true;

    if (body[0] == '#') {
        uint64_t value = 0;
        bool ok = false;
        if (body.size() > 2 && (body[1] == 'x' || body[1] == 'X')) {
            ok = true;
            for (size_t i = 2; i < body.size(); ++i) {
                const char c = body[i];
                uint32_t digit = 0;
                if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
                else {
                    ok = false;
                    break;
                }
                value = value * 16 + digit;
                if (value > 0x10FFFF) {
                    ok = false;
                    break;
                }
            }
        } else if (body.size() > 1) {
            ok = true;
            for (size_t i = 1; i < body.size(); ++i) {
                const char c = body[i];
                if (c < '0' || c > '9') {
                    ok = false;
                    break;
                }
                value = value * 10 + static_cast<uint32_t>(c - '0');
                if (value > 0x10FFFF) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok || value == 0 || (value >= 0xD800 && value <= 0xDFFF)) return true;
        utf8_append(replacement, static_cast<uint32_t>(value));
        return true;
    }

    if (body == "amp") replacement = "&";
    else if (body == "lt") replacement = "<";
    else if (body == "gt") replacement = ">";
    else if (body == "quot") replacement = "\"";
    else if (body == "apos") replacement = "'";
    return true;
}

std::string decode_text(const std::string& text, size_t begin, size_t end) {
    std::string out;
    out.reserve(end - begin);
    size_t pos = begin;
    while (pos < end) {
        if (text[pos] != '&') {
            out.push_back(text[pos]);
            ++pos;
            continue;
        }
        size_t consumed = 0;
        std::string replacement;
        if (decode_entity(text, pos, end, consumed, replacement)) {
            out += replacement;
            pos += consumed;
        } else {
            out.push_back('&');
            ++pos;
        }
    }
    return out;
}

enum class TokenType { Text, StartTag, EndTag, Eof };

struct Token {
    TokenType type = TokenType::Eof;
    std::string name; // lowercased tag name
    std::string text; // decoded character data
    size_t offset = 0;
    bool self_closing = false;
    // Attribute values the parsers care about: href (links) and id (anchor target).
    std::string href;
    std::string id;
    std::string name_attr; // <body name="notes">
};

// Tolerant XML tokenizer. Anything it does not understand is skipped, and a
// lone '<' is handed back as text so no input can stall it.
class Scanner {
  public:
    explicit Scanner(const std::string& text) : _text(text) {}

    // Two-pass parsing (notes first, then the text) needs a restart.
    void rewind() { _pos = 0; }

    bool next(Token& token) {
        for (;;) {
            token = Token{};
            if (_pos >= _text.size()) {
                token.type = TokenType::Eof;
                return false;
            }
            if (_text[_pos] != '<') {
                const size_t start = _pos;
                const size_t lt = _text.find('<', _pos);
                const size_t end = lt == std::string::npos ? _text.size() : lt;
                token.type = TokenType::Text;
                token.offset = start;
                token.text = decode_text(_text, start, end);
                _pos = end;
                return true;
            }
            if (starts_with("<!--")) {
                _pos += 4;
                const size_t end = _text.find("-->", _pos);
                _pos = end == std::string::npos ? _text.size() : end + 3;
                continue;
            }
            if (starts_with("<![CDATA[")) {
                _pos += 9;
                const size_t end = _text.find("]]>", _pos);
                const size_t content_end = end == std::string::npos ? _text.size() : end;
                token.type = TokenType::Text;
                token.offset = _pos;
                token.text.assign(_text, _pos, content_end - _pos);
                _pos = end == std::string::npos ? _text.size() : end + 3;
                if (token.text.empty()) continue;
                return true;
            }
            if (starts_with("<!")) {
                skip_declaration();
                continue;
            }
            if (starts_with("<?")) {
                _pos += 2;
                const size_t end = _text.find("?>", _pos);
                _pos = end == std::string::npos ? _text.size() : end + 2;
                continue;
            }
            if (starts_with("</")) {
                parse_end_tag(token);
                return true;
            }
            if (_pos + 1 < _text.size() && is_name_start(_text[_pos + 1])) {
                parse_start_tag(token);
                return true;
            }
            token.type = TokenType::Text;
            token.offset = _pos;
            token.text = "<";
            ++_pos;
            return true;
        }
    }

  private:
    bool starts_with(const char* prefix) const {
        const size_t len = std::char_traits<char>::length(prefix);
        return _text.size() - _pos >= len && _text.compare(_pos, len, prefix) == 0;
    }

    void skip_declaration() {
        size_t pos = _pos + 2;
        int brackets = 0;
        char quote = 0;
        while (pos < _text.size()) {
            const char c = _text[pos];
            if (quote != 0) {
                if (c == quote) quote = 0;
                ++pos;
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
                ++pos;
                continue;
            }
            if (c == '[') {
                ++brackets;
                ++pos;
                continue;
            }
            if (c == ']') {
                if (brackets > 0) --brackets;
                ++pos;
                continue;
            }
            if (c == '>' && brackets == 0) {
                _pos = pos + 1;
                return;
            }
            ++pos;
        }
        _pos = _text.size();
    }

    void parse_end_tag(Token& token) {
        size_t pos = _pos + 2;
        const size_t name_start = pos;
        while (pos < _text.size() && is_name_char(_text[pos])) ++pos;
        token.type = TokenType::EndTag;
        token.name = ascii_lower(_text.substr(name_start, pos - name_start));
        token.offset = _pos;
        while (pos < _text.size() && _text[pos] != '>') ++pos;
        _pos = pos < _text.size() ? pos + 1 : _text.size();
    }

    void parse_start_tag(Token& token) {
        size_t pos = _pos + 1;
        const size_t name_start = pos;
        while (pos < _text.size() && is_name_char(_text[pos])) ++pos;
        token.type = TokenType::StartTag;
        token.name = ascii_lower(_text.substr(name_start, pos - name_start));
        token.offset = _pos;
        token.href.clear();
        token.id.clear();
        token.name_attr.clear();
        while (pos < _text.size()) {
            const char c = _text[pos];
            if (is_space(c)) {
                ++pos;
                continue;
            }
            if (c == '>') {
                ++pos;
                break;
            }
            if (c == '/') {
                size_t probe = pos + 1;
                while (probe < _text.size() && is_space(_text[probe])) ++probe;
                if (probe < _text.size() && _text[probe] == '>') {
                    token.self_closing = true;
                    pos = probe + 1;
                    break;
                }
                ++pos;
                continue;
            }
            if (c == '"' || c == '\'') {
                const char quote = c;
                ++pos;
                while (pos < _text.size() && _text[pos] != quote) ++pos;
                if (pos < _text.size()) ++pos;
                continue;
            }
            // Attribute: name="value" / name='value'. Only a few names are kept.
            const size_t attr_start = pos;
            while (pos < _text.size() && _text[pos] != '=' && _text[pos] != '>' && !is_space(_text[pos])) {
                ++pos;
            }
            const size_t attr_end = pos;
            while (pos < _text.size() && is_space(_text[pos])) ++pos;
            if (pos < _text.size() && _text[pos] == '=') {
                ++pos;
                while (pos < _text.size() && is_space(_text[pos])) ++pos;
                if (pos < _text.size() && (_text[pos] == '"' || _text[pos] == '\'')) {
                    const char quote = _text[pos];
                    const size_t value_start = ++pos;
                    while (pos < _text.size() && _text[pos] != quote) ++pos;
                    const std::string attr = ascii_lower(_text.substr(attr_start, attr_end - attr_start));
                    const std::string value = _text.substr(value_start, pos - value_start);
                    if (attr == "l:href" || attr == "href") {
                        token.href = value;
                    } else if (attr == "id") {
                        token.id = value;
                    } else if (attr == "name") {
                        token.name_attr = value;
                    }
                    if (pos < _text.size()) ++pos;
                }
            }
        }
        _pos = pos;
    }

    const std::string& _text;
    size_t _pos = 0;
};

// Whitespace-normalizing text sink for titles and metadata.
class TextCollector {
  public:
    void add(const std::string& raw) {
        for (char c : raw) {
            if (is_space(c)) {
                if (!_text.empty()) _pending_space = true;
                continue;
            }
            if (_pending_space) {
                _text.push_back(' ');
                _pending_space = false;
            }
            _text.push_back(c);
        }
    }

    const std::string& text() const { return _text; }
    bool empty() const { return _text.empty(); }

  private:
    std::string _text;
    bool _pending_space = false;
};

// Accumulates styled inline runs while collapsing whitespace *across* run
// boundaries, so "a <emphasis> b </emphasis>" keeps a single space.
class InlineAccumulator {
  public:
    void add_text(Style style, const std::string& raw) {
        size_t pos = 0;
        bool leading_space = false;
        while (pos < raw.size() && is_space(raw[pos])) {
            leading_space = true;
            ++pos;
        }
        if (leading_space && !_runs.empty()) _pending_space = true;

        std::string cleaned;
        cleaned.reserve(raw.size() - pos);
        bool pending_space = false;
        for (; pos < raw.size(); ++pos) {
            const char c = raw[pos];
            if (is_space(c)) {
                pending_space = true;
                continue;
            }
            if (pending_space) {
                cleaned.push_back(' ');
                pending_space = false;
            }
            cleaned.push_back(c);
        }
        if (cleaned.empty()) {
            if (pending_space && !_runs.empty()) _pending_space = true;
            return;
        }
        if (_pending_space && !_runs.empty()) cleaned.insert(cleaned.begin(), ' ');
        _pending_space = pending_space;
        push(style, cleaned);
    }

    void finalize() {
        while (!_runs.empty()) {
            std::string& text = _runs.back().text;
            while (!text.empty() && is_space(text.back())) text.pop_back();
            if (!text.empty()) break;
            _runs.pop_back();
        }
    }

    bool empty() const { return _runs.empty(); }

    std::vector<Run> take() { return std::move(_runs); }

    // Link bookkeeping: the anchor text is added normally by the caller, these two
    // calls only remember which bytes of which run belong to the reference.
    void begin_link() {
        _link_run = _runs.empty() ? 0 : _runs.size() - 1;
        _link_begin = _runs.empty() ? 0 : static_cast<uint32_t>(_runs.back().text.size());
        _link_active = true;
    }

    void end_link(uint32_t note) {
        if (!_link_active || _runs.empty()) return;
        _link_active = false;
        const uint32_t run = static_cast<uint32_t>(_runs.size() - 1);
        uint32_t begin = run == _link_run ? _link_begin : 0;
        const uint32_t end = static_cast<uint32_t>(_runs.back().text.size());
        // The accumulator may have inserted a separator space before the marker.
        while (begin < end && is_space(_runs.back().text[begin])) ++begin;
        if (end <= begin) return;
        Link link;
        link.run = run;
        link.begin = begin;
        link.end = end;
        link.note = note;
        _links.push_back(link);
    }

    std::vector<Link> take_links() { return std::move(_links); }

  private:
    void push(Style style, const std::string& text) {
        if (text.empty()) return;
        if (!_runs.empty() && same_style(_runs.back().style, style)) {
            _runs.back().text += text;
            return;
        }
        Run run;
        run.style = style;
        run.text = text;
        _runs.push_back(std::move(run));
    }

    std::vector<Run> _runs;
    std::vector<Link> _links;
    bool _pending_space = false;
    bool _link_active = false;
    uint32_t _link_run = 0;
    uint32_t _link_begin = 0;
};

// Parses the UTF-8 document into DocInfo plus a flat paragraph list.
class Fb2Parser {
  public:
    // First pass: collect the note texts of <body name="notes">. Each <section id="…">
    // becomes one note, numbered from 1 in document order; the id map resolves the
    // references inside the main text.
    void collect_notes() {
        Token token;
        bool in_notes = false;
        bool started = false;
        bool in_note_title = false;
        int title_depth = 0;
        std::string current_id;
        TextCollector current;
        while (_scanner.next(token)) {
            if (token.type == TokenType::Eof) break;
            if (token.type == TokenType::StartTag) {
                if (token.name == "body" && token.name_attr == "notes" && !token.self_closing) {
                    in_notes = true;
                    continue;
                }
                if (!in_notes) continue;
                if (token.name == "section" && !token.self_closing) {
                    flush_note(current_id, current); // previous note ends here
                    started = true;
                    current_id = token.id;
                    continue;
                }
                // <title><p>1</p></title> becomes the panel heading, not the body.
                if (token.name == "title" && !token.self_closing) {
                    in_note_title = true;
                    title_depth = 1;
                    continue;
                }
                if (token.name == "binary" && !token.self_closing) skip_element("binary");
                continue;
            }
            if (token.type == TokenType::EndTag) {
                if (!in_notes) continue;
                if (token.name == "body") {
                    flush_note(current_id, current);
                    in_notes = false;
                    started = false;
                    continue;
                }
                if (token.name == "section") {
                    flush_note(current_id, current);
                    started = false;
                }
                if (token.name == "title") {
                    in_note_title = false;
                    title_depth = 0;
                }
                continue;
            }
            if (in_notes && started && token.type == TokenType::Text) {
                if (in_note_title) {
                    _note_title_pending.add(token.text);
                    _note_title_pending.add(" ");
                } else {
                    current.add(token.text);
                    current.add(" ");
                }
            }
        }
        flush_note(current_id, current);
    }

    void flush_note(const std::string& id, TextCollector& collector) {
        if (id.empty()) {
            collector = TextCollector();
            return;
        }
        std::string text = collector.text();
        std::string title = _note_title_pending.text();
        _note_title_pending = TextCollector();
        while (!title.empty() && is_space(title.back())) title.pop_back();
        // A heading that is just the note number ("6") duplicates the marker.
        {
            size_t digits = 0;
            while (digits < title.size() && digits < 4 && title[digits] >= (char)0x30 &&
                   title[digits] <= (char)0x39) {
                ++digits;
            }
            size_t skip = digits;
            while (skip < title.size() && (is_space(title[skip]) || title[skip] == (char)0x2E ||
                                           title[skip] == (char)0x29)) {
                ++skip;
            }
            if (digits > 0) title = title.substr(std::min(skip, title.size()));
        }
        while (!text.empty() && is_space(text.back())) text.pop_back();
        // Many FB2 files repeat the note number as a heading inside the note:
        // "1 Posvyashchaetsya..." — the number is already shown as the marker.
        size_t digits = 0;
        while (digits < text.size() && digits < 4 && text[digits] >= (char)0x30 && text[digits] <= (char)0x39) ++digits;
        if (digits > 0 && digits < text.size() && (text[digits] == (char)0x2E || is_space(text[digits]))) {
            size_t skip = digits;
            while (skip < text.size() && (is_space(text[skip]) || text[skip] == (char)0x2E || text[skip] == (char)0x29)) ++skip;
            if (skip < text.size()) text.erase(0, skip);
        }
        if (!text.empty()) {
            _notes.push_back(text);
            _note_titles.push_back(title);
            _note_ids[id] = static_cast<uint32_t>(_notes.size()); // 1-based
        }
        collector = TextCollector();
    }

    uint32_t note_index_for(const std::string& href) const {
        if (href.size() < 2 || href[0] != '#') return 0;
        const auto it = _note_ids.find(href.substr(1));
        return it == _note_ids.end() ? 0 : it->second;
    }
    explicit Fb2Parser(const std::string& text) : _scanner(text) {}

    void parse() {
        collect_notes();
        _scanner.rewind();
        Token token;
        while (_scanner.next(token)) {
            if (token.type != TokenType::StartTag) continue;
            if (token.name == "description") {
                if (!token.self_closing) parse_description();
            } else if (token.name == "body") {
                // Notes live in a separate body and must not become part of the text.
                if (token.self_closing) continue;
                if (token.name_attr == "notes") {
                    skip_element("body");
                    continue;
                }
                parse_blocks(0, Style{}, false, "body", 1);
            } else if (token.name == "binary") {
                if (!token.self_closing) skip_element("binary");
            } else if (token.name == "image" || token.name == "fictionbook") {
                continue;
            } else if (!token.self_closing) {
                skip_element(token.name);
            }
        }
        _info.title = _title.text();
        _info.author = join_authors();
    }

    const DocInfo& info() const { return _info; }

    std::vector<Paragraph> take_paragraphs() { return std::move(_paragraphs); }

    std::vector<std::string> take_notes() { return std::move(_notes); }

    std::vector<std::string> take_note_titles() { return std::move(_note_titles); }

  private:
    void parse_description() {
        Token token;
        while (_scanner.next(token)) {
            if (token.type == TokenType::Eof) return;
            if (token.type == TokenType::EndTag) {
                if (token.name == "description") return;
                continue;
            }
            if (token.type != TokenType::StartTag) continue;
            if (token.name == "title-info") {
                if (!token.self_closing) parse_title_info();
            } else if (!token.self_closing) {
                skip_element(token.name);
            }
        }
    }

    void parse_title_info() {
        Token token;
        while (_scanner.next(token)) {
            if (token.type == TokenType::Eof) return;
            if (token.type == TokenType::EndTag) {
                if (token.name == "title-info") return;
                continue;
            }
            if (token.type != TokenType::StartTag) continue;
            if (token.name == "book-title") {
                if (!token.self_closing) {
                    TextCollector collector;
                    collect_text(collector, "book-title", 1);
                    if (!collector.empty()) _title = collector;
                }
            } else if (token.name == "author") {
                if (!token.self_closing) parse_author();
            } else if (token.name == "annotation") {
                // Annotation normally lives in <title-info>; treat it as a quote.
                if (!token.self_closing) parse_blocks(0, Style{}, true, "annotation", 1);
            } else if (!token.self_closing) {
                skip_element(token.name);
            }
        }
    }

    void parse_author() {
        std::string first;
        std::string middle;
        std::string last;
        std::string nickname;
        Token token;
        while (_scanner.next(token)) {
            if (token.type == TokenType::Eof) return;
            if (token.type == TokenType::EndTag) {
                if (token.name == "author") break;
                continue;
            }
            if (token.type != TokenType::StartTag) continue;
            if (token.name == "first-name" || token.name == "middle-name" ||
                token.name == "last-name" || token.name == "nickname") {
                if (token.self_closing) continue;
                TextCollector collector;
                collect_text(collector, token.name, 1);
                if (token.name == "first-name") first = collector.text();
                else if (token.name == "middle-name") middle = collector.text();
                else if (token.name == "last-name") last = collector.text();
                else nickname = collector.text();
            } else if (!token.self_closing) {
                skip_element(token.name);
            }
        }
        std::string author;
        append_part(author, first);
        append_part(author, middle);
        append_part(author, last);
        if (author.empty()) author = nickname;
        if (!author.empty()) _authors.push_back(author);
    }

    static void append_part(std::string& out, const std::string& part) {
        if (part.empty()) return;
        if (!out.empty()) out.push_back(' ');
        out += part;
    }

    std::string join_authors() const {
        std::string out;
        for (const std::string& author : _authors) {
            if (author.empty()) continue;
            if (!out.empty()) out += ", ";
            out += author;
        }
        return out;
    }

    // Collects the normalized character data of an element, ignoring markup.
    void collect_text(TextCollector& collector, const std::string& end_tag, int depth) {
        Token token;
        while (_scanner.next(token)) {
            if (token.type == TokenType::Eof) return;
            if (token.type == TokenType::EndTag) {
                if (token.name == end_tag) return;
                continue;
            }
            if (token.type == TokenType::Text) {
                collector.add(token.text);
                continue;
            }
            if (token.type != TokenType::StartTag) continue;
            if (token.name == "image") continue;
            if (token.name == "binary") {
                if (!token.self_closing) skip_element("binary");
                continue;
            }
            if (token.self_closing) continue;
            if (depth >= kMaxInlineDepth) continue;
            collect_text(collector, token.name, depth + 1);
        }
    }

    // Consumes tokens up to and including the matching end tag of `name`.
    void skip_element(const std::string& name) {
        int depth = 1;
        Token token;
        while (depth > 0 && _scanner.next(token)) {
            if (token.type == TokenType::StartTag) {
                if (!token.self_closing && token.name == name) ++depth;
            } else if (token.type == TokenType::EndTag) {
                if (token.name == name) --depth;
            }
        }
    }

    void parse_blocks(int level, Style base, bool quote, const std::string& end_tag, int depth) {
        Token token;
        while (_scanner.next(token)) {
            if (token.type == TokenType::Eof) return;
            if (token.type == TokenType::EndTag) {
                if (token.name == end_tag) return;
                continue;
            }
            if (token.type == TokenType::Text) {
                emit_stray_text(token, quote, base);
                continue;
            }

            const std::string& name = token.name;
            if (name == "image") continue; // FB2 images are empty markers
            if (name == "binary") {
                if (!token.self_closing) skip_element("binary");
                continue;
            }
            if (name == "empty-line") {
                Paragraph blank;
                blank.kind = ParaKind::Blank;
                blank.token = token.offset;
                _paragraphs.push_back(std::move(blank));
                continue;
            }
            if (token.self_closing) continue;

            const bool too_deep = depth >= kMaxBlockDepth;
            if (name == "p" || name == "v" || name == "text-author") {
                parse_paragraph(quote ? ParaKind::Quote : ParaKind::Text, 0, base, name,
                                token.offset, depth);
            } else if (name == "subtitle") {
                parse_paragraph(ParaKind::Heading, 2, base, name, token.offset, depth);
            } else if (name == "title") {
                parse_paragraph(ParaKind::Heading, heading_level(level), base, name, token.offset,
                                depth);
            } else if (name == "code") {
                parse_code(token.offset, base, depth);
            } else if (name == "table") {
                parse_table("table", depth + 1);
            } else if (too_deep) {
                // Flatten: children are handled by this loop, the stray end tag
                // of the ignored element is skipped.
                continue;
            } else if (name == "section") {
                parse_blocks(level + 1, base, quote, name, depth + 1);
            } else if (name == "epigraph" || name == "cite" || name == "annotation") {
                parse_blocks(level, base, true, name, depth + 1);
            } else if (name == "poem" || name == "stanza") {
                parse_blocks(level, base, quote, name, depth + 1);
            } else {
                // Unknown block wrapper: keep its text, stay tolerant.
                parse_blocks(level, base, quote, name, depth + 1);
            }
        }
    }

    void emit_stray_text(const Token& token, bool quote, Style base) {
        bool visible = false;
        for (char c : token.text) {
            if (!is_space(c)) {
                visible = true;
                break;
            }
        }
        if (!visible) return;
        InlineAccumulator accumulator;
        accumulator.add_text(base, token.text);
        accumulator.finalize();
        if (accumulator.empty()) return;
        Paragraph paragraph;
        paragraph.kind = quote ? ParaKind::Quote : ParaKind::Text;
        paragraph.token = token.offset;
        paragraph.runs = accumulator.take();
        _paragraphs.push_back(std::move(paragraph));
    }

    void parse_paragraph(ParaKind kind, uint8_t level, Style base, const std::string& end_tag,
                         size_t token_offset, int depth) {
        InlineAccumulator accumulator;
        parse_inline(accumulator, base, end_tag, depth);
        accumulator.finalize();
        if (accumulator.empty()) return;
        Paragraph paragraph;
        paragraph.kind = kind;
        paragraph.level = level;
        paragraph.token = token_offset;
        paragraph.runs = accumulator.take();
        paragraph.links = accumulator.take_links();
        _paragraphs.push_back(std::move(paragraph));
    }

    void parse_code(size_t token_offset, Style base, int depth) {
        InlineAccumulator accumulator;
        parse_inline(accumulator, mono_style(base), "code", depth);
        accumulator.finalize();
        if (accumulator.empty()) return;
        Paragraph paragraph;
        paragraph.kind = ParaKind::Code;
        paragraph.token = token_offset;
        paragraph.runs = accumulator.take();
        _paragraphs.push_back(std::move(paragraph));
    }

    void parse_inline(InlineAccumulator& accumulator, Style style, const std::string& end_tag,
                      int depth) {
        Token token;
        while (_scanner.next(token)) {
            if (token.type == TokenType::Eof) return;
            if (token.type == TokenType::EndTag) {
                if (token.name == end_tag) return;
                continue;
            }
            if (token.type == TokenType::Text) {
                accumulator.add_text(style, token.text);
                continue;
            }

            const std::string& name = token.name;
            if (name == "image") continue;
            if (name == "binary") {
                if (!token.self_closing) skip_element("binary");
                continue;
            }
            if (token.self_closing) {
                // A self-closing <empty-line/> inside a title separates its parts.
                if (name == "empty-line" && end_tag == "title") accumulator.add_text(style, " ");
                continue;
            }
            if (name == "empty-line") continue;

            // <a l:href="#note_1">[1]</a>: remember the byte range of the reference
            // and which note it points at.
            if (name == "a") {
                // Recorded even when the target is missing (note = 0): the UI can then
                // say "footnote text not found" instead of silently ignoring the marker.
                const uint32_t note = note_index_for(token.href);
                accumulator.begin_link();
                parse_inline(accumulator, style, "a", depth + 1);
                accumulator.end_link(note);
                continue;
            }

            Style nested = style;
            if (name == "emphasis") {
                nested = italic_style(style);
            } else if (name == "strong") {
                nested = bold_style(style);
            } else if (name == "code") {
                nested = mono_style(style);
            } else if (name == "p") {
                // <title><p>part 1</p><p>part 2</p></title>
                if (end_tag == "title") accumulator.add_text(style, " ");
            }
            // <strikethrough>, <a>, <sup>, <sub> and unknown wrappers keep the
            // inherited style and only contribute their text.
            if (depth >= kMaxInlineDepth) continue;
            parse_inline(accumulator, nested, name, depth + 1);
        }
    }

    void parse_table(const std::string& end_tag, int depth) {
        Token token;
        while (_scanner.next(token)) {
            if (token.type == TokenType::Eof) return;
            if (token.type == TokenType::EndTag) {
                if (token.name == end_tag) return;
                continue;
            }
            if (token.type != TokenType::StartTag) continue;
            if (token.name == "tr") {
                if (!token.self_closing) parse_table_row(token.offset, depth + 1);
                continue;
            }
            if (token.name == "td" || token.name == "th") {
                // Broken table without <tr>: a lone cell becomes its own row.
                if (!token.self_closing) parse_lone_cell(token, depth + 1);
                continue;
            }
            if (token.name == "image") continue;
            if (token.name == "binary") {
                if (!token.self_closing) skip_element("binary");
                continue;
            }
            if (token.self_closing) continue;
            if (depth >= kMaxBlockDepth) continue;
            parse_table(token.name, depth + 1); // <thead>, <tbody>, ...
        }
    }

    void parse_table_row(size_t row_token, int depth) {
        std::vector<Run> runs;
        bool have_cell = false;
        Token token;
        while (_scanner.next(token)) {
            if (token.type == TokenType::Eof) break;
            if (token.type == TokenType::EndTag) {
                if (token.name == "tr") break;
                continue;
            }
            if (token.type != TokenType::StartTag) continue;
            if (token.name == "image") continue;
            if (token.name == "binary") {
                if (!token.self_closing) skip_element("binary");
                continue;
            }
            if (token.self_closing) continue;

            InlineAccumulator cell;
            parse_inline(cell, Style{}, token.name, depth + 1);
            cell.finalize();
            if (cell.empty()) continue;
            if (have_cell) {
                Run separator;
                separator.style = Style{};
                separator.text = " | ";
                runs.push_back(std::move(separator));
            }
            for (Run& run : cell.take()) runs.push_back(std::move(run));
            have_cell = true;
        }
        if (runs.empty()) return;
        Paragraph paragraph;
        paragraph.kind = ParaKind::Text;
        paragraph.token = row_token;
        paragraph.runs = std::move(runs);
        _paragraphs.push_back(std::move(paragraph));
    }

    void parse_lone_cell(const Token& start, int depth) {
        InlineAccumulator cell;
        parse_inline(cell, Style{}, start.name, depth + 1);
        cell.finalize();
        if (cell.empty()) return;
        Paragraph paragraph;
        paragraph.kind = ParaKind::Text;
        paragraph.token = start.offset;
        paragraph.runs = cell.take();
        _paragraphs.push_back(std::move(paragraph));
    }

    Scanner _scanner;
    DocInfo _info;
    TextCollector _title;
    std::vector<std::string> _authors;
    std::vector<Paragraph> _paragraphs;
    std::vector<std::string> _notes;
    std::vector<std::string> _note_titles;
    TextCollector _note_title_pending;
    std::unordered_map<std::string, uint32_t> _note_ids;
};

class Fb2Document final : public Document {
  public:
    Fb2Document(std::string text, DocInfo info, std::vector<Paragraph> paragraphs)
        : _text(std::move(text)), _info(std::move(info)), _paragraphs(std::move(paragraphs)),
          _chars(utf8_length(_text)) {}

    const DocInfo& info() const override { return _info; }

    bool rewind() override {
        _index = 0;
        return true;
    }

    bool next(Paragraph& out) override {
        if (_index >= _paragraphs.size()) return false;
        out = _paragraphs[_index++];
        return true;
    }

    bool seek(uint64_t token) override {
        size_t lo = 0;
        size_t hi = _paragraphs.size();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (_paragraphs[mid].token < token) lo = mid + 1;
            else hi = mid;
        }
        _index = lo;
        return lo < _paragraphs.size() && _paragraphs[lo].token == token;
    }

    uint32_t permille(uint64_t token) const override {
        if (_text.empty()) return 0;
        const uint64_t value = token * 1000u / _text.size();
        return value > 1000u ? 1000u : static_cast<uint32_t>(value);
    }

    uint64_t total_chars() const override { return _chars; }

    void set_notes(std::vector<std::string> notes, std::vector<std::string> titles) {
        _notes = std::move(notes);
        _note_titles = std::move(titles);
    }

    std::string note_title(size_t index) const override {
        return index < _note_titles.size() ? _note_titles[index] : std::string();
    }

    size_t note_count() const override { return _notes.size(); }
    std::string note(size_t index) const override {
        return index < _notes.size() ? _notes[index] : std::string();
    }

  private:
    std::string _text;
    DocInfo _info;
    std::vector<Paragraph> _paragraphs;
    std::vector<std::string> _notes;
    std::vector<std::string> _note_titles;
    std::unordered_map<std::string, uint32_t> _note_ids;
    size_t _index = 0;
    uint64_t _chars = 0;
};

} // namespace

std::unique_ptr<Document> open_fb2(std::unique_ptr<ByteSource> source, std::string* error) {
    if (!source) {
        if (error) *error = "no data source";
        return nullptr;
    }
    if (source->size() == 0) {
        if (error) *error = "file is empty";
        return nullptr;
    }
    if (source->size() > kMaxSourceBytes) {
        if (error) *error = "file is too large";
        return nullptr;
    }

    std::string raw;
    if (!read_all(*source, kMaxSourceBytes, &raw, error)) return nullptr;
    if (raw.empty()) {
        if (error) *error = "file is empty";
        return nullptr;
    }

    const size_t probe = raw.size() < kEncodingProbeBytes ? raw.size() : kEncodingProbeBytes;
    const TextEncoding encoding =
        detect_encoding(reinterpret_cast<const uint8_t*>(raw.data()), probe);
    std::string text = to_utf8(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), encoding);
    if (text.size() > kMaxDecodedBytes) {
        if (error) *error = "file is too large";
        return nullptr;
    }

    const size_t bom = utf8_skip_bom(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    if (bom != 0) text.erase(0, bom);

    Fb2Parser parser(text);
    parser.parse();
    const DocInfo info = parser.info();
    std::vector<Paragraph> paragraphs = parser.take_paragraphs();
    std::vector<std::string> notes = parser.take_notes();
    std::vector<std::string> note_titles = parser.take_note_titles();
    if (paragraphs.empty() && info.title.empty() && info.author.empty()) {
        if (error) *error = "no FB2 text found";
        return nullptr;
    }

    auto document = std::unique_ptr<Fb2Document>(
        new Fb2Document(std::move(text), info, std::move(paragraphs)));
    document->set_notes(std::move(notes), std::move(note_titles));
    return std::unique_ptr<Document>(std::move(document));
}

} // namespace reader
