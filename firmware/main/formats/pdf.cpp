// PDF text-layer extraction: objects, xref (classic + streams), object streams,
// page tree, content-stream interpreter and font encodings. Pure C++17, no
// ESP-IDF dependencies; FlateDecode uses the bundled miniz.
#include "formats/formats.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/utf8.h"
#include "text/types.h"

// miniz v1.15 amalgamation (symbols are prefixed with lgfx_).
extern "C" size_t lgfx_tinfl_decompress_mem_to_mem(void* pOut, size_t out_len, const void* pSrc,
                                                   size_t src_len, int flags);

namespace reader {
namespace {

constexpr size_t kMaxPdfBytes = 12u * 1024u * 1024u;
constexpr size_t kMaxStreamBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxContentBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxPageText = 200u * 1024u;
constexpr size_t kMaxTotalText = 8u * 1024u * 1024u;
constexpr size_t kMaxPages = 2000;
constexpr size_t kMaxFragments = 200000;
constexpr int kTinflParseZlibHeader = 1; // TINFL_FLAG_PARSE_ZLIB_HEADER in miniz.c

void set_error(std::string* error, const char* message) {
    if (error != nullptr) *error = message;
}

bool is_space_char(char c) {
    switch (c) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
    case '\f':
    case '\0':
        return true;
    default:
        return false;
    }
}

bool is_delimiter(char c) {
    switch (c) {
    case '(':
    case ')':
    case '<':
    case '>':
    case '[':
    case ']':
    case '{':
    case '}':
    case '/':
    case '%':
    case '\'':
    case '"':
        return true;
    default:
        return is_space_char(c);
    }
}

bool is_digit_char(char c) { return c >= '0' && c <= '9'; }

bool is_number_start(char c) {
    return is_digit_char(c) || c == '+' || c == '-' || c == '.';
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ---------------------------------------------------------------------------
// PDF object model
// ---------------------------------------------------------------------------

struct Value {
    enum class Kind : uint8_t { Null, Bool, Num, Name, Str, Array, Dict, Ref };

    Kind kind = Kind::Null;
    bool boolean = false;
    double num = 0.0;
    std::string str; // name (without slash) or string bytes
    std::vector<Value> items;
    std::map<std::string, Value> dict;
    int ref = 0; // indirect reference object number

    // Streams keep a view into the file buffer instead of a copy.
    bool stream = false;
    size_t stream_off = 0;
    size_t stream_len = 0;

    bool is(Kind k) const { return kind == k; }
};

class Lexer {
  public:
    explicit Lexer(const std::string& source) : source_(source), pos_(0) {}
    Lexer(const std::string& source, size_t pos)
        : source_(source), pos_(pos > source.size() ? source.size() : pos) {}

    const std::string& source() const { return source_; }
    size_t pos() const { return pos_; }
    void set_pos(size_t pos) { pos_ = pos > source_.size() ? source_.size() : pos; }
    bool eof() const { return pos_ >= source_.size(); }
    char peek() const { return pos_ < source_.size() ? source_[pos_] : '\0'; }
    char peek_at(size_t delta) const {
        return pos_ + delta < source_.size() ? source_[pos_ + delta] : '\0';
    }

    void skip_space() {
        while (pos_ < source_.size()) {
            const char c = source_[pos_];
            if (is_space_char(c)) {
                ++pos_;
                continue;
            }
            if (c == '%') {
                while (pos_ < source_.size() && source_[pos_] != '\n' && source_[pos_] != '\r') {
                    ++pos_;
                }
                continue;
            }
            break;
        }
    }

    bool match_keyword(const char* keyword) {
        skip_space();
        const size_t length = std::strlen(keyword);
        if (pos_ + length > source_.size()) return false;
        if (source_.compare(pos_, length, keyword) != 0) return false;
        if (pos_ + length < source_.size() && !is_delimiter(source_[pos_ + length])) return false;
        pos_ += length;
        return true;
    }

    bool read_word(std::string* out) {
        skip_space();
        const size_t start = pos_;
        while (pos_ < source_.size() && !is_delimiter(source_[pos_])) ++pos_;
        if (pos_ == start) return false;
        out->assign(source_, start, pos_ - start);
        return true;
    }

    bool read_int(long long* out) {
        skip_space();
        const size_t start = pos_;
        bool negative = false;
        if (pos_ < source_.size() && (source_[pos_] == '+' || source_[pos_] == '-')) {
            negative = source_[pos_] == '-';
            ++pos_;
        }
        const size_t digits_start = pos_;
        unsigned long long value = 0;
        while (pos_ < source_.size() && is_digit_char(source_[pos_])) {
            const unsigned digit = static_cast<unsigned>(source_[pos_] - '0');
            if (value > 100000000000ULL) return false;
            value = value * 10 + digit;
            ++pos_;
        }
        if (pos_ == digits_start) {
            pos_ = start;
            return false;
        }
        const long long signed_value = static_cast<long long>(value);
        *out = negative ? -signed_value : signed_value;
        return true;
    }

    bool read_number(double* out) {
        skip_space();
        const size_t start = pos_;
        if (pos_ < source_.size() && (source_[pos_] == '+' || source_[pos_] == '-')) ++pos_;
        bool any_digit = false;
        while (pos_ < source_.size() && is_digit_char(source_[pos_])) {
            any_digit = true;
            ++pos_;
        }
        if (pos_ < source_.size() && source_[pos_] == '.') {
            ++pos_;
            while (pos_ < source_.size() && is_digit_char(source_[pos_])) {
                any_digit = true;
                ++pos_;
            }
        }
        if (!any_digit) {
            pos_ = start;
            return false;
        }
        const std::string text = source_.substr(start, pos_ - start);
        *out = std::strtod(text.c_str(), nullptr);
        return true;
    }

    bool parse_value(Value& out, int depth = 0) {
        if (depth > 64) return false;
        skip_space();
        if (eof()) return false;
        const char c = peek();

        if (c == '/') return parse_name(out);
        if (c == '(') return parse_literal_string(out);
        if (c == '<') {
            if (peek_at(1) == '<') return parse_dict(out, depth);
            return parse_hex_string(out);
        }
        if (c == '[') return parse_array(out, depth);
        if (c == ']' || c == '>' || c == ')' || c == '}' || c == '{') return false;
        if (is_number_start(c)) return parse_number_or_ref(out);

        std::string word;
        if (!read_word(&word)) return false;
        if (word == "true") {
            out = Value();
            out.kind = Value::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (word == "false") {
            out = Value();
            out.kind = Value::Kind::Bool;
            out.boolean = false;
            return true;
        }
        if (word == "null") {
            out = Value();
            return true;
        }
        return false;
    }

  private:
    bool parse_name(Value& out) {
        skip_space();
        if (eof() || peek() != '/') return false;
        ++pos_;
        std::string name;
        while (pos_ < source_.size() && !is_delimiter(source_[pos_])) {
            char c = source_[pos_];
            if (c == '#' && pos_ + 2 < source_.size()) {
                const int high = hex_digit(source_[pos_ + 1]);
                const int low = hex_digit(source_[pos_ + 2]);
                if (high >= 0 && low >= 0) {
                    name.push_back(static_cast<char>((high << 4) | low));
                    pos_ += 3;
                    continue;
                }
            }
            name.push_back(c);
            ++pos_;
        }
        out = Value();
        out.kind = Value::Kind::Name;
        out.str = std::move(name);
        return true;
    }

    bool parse_literal_string(Value& out) {
        if (eof() || peek() != '(') return false;
        ++pos_;
        std::string bytes;
        int nesting = 1;
        while (pos_ < source_.size()) {
            char c = source_[pos_++];
            if (c == '\\') {
                if (eof()) break;
                const char escaped = source_[pos_++];
                switch (escaped) {
                case 'n':
                    bytes.push_back('\n');
                    break;
                case 'r':
                    bytes.push_back('\r');
                    break;
                case 't':
                    bytes.push_back('\t');
                    break;
                case 'b':
                    bytes.push_back('\b');
                    break;
                case 'f':
                    bytes.push_back('\f');
                    break;
                case '(':
                case ')':
                case '\\':
                    bytes.push_back(escaped);
                    break;
                case '\r':
                    if (!eof() && peek() == '\n') ++pos_;
                    break;
                case '\n':
                    break;
                default:
                    if (escaped >= '0' && escaped <= '7') {
                        int value = escaped - '0';
                        for (int i = 0; i < 2 && !eof() && peek() >= '0' && peek() <= '7'; ++i) {
                            value = value * 8 + (source_[pos_++] - '0');
                        }
                        bytes.push_back(static_cast<char>(value & 0xFF));
                    } else {
                        bytes.push_back(escaped);
                    }
                    break;
                }
            } else if (c == '(') {
                ++nesting;
                bytes.push_back(c);
            } else if (c == ')') {
                if (--nesting == 0) break;
                bytes.push_back(c);
            } else if (c == '\r') {
                bytes.push_back('\n');
                if (!eof() && peek() == '\n') ++pos_;
            } else {
                bytes.push_back(c);
            }
        }
        out = Value();
        out.kind = Value::Kind::Str;
        out.str = std::move(bytes);
        return true;
    }

    bool parse_hex_string(Value& out) {
        if (eof() || peek() != '<') return false;
        ++pos_;
        std::string bytes;
        int high = -1;
        while (pos_ < source_.size() && source_[pos_] != '>') {
            const int digit = hex_digit(source_[pos_++]);
            if (digit < 0) continue;
            if (high < 0) {
                high = digit;
            } else {
                bytes.push_back(static_cast<char>((high << 4) | digit));
                high = -1;
            }
        }
        if (!eof() && peek() == '>') ++pos_;
        if (high >= 0) bytes.push_back(static_cast<char>(high << 4));
        out = Value();
        out.kind = Value::Kind::Str;
        out.str = std::move(bytes);
        return true;
    }

    bool parse_array(Value& out, int depth) {
        if (eof() || peek() != '[') return false;
        ++pos_;
        out = Value();
        out.kind = Value::Kind::Array;
        while (true) {
            skip_space();
            if (eof()) break;
            if (peek() == ']') {
                ++pos_;
                break;
            }
            Value item;
            if (parse_value(item, depth + 1)) {
                if (out.items.size() < 1000000) out.items.push_back(std::move(item));
            } else {
                ++pos_; // tolerate junk
            }
        }
        return true;
    }

    bool parse_dict(Value& out, int depth) {
        if (pos_ + 1 >= source_.size() || source_[pos_] != '<' || source_[pos_ + 1] != '<') {
            return false;
        }
        pos_ += 2;
        out = Value();
        out.kind = Value::Kind::Dict;
        while (true) {
            skip_space();
            if (eof()) break;
            if (peek() == '>') {
                if (peek_at(1) == '>') {
                    pos_ += 2;
                    break;
                }
                ++pos_;
                break;
            }
            if (peek() != '/') {
                ++pos_;
                continue;
            }
            Value key;
            if (!parse_name(key)) break;
            Value value;
            if (!parse_value(value, depth + 1)) break;
            out.dict[key.str] = std::move(value);
        }
        return true;
    }

    bool parse_number_or_ref(Value& out) {
        const double number = 0.0;
        (void)number;
        double value = 0.0;
        if (!read_number(&value)) return false;
        const size_t after_first = pos_;

        // Look ahead for "num gen R".
        if (value >= 0.0 && value <= static_cast<double>(INT_MAX) && std::floor(value) == value) {
            skip_space();
            long long generation = 0;
            if (read_int(&generation) && generation >= 0) {
                skip_space();
                if (!eof() && peek() == 'R' &&
                    (pos_ + 1 >= source_.size() || is_delimiter(source_[pos_ + 1]))) {
                    ++pos_;
                    out = Value();
                    out.kind = Value::Kind::Ref;
                    out.ref = static_cast<int>(value);
                    return true;
                }
            }
        }

        set_pos(after_first);
        out = Value();
        out.kind = Value::Kind::Num;
        out.num = value;
        return true;
    }

    const std::string& source_;
    size_t pos_;
};

bool value_number(const Value* value, double* out) {
    if (value == nullptr || value->kind != Value::Kind::Num) return false;
    *out = value->num;
    return true;
}

bool value_int(const Value* value, long long* out) {
    double number = 0.0;
    if (!value_number(value, &number)) return false;
    if (std::floor(number) != number) return false;
    *out = static_cast<long long>(number);
    return true;
}

// ---------------------------------------------------------------------------
// Encodings and glyph names
// ---------------------------------------------------------------------------

struct GlyphName {
    const char* name;
    uint32_t codepoint;
};

const GlyphName kGlyphNames[] = {
    {"space", 0x20},        {"exclam", 0x21},       {"quotedbl", 0x22},
    {"numbersign", 0x23},   {"dollar", 0x24},       {"percent", 0x25},
    {"ampersand", 0x26},    {"quotesingle", 0x27},  {"parenleft", 0x28},
    {"parenright", 0x29},   {"asterisk", 0x2A},     {"plus", 0x2B},
    {"comma", 0x2C},        {"hyphen", 0x2D},       {"period", 0x2E},
    {"slash", 0x2F},        {"zero", 0x30},         {"one", 0x31},
    {"two", 0x32},          {"three", 0x33},        {"four", 0x34},
    {"five", 0x35},         {"six", 0x36},          {"seven", 0x37},
    {"eight", 0x38},        {"nine", 0x39},         {"colon", 0x3A},
    {"semicolon", 0x3B},    {"less", 0x3C},         {"equal", 0x3D},
    {"greater", 0x3E},      {"question", 0x3F},     {"at", 0x40},
    {"bracketleft", 0x5B},  {"backslash", 0x5C},    {"bracketright", 0x5D},
    {"asciicircum", 0x5E},  {"underscore", 0x5F},   {"grave", 0x60},
    {"braceleft", 0x7B},    {"bar", 0x7C},          {"braceright", 0x7D},
    {"asciitilde", 0x7E},   {"exclamdown", 0xA1},   {"cent", 0xA2},
    {"sterling", 0xA3},     {"currency", 0xA4},     {"yen", 0xA5},
    {"brokenbar", 0xA6},    {"section", 0xA7},      {"dieresis", 0xA8},
    {"copyright", 0xA9},    {"ordfeminine", 0xAA},  {"guillemotleft", 0xAB},
    {"logicalnot", 0xAC},   {"registered", 0xAE},   {"macron", 0xAF},
    {"degree", 0xB0},       {"plusminus", 0xB1},    {"twosuperior", 0xB2},
    {"threesuperior", 0xB3}, {"acute", 0xB4},       {"mu", 0xB5},
    {"paragraph", 0xB6},    {"periodcentered", 0xB7}, {"cedilla", 0xB8},
    {"onesuperior", 0xB9},  {"ordmasculine", 0xBA}, {"guillemotright", 0xBB},
    {"onequarter", 0xBC},   {"onehalf", 0xBD},      {"threequarters", 0xBE},
    {"questiondown", 0xBF}, {"Agrave", 0xC0},       {"Aacute", 0xC1},
    {"Acircumflex", 0xC2},  {"Atilde", 0xC3},       {"Adieresis", 0xC4},
    {"Aring", 0xC5},        {"AE", 0xC6},           {"Ccedilla", 0xC7},
    {"Egrave", 0xC8},       {"Eacute", 0xC9},       {"Ecircumflex", 0xCA},
    {"Edieresis", 0xCB},    {"Igrave", 0xCC},       {"Iacute", 0xCD},
    {"Icircumflex", 0xCE},  {"Idieresis", 0xCF},    {"Eth", 0xD0},
    {"Ntilde", 0xD1},       {"Ograve", 0xD2},       {"Oacute", 0xD3},
    {"Ocircumflex", 0xD4},  {"Otilde", 0xD5},       {"Odieresis", 0xD6},
    {"multiply", 0xD7},     {"Oslash", 0xD8},       {"Ugrave", 0xD9},
    {"Uacute", 0xDA},       {"Ucircumflex", 0xDB},  {"Udieresis", 0xDC},
    {"Yacute", 0xDD},       {"Thorn", 0xDE},        {"germandbls", 0xDF},
    {"agrave", 0xE0},       {"aacute", 0xE1},       {"acircumflex", 0xE2},
    {"atilde", 0xE3},       {"adieresis", 0xE4},    {"aring", 0xE5},
    {"ae", 0xE6},           {"ccedilla", 0xE7},     {"egrave", 0xE8},
    {"eacute", 0xE9},       {"ecircumflex", 0xEA},  {"edieresis", 0xEB},
    {"igrave", 0xEC},       {"iacute", 0xED},       {"icircumflex", 0xEE},
    {"idieresis", 0xEF},    {"eth", 0xF0},          {"ntilde", 0xF1},
    {"ograve", 0xF2},       {"oacute", 0xF3},       {"ocircumflex", 0xF4},
    {"otilde", 0xF5},       {"odieresis", 0xF6},    {"divide", 0xF7},
    {"oslash", 0xF8},       {"ugrave", 0xF9},       {"uacute", 0xFA},
    {"ucircumflex", 0xFB},  {"udieresis", 0xFC},    {"yacute", 0xFD},
    {"thorn", 0xFE},        {"ydieresis", 0xFF},    {"OE", 0x152},
    {"oe", 0x153},          {"Lslash", 0x141},      {"lslash", 0x142},
    {"dotlessi", 0x131},    {"dotlessj", 0x237},    {"Scaron", 0x160},
    {"scaron", 0x161},      {"Zcaron", 0x17D},      {"zcaron", 0x17E},
    {"Ccaron", 0x10C},      {"ccaron", 0x10D},      {"Ecaron", 0x11A},
    {"ecaron", 0x11B},      {"Rcaron", 0x158},      {"rcaron", 0x159},
    {"Uring", 0x16E},       {"uring", 0x16F},       {"Aogonek", 0x104},
    {"aogonek", 0x105},     {"Eogonek", 0x118},     {"eogonek", 0x119},
    {"quoteleft", 0x2018},  {"quoteright", 0x2019}, {"quotesinglbase", 0x201A},
    {"quotedblleft", 0x201C}, {"quotedblright", 0x201D}, {"quotedblbase", 0x201E},
    {"guilsinglleft", 0x2039}, {"guilsinglright", 0x203A}, {"endash", 0x2013},
    {"emdash", 0x2014},     {"bullet", 0x2022},     {"ellipsis", 0x2026},
    {"dagger", 0x2020},     {"daggerdbl", 0x2021},  {"perthousand", 0x2030},
    {"fraction", 0x2044},   {"florin", 0x192},      {"Euro", 0x20AC},
    {"euro", 0x20AC},       {"trademark", 0x2122},  {"circumflex", 0x2C6},
    {"tilde", 0x2DC},       {"breve", 0x2D8},       {"dotaccent", 0x2D9},
    {"ring", 0x2DA},        {"ogonek", 0x2DB},      {"caron", 0x2C7},
    {"hungarumlaut", 0x2DD}, {"minus", 0x2212},     {"fi", 0xFB01},
    {"fl", 0xFB02},         {"ff", 0xFB00},         {"ffi", 0xFB03},
    {"ffl", 0xFB04},        {"Omega", 0x3A9},       {"Delta", 0x394},
    {"pi", 0x3C0},          {"summation", 0x2211},  {"product", 0x220F},
    {"radical", 0x221A},    {"infinity", 0x221E},   {"integral", 0x222B},
    {"approxequal", 0x2248}, {"notequal", 0x2260},  {"lessequal", 0x2264},
    {"greaterequal", 0x2265}, {"lozenge", 0x25CA},  {"nbspace", 0xA0},
    {"commaaccent", 0x326}, {"Abreve", 0x102},      {"abreve", 0x103},
    {"Scedilla", 0x15E},    {"scedilla", 0x15F},    {"Nacute", 0x143},
    {"nacute", 0x144},      {"Sacute", 0x15A},      {"sacute", 0x15B},
    {"Zacute", 0x179},      {"zacute", 0x17A},      {"Zdotaccent", 0x17B},
    {"zdotaccent", 0x17C},  {"Gbreve", 0x11E},      {"gbreve", 0x11F},
    {"Idotaccent", 0x130},  {"Cacute", 0x106},      {"cacute", 0x107},
    {"Ncaron", 0x147},      {"ncaron", 0x148},      {"Dcaron", 0x10E},
    {"dcaron", 0x10F},      {"Dcroat", 0x110},      {"dcroat", 0x111},
    // Adobe historical glyph names for Cyrillic (afii10017..afii10088).
    {"afii10017", 0x410},   {"afii10018", 0x411},   {"afii10019", 0x412},
    {"afii10020", 0x413},   {"afii10021", 0x414},   {"afii10022", 0x415},
    {"afii10023", 0x416},   {"afii10024", 0x417},   {"afii10025", 0x418},
    {"afii10026", 0x419},   {"afii10027", 0x41A},   {"afii10028", 0x41B},
    {"afii10029", 0x41C},   {"afii10030", 0x41D},   {"afii10031", 0x41E},
    {"afii10032", 0x41F},   {"afii10033", 0x420},   {"afii10034", 0x421},
    {"afii10035", 0x422},   {"afii10036", 0x423},   {"afii10037", 0x424},
    {"afii10038", 0x425},   {"afii10039", 0x426},   {"afii10040", 0x427},
    {"afii10041", 0x428},   {"afii10042", 0x429},   {"afii10043", 0x42A},
    {"afii10044", 0x42B},   {"afii10045", 0x42C},   {"afii10046", 0x42D},
    {"afii10047", 0x42E},   {"afii10048", 0x42F},   {"afii10049", 0x430},
    {"afii10050", 0x431},   {"afii10051", 0x432},   {"afii10052", 0x433},
    {"afii10053", 0x434},   {"afii10054", 0x435},   {"afii10055", 0x436},
    {"afii10056", 0x437},   {"afii10057", 0x438},   {"afii10058", 0x439},
    {"afii10059", 0x43A},   {"afii10060", 0x43B},   {"afii10061", 0x43C},
    {"afii10062", 0x43D},   {"afii10063", 0x43E},   {"afii10064", 0x43F},
    {"afii10065", 0x440},   {"afii10066", 0x441},   {"afii10067", 0x442},
    {"afii10068", 0x443},   {"afii10069", 0x444},   {"afii10070", 0x445},
    {"afii10071", 0x446},   {"afii10072", 0x447},   {"afii10073", 0x448},
    {"afii10074", 0x449},   {"afii10075", 0x44A},   {"afii10076", 0x44B},
    {"afii10077", 0x44C},   {"afii10078", 0x44D},   {"afii10079", 0x44E},
    {"afii10080", 0x44F},   {"afii10081", 0x401},   {"afii10082", 0x451},
    {"afii10083", 0x404},   {"afii10084", 0x454},   {"afii10085", 0x407},
    {"afii10086", 0x457},   {"afii10087", 0x490},   {"afii10088", 0x491},
    {"afii10089", 0x2116},
};

uint32_t glyph_to_unicode(const std::string& name) {
    if (name.empty()) return 0;
    if (name.rfind("uni", 0) == 0 && name.size() >= 7) {
        uint32_t value = 0;
        bool valid = true;
        for (size_t i = 3; i < name.size() && i < 7; ++i) {
            const int digit = hex_digit(name[i]);
            if (digit < 0) {
                valid = false;
                break;
            }
            value = (value << 4) | static_cast<uint32_t>(digit);
        }
        if (valid) return value;
    }
    if (name[0] == 'u' && name.size() >= 5 && name.size() <= 7) {
        uint32_t value = 0;
        bool valid = true;
        for (size_t i = 1; i < name.size(); ++i) {
            const int digit = hex_digit(name[i]);
            if (digit < 0) {
                valid = false;
                break;
            }
            value = (value << 4) | static_cast<uint32_t>(digit);
        }
        if (valid) return value;
    }
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') return static_cast<uint32_t>(name[0]);
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') return static_cast<uint32_t>(name[0]);
    for (const GlyphName& glyph : kGlyphNames) {
        if (name == glyph.name) return glyph.codepoint;
    }
    return 0;
}

uint32_t win_ansi_codepoint(int code) {
    switch (code) {
    case 0x80:
        return 0x20AC;
    case 0x81:
        return 0;
    case 0x82:
        return 0x201A;
    case 0x83:
        return 0x0192;
    case 0x84:
        return 0x201E;
    case 0x85:
        return 0x2026;
    case 0x86:
        return 0x2020;
    case 0x87:
        return 0x2021;
    case 0x88:
        return 0x02C6;
    case 0x89:
        return 0x2030;
    case 0x8A:
        return 0x0160;
    case 0x8B:
        return 0x2039;
    case 0x8C:
        return 0x0152;
    case 0x8D:
        return 0;
    case 0x8E:
        return 0x017D;
    case 0x8F:
        return 0;
    case 0x90:
        return 0;
    case 0x91:
        return 0x2018;
    case 0x92:
        return 0x2019;
    case 0x93:
        return 0x201C;
    case 0x94:
        return 0x201D;
    case 0x95:
        return 0x2022;
    case 0x96:
        return 0x2013;
    case 0x97:
        return 0x2014;
    case 0x98:
        return 0x02DC;
    case 0x99:
        return 0x2122;
    case 0x9A:
        return 0x0161;
    case 0x9B:
        return 0x203A;
    case 0x9C:
        return 0x0153;
    case 0x9D:
        return 0;
    case 0x9E:
        return 0x017E;
    case 0x9F:
        return 0x0178;
    default:
        return code >= 0x20 && code <= 0xFF ? static_cast<uint32_t>(code) : 0;
    }
}

const uint32_t kMacRomanHigh[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4,
    0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF,
    0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020,
    0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4,
    0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202,
    0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1,
    0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3,
    0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A,
    0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC,
    0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF,
    0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

uint32_t standard_codepoint(int code) {
    switch (code) {
    case 0xA1:
        return 0xA1;
    case 0xA2:
        return 0xA2;
    case 0xA3:
        return 0xA3;
    case 0xA4:
        return 0x2044;
    case 0xA5:
        return 0xA5;
    case 0xA6:
        return 0x192;
    case 0xA7:
        return 0xA7;
    case 0xA8:
        return 0xA4;
    case 0xA9:
        return 0x27;
    case 0xAA:
        return 0x201C;
    case 0xAB:
        return 0xAB;
    case 0xAC:
        return 0x2039;
    case 0xAD:
        return 0x203A;
    case 0xAE:
        return 0xFB01;
    case 0xAF:
        return 0xFB02;
    case 0xB1:
        return 0x2013;
    case 0xB2:
        return 0x2020;
    case 0xB3:
        return 0x2021;
    case 0xB4:
        return 0xB7;
    case 0xB6:
        return 0xB6;
    case 0xB7:
        return 0x2022;
    case 0xB8:
        return 0x201A;
    case 0xB9:
        return 0x201E;
    case 0xBA:
        return 0x201D;
    case 0xBB:
        return 0xBB;
    case 0xBC:
        return 0x2026;
    case 0xBD:
        return 0x2030;
    case 0xBF:
        return 0xBF;
    case 0xC1:
        return 0x60;
    case 0xC2:
        return 0xB4;
    case 0xC3:
        return 0x2C6;
    case 0xC4:
        return 0x2DC;
    case 0xC5:
        return 0xAF;
    case 0xC6:
        return 0x2D8;
    case 0xC7:
        return 0x2D9;
    case 0xC8:
        return 0xA8;
    case 0xCA:
        return 0x2DA;
    case 0xCB:
        return 0xB8;
    case 0xCD:
        return 0x2DD;
    case 0xCE:
        return 0x2DB;
    case 0xCF:
        return 0x2C7;
    case 0xD0:
        return 0x2014;
    case 0xE1:
        return 0xC6;
    case 0xE3:
        return 0xAA;
    case 0xE8:
        return 0x141;
    case 0xE9:
        return 0xD8;
    case 0xEA:
        return 0x152;
    case 0xEB:
        return 0xBA;
    case 0xF1:
        return 0xE6;
    case 0xF5:
        return 0x131;
    case 0xF8:
        return 0x142;
    case 0xF9:
        return 0xF8;
    case 0xFA:
        return 0x153;
    case 0xFB:
        return 0xDF;
    default:
        return 0;
    }
}

void fill_encoding(uint32_t map[256], const std::string& name) {
    for (int i = 0; i < 256; ++i) map[i] = 0;
    for (int i = 0x20; i <= 0x7E; ++i) map[i] = static_cast<uint32_t>(i);
    for (int i = 0xA0; i <= 0xFF; ++i) map[i] = static_cast<uint32_t>(i);

    if (name == "MacRomanEncoding") {
        for (int i = 0x80; i <= 0xFF; ++i) map[i] = kMacRomanHigh[i - 0x80];
    } else if (name == "StandardEncoding") {
        for (int i = 0xA1; i <= 0xFF; ++i) map[i] = standard_codepoint(i);
        map[0x27] = 0x2019;
        map[0x60] = 0x2018;
    } else { // WinAnsiEncoding default
        for (int i = 0x80; i <= 0x9F; ++i) map[i] = win_ansi_codepoint(i);
    }
    for (int i = 0; i < 0x20; ++i) map[i] = 0;
    map[0x7F] = 0;
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

std::string utf16_bytes_to_utf8(const std::string& bytes, bool little_endian) {
    std::string out;
    const size_t count = bytes.size() / 2;
    for (size_t i = 0; i < count; ++i) {
        const auto* p = reinterpret_cast<const unsigned char*>(bytes.data()) + i * 2;
        uint32_t unit = little_endian ? (static_cast<uint32_t>(p[1]) << 8) | p[0]
                                      : (static_cast<uint32_t>(p[0]) << 8) | p[1];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < count) {
            const auto* q = reinterpret_cast<const unsigned char*>(bytes.data()) + (i + 1) * 2;
            const uint32_t low = little_endian ? (static_cast<uint32_t>(q[1]) << 8) | q[0]
                                               : (static_cast<uint32_t>(q[0]) << 8) | q[1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            } else {
                unit = kReplacementChar;
            }
        } else if (unit >= 0xD800 && unit <= 0xDFFF) {
            unit = kReplacementChar;
        }
        utf8_append(out, unit);
    }
    return out;
}

std::string pdf_string_to_utf8(const std::string& raw) {
    if (raw.size() >= 2) {
        const auto first = static_cast<unsigned char>(raw[0]);
        const auto second = static_cast<unsigned char>(raw[1]);
        if (first == 0xFE && second == 0xFF) {
            return utf16_bytes_to_utf8(raw.substr(2), false);
        }
        if (first == 0xFF && second == 0xFE) {
            return utf16_bytes_to_utf8(raw.substr(2), true);
        }
    }
    std::string out;
    for (char c : raw) {
        const unsigned char byte = static_cast<unsigned char>(c);
        uint32_t codepoint = byte;
        if (byte >= 0x80 && byte <= 0x9F) codepoint = win_ansi_codepoint(byte);
        if (codepoint != 0) utf8_append(out, codepoint);
    }
    return out;
}

bool codepoint_is_letter(uint32_t cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return true;
    if (cp >= 0x00C0 && cp <= 0x024F) return true; // Latin supplements
    if (cp >= 0x0370 && cp <= 0x03FF) return true; // Greek
    if (cp >= 0x0400 && cp <= 0x04FF) return true; // Cyrillic
    return false;
}

bool codepoint_is_lowercase(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 0x00DF && cp <= 0x00FF) return true;
    if (cp >= 0x0100 && cp <= 0x017F && (cp & 1) == 1) return true; // Latin Ext-A
    if (cp >= 0x0430 && cp <= 0x044F) return true;
    if (cp >= 0x0450 && cp <= 0x045F) return true;
    if (cp >= 0x03B1 && cp <= 0x03C9) return true;
    return false;
}

uint32_t utf8_first_codepoint(const std::string& text) {
    if (text.empty()) return 0;
    size_t used = 0;
    return utf8_decode(text.data(), text.size(), &used);
}

uint32_t utf8_last_codepoint(const std::string& text, size_t* start_out = nullptr) {
    if (text.empty()) return 0;
    size_t pos = 0;
    uint32_t last = 0;
    size_t last_start = 0;
    while (pos < text.size()) {
        size_t used = 0;
        const uint32_t cp = utf8_decode(text.data() + pos, text.size() - pos, &used);
        last = cp;
        last_start = pos;
        pos += used == 0 ? 1 : used;
    }
    if (start_out != nullptr) *start_out = last_start;
    return last;
}

size_t paragraph_byte_length(const Paragraph& paragraph) {
    size_t total = 0;
    for (const Run& run : paragraph.runs) total += run.text.size();
    return total;
}

// ---------------------------------------------------------------------------
// Fonts and CMaps
// ---------------------------------------------------------------------------

struct FontInfo {
    bool type0 = false;
    bool unsupported = false;
    int code_bytes = 1;
    std::map<uint32_t, std::string> to_unicode;
    uint32_t simple_map[256];

    FontInfo() {
        for (int i = 0; i < 256; ++i) simple_map[i] = 0;
    }
};

std::string decode_font_text(const FontInfo& font, const std::string& raw) {
    std::string out;
    if (font.type0) {
        int code_bytes = font.code_bytes;
        if (code_bytes < 1) code_bytes = 1;
        if (code_bytes > 4) code_bytes = 4;
        for (size_t i = 0; i + static_cast<size_t>(code_bytes) <= raw.size();
             i += static_cast<size_t>(code_bytes)) {
            uint32_t code = 0;
            for (int k = 0; k < code_bytes; ++k) {
                code = (code << 8) | static_cast<unsigned char>(raw[i + static_cast<size_t>(k)]);
            }
            const auto it = font.to_unicode.find(code);
            if (it != font.to_unicode.end()) out += it->second;
        }
        return out;
    }
    for (char c : raw) {
        const auto byte = static_cast<unsigned char>(c);
        const auto it = font.to_unicode.find(byte);
        if (it != font.to_unicode.end()) {
            out += it->second;
        } else if (font.simple_map[byte] != 0) {
            utf8_append(out, font.simple_map[byte]);
        }
    }
    return out;
}

uint32_t cmap_bytes_code(const std::string& bytes) {
    uint32_t code = 0;
    const size_t take = bytes.size() > 4 ? 4 : bytes.size();
    for (size_t i = 0; i < take; ++i) {
        code = (code << 8) | static_cast<unsigned char>(bytes[i]);
    }
    return code;
}

std::string cmap_hex_to_utf8(const std::string& bytes) {
    if (bytes.empty()) return std::string();
    if (bytes.size() % 2 == 0) return utf16_bytes_to_utf8(bytes, false);
    std::string out;
    for (char c : bytes) utf8_append(out, static_cast<unsigned char>(c));
    return out;
}

struct CmapToken {
    bool is_hex = false;
    std::string bytes;
    std::string word;
};

std::vector<CmapToken> tokenize_cmap(const std::string& text) {
    std::vector<CmapToken> tokens;
    size_t pos = 0;
    while (pos < text.size() && tokens.size() < 1000000) {
        const char c = text[pos];
        if (is_space_char(c)) {
            ++pos;
            continue;
        }
        if (c == '%') {
            while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r') ++pos;
            continue;
        }
        if (c == '<') {
            if (pos + 1 < text.size() && text[pos + 1] == '<') {
                pos += 2;
                CmapToken token;
                token.word = "<<";
                tokens.push_back(std::move(token));
                continue;
            }
            ++pos;
            CmapToken token;
            token.is_hex = true;
            int high = -1;
            while (pos < text.size() && text[pos] != '>') {
                const int digit = hex_digit(text[pos++]);
                if (digit < 0) continue;
                if (high < 0) {
                    high = digit;
                } else {
                    token.bytes.push_back(static_cast<char>((high << 4) | digit));
                    high = -1;
                }
            }
            if (pos < text.size() && text[pos] == '>') ++pos;
            if (high >= 0) token.bytes.push_back(static_cast<char>(high << 4));
            tokens.push_back(std::move(token));
            continue;
        }
        if (c == '(') {
            ++pos;
            int depth = 1;
            while (pos < text.size() && depth > 0) {
                const char inner = text[pos++];
                if (inner == '\\' && pos < text.size()) {
                    ++pos;
                } else if (inner == '(') {
                    ++depth;
                } else if (inner == ')') {
                    --depth;
                }
            }
            CmapToken token;
            token.word = "()";
            tokens.push_back(std::move(token));
            continue;
        }
        if (c == '[' || c == ']' || c == '{' || c == '}') {
            ++pos;
            CmapToken token;
            token.word = std::string(1, c);
            tokens.push_back(std::move(token));
            continue;
        }
        const size_t start = pos;
        while (pos < text.size() && !is_delimiter(text[pos])) ++pos;
        if (pos == start) {
            ++pos;
            continue;
        }
        CmapToken token;
        token.word.assign(text, start, pos - start);
        tokens.push_back(std::move(token));
    }
    return tokens;
}

void append_bfrange_destination(std::map<uint32_t, std::string>* out, uint32_t code,
                                const std::string& destination, uint32_t offset) {
    if (out->size() >= 65536) return;
    std::string mapped;
    if (destination.size() % 2 == 0 && !destination.empty()) {
        std::string adjusted = destination;
        // Increment the final UTF-16 code unit.
        const size_t last = adjusted.size() - 2;
        unsigned value = (static_cast<unsigned char>(adjusted[last]) << 8) |
                         static_cast<unsigned char>(adjusted[last + 1]);
        value = (value + offset) & 0xFFFF;
        adjusted[last] = static_cast<char>(value >> 8);
        adjusted[last + 1] = static_cast<char>(value & 0xFF);
        mapped = utf16_bytes_to_utf8(adjusted, false);
    } else {
        std::string adjusted = destination;
        if (!adjusted.empty()) {
            unsigned value = (static_cast<unsigned char>(adjusted.back()) + offset) & 0xFF;
            adjusted.back() = static_cast<char>(value);
        }
        for (char c : adjusted) utf8_append(mapped, static_cast<unsigned char>(c));
    }
    (*out)[code] = std::move(mapped);
}

void parse_cmap(const std::string& data, std::map<uint32_t, std::string>* out, int* code_bytes) {
    const std::vector<CmapToken> tokens = tokenize_cmap(data);
    size_t i = 0;
    while (i < tokens.size()) {
        const CmapToken& token = tokens[i];
        if (token.word == "beginbfchar") {
            ++i;
            while (i + 1 < tokens.size() && tokens[i].word != "endbfchar") {
                if (!tokens[i].is_hex || !tokens[i + 1].is_hex) {
                    ++i;
                    continue;
                }
                const size_t bytes = tokens[i].bytes.size();
                if (bytes > 0 && static_cast<int>(bytes > 4 ? 4 : bytes) > *code_bytes) {
                    *code_bytes = static_cast<int>(bytes > 4 ? 4 : bytes);
                }
                if (out->size() < 65536) {
                    (*out)[cmap_bytes_code(tokens[i].bytes)] =
                        cmap_hex_to_utf8(tokens[i + 1].bytes);
                }
                i += 2;
            }
            ++i;
            continue;
        }
        if (token.word == "beginbfrange") {
            ++i;
            while (i + 1 < tokens.size() && tokens[i].word != "endbfrange") {
                if (!tokens[i].is_hex || !tokens[i + 1].is_hex) {
                    ++i;
                    continue;
                }
                const size_t bytes = tokens[i].bytes.size();
                if (bytes > 0 && static_cast<int>(bytes > 4 ? 4 : bytes) > *code_bytes) {
                    *code_bytes = static_cast<int>(bytes > 4 ? 4 : bytes);
                }
                const uint32_t low = cmap_bytes_code(tokens[i].bytes);
                const uint32_t high = cmap_bytes_code(tokens[i + 1].bytes);
                i += 2;
                if (i < tokens.size() && tokens[i].is_hex) {
                    const std::string destination = tokens[i].bytes;
                    ++i;
                    if (high >= low && high - low <= 65535) {
                        for (uint32_t code = low; code <= high; ++code) {
                            append_bfrange_destination(out, code, destination, code - low);
                            if (out->size() >= 65536) break;
                        }
                    }
                } else if (i < tokens.size() && tokens[i].word == "[") {
                    ++i;
                    uint32_t code = low;
                    while (i < tokens.size() && tokens[i].word != "]") {
                        if (tokens[i].is_hex && out->size() < 65536) {
                            (*out)[code] = cmap_hex_to_utf8(tokens[i].bytes);
                            ++code;
                        }
                        ++i;
                    }
                    if (i < tokens.size() && tokens[i].word == "]") ++i;
                }
            }
            ++i;
            continue;
        }
        if (token.word == "begincodespacerange") {
            ++i;
            while (i < tokens.size() && tokens[i].word != "endcodespacerange") ++i;
            ++i;
            continue;
        }
        ++i;
    }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

struct Mat {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;
};

Mat mat_mul(const Mat& m1, const Mat& m2) {
    Mat out;
    out.a = m1.a * m2.a + m1.b * m2.c;
    out.b = m1.a * m2.b + m1.b * m2.d;
    out.c = m1.c * m2.a + m1.d * m2.c;
    out.d = m1.c * m2.b + m1.d * m2.d;
    out.e = m1.e * m2.a + m1.f * m2.c + m2.e;
    out.f = m1.e * m2.b + m1.f * m2.d + m2.f;
    return out;
}

Mat mat_translate(double tx, double ty) {
    Mat out;
    out.e = tx;
    out.f = ty;
    return out;
}

struct TextFrag {
    double x = 0.0;
    double y = 0.0;
    double size = 12.0;
    double width = 0.0;
    std::string text;
};

struct PageNode {
    const Value* dict = nullptr;
    const Value* resources = nullptr;
    const Value* media_box = nullptr;
};

class PdfDocument;

// ---------------------------------------------------------------------------
// PDF parser
// ---------------------------------------------------------------------------

class PdfParser {
  public:
    explicit PdfParser(std::string data) : data_(std::move(data)) {}

    bool load(std::string* error);
    std::unique_ptr<Document> build(std::string* error);

    const std::string& data() const { return data_; }

    const Value* resolve(const Value* value, int depth = 0) const {
        while (value != nullptr && value->kind == Value::Kind::Ref && depth < 16) {
            const auto it = objects_.find(value->ref);
            if (it == objects_.end()) return nullptr;
            value = &it->second;
            ++depth;
        }
        if (value != nullptr && value->kind == Value::Kind::Ref) return nullptr;
        return value;
    }

    const Value* get(const Value* dict, const std::string& key) const {
        dict = resolve(dict);
        if (dict == nullptr || dict->kind != Value::Kind::Dict) return nullptr;
        const auto it = dict->dict.find(key);
        if (it == dict->dict.end()) return nullptr;
        return resolve(&it->second);
    }

    const Value* get_raw(const Value* dict, const std::string& key) const {
        dict = resolve(dict);
        if (dict == nullptr || dict->kind != Value::Kind::Dict) return nullptr;
        const auto it = dict->dict.find(key);
        return it == dict->dict.end() ? nullptr : &it->second;
    }

    std::string type_name(const Value* dict) const {
        const Value* type = get(dict, "Type");
        return type != nullptr && type->kind == Value::Kind::Name ? type->str : std::string();
    }

    bool decode_stream_bytes(const Value* stream, std::string* out) const;
    FontInfo build_font(const Value* font_dict) const;

    std::vector<Paragraph> extract_page(const PageNode& page, size_t page_index,
                                        size_t& total_bytes);

  private:
    struct XrefEntry {
        int type = 0;
        size_t offset = 0;
        int stream = 0;
        int index = 0;
    };

    bool parse_object_at(size_t pos, int* num, int* gen, Value* out, size_t* after) const;
    bool locate_object_header(size_t obj_keyword_pos, size_t* start, int* num, int* gen) const;
    void scan_objects();
    void parse_xref_chain();
    void follow_xref(size_t offset, int depth, std::set<size_t>& visited);
    void parse_xref_table(size_t offset, int depth, std::set<size_t>& visited);
    void process_xref_stream(const Value& stream, int depth, std::set<size_t>& visited);
    void merge_trailer(const Value* trailer);
    void find_trailers();
    void load_xref_objects();
    void load_object_streams();
    void fix_stream_lengths();
    const Value* find_catalog() const;
    void fill_info();
    void walk_pages(const Value& node, const Value* inherited_resources,
                    const Value* inherited_media_box, int depth);
    void collect_pages_fallback();
    std::string gather_contents(const PageNode& page) const;
    bool inflate_bytes(const std::string& input, std::string* out) const;

    std::string data_;
    std::map<int, Value> objects_;
    std::map<int, XrefEntry> xref_;
    std::map<std::string, Value> trailer_;
    std::vector<PageNode> pages_;
    std::set<int> visited_page_nodes_;
    bool encrypted_ = false;
    DocInfo info_;
};

// ---------------------------------------------------------------------------
// Low-level object parsing
// ---------------------------------------------------------------------------

bool PdfParser::locate_object_header(size_t obj_keyword_pos, size_t* start, int* num, int* gen) const {
    size_t pos = obj_keyword_pos;
    while (pos > 0 && is_space_char(data_[pos - 1])) --pos;

    const size_t gen_end = pos;
    while (pos > 0 && is_digit_char(data_[pos - 1])) --pos;
    if (pos == gen_end) return false;
    const size_t gen_start = pos;

    while (pos > 0 && is_space_char(data_[pos - 1])) --pos;
    const size_t num_end = pos;
    while (pos > 0 && is_digit_char(data_[pos - 1])) --pos;
    if (pos == num_end) return false;
    const size_t num_start = pos;
    if (num_start > 0 && !is_delimiter(data_[num_start - 1])) return false;

    const std::string num_text = data_.substr(num_start, num_end - num_start);
    const std::string gen_text = data_.substr(gen_start, gen_end - gen_start);
    if (num_text.empty() || num_text.size() > 9 || gen_text.size() > 5) return false;
    const long number = std::strtol(num_text.c_str(), nullptr, 10);
    const long generation = std::strtol(gen_text.c_str(), nullptr, 10);
    if (number < 0 || number > INT_MAX || generation < 0) return false;

    *start = num_start;
    *num = static_cast<int>(number);
    *gen = static_cast<int>(generation);
    return true;
}

bool PdfParser::parse_object_at(size_t pos, int* num_out, int* gen_out, Value* out,
                                size_t* after_out) const {
    if (pos >= data_.size()) return false;
    Lexer lex(data_, pos);
    long long number = 0;
    long long generation = 0;
    if (!lex.read_int(&number) || !lex.read_int(&generation)) return false;
    if (!lex.match_keyword("obj")) return false;
    if (!lex.parse_value(*out, 0)) return false;

    size_t after = lex.pos();
    if (out->kind == Value::Kind::Dict) {
        const size_t save = after;
        lex.skip_space();
        if (lex.match_keyword("stream")) {
            size_t stream_start = lex.pos();
            if (stream_start < data_.size() && data_[stream_start] == '\r') {
                ++stream_start;
                if (stream_start < data_.size() && data_[stream_start] == '\n') ++stream_start;
            } else if (stream_start < data_.size() && data_[stream_start] == '\n') {
                ++stream_start;
            }

            size_t length = 0;
            bool has_length = false;
            if (const Value* length_value = get_raw(out, "Length")) {
                const Value* resolved = resolve(length_value);
                double numeric = 0.0;
                if (value_number(resolved, &numeric) && numeric >= 0.0 &&
                    numeric <= 100000000.0) {
                    length = static_cast<size_t>(numeric);
                    has_length = true;
                }
            }

            size_t stream_end = 0;
            bool found = false;
            if (has_length && stream_start <= data_.size() &&
                length <= data_.size() - stream_start) {
                size_t check = stream_start + length;
                while (check < data_.size() && is_space_char(data_[check])) ++check;
                if (check + 9 <= data_.size() && data_.compare(check, 9, "endstream") == 0) {
                    stream_end = check;
                    found = true;
                }
            }
            if (!found) {
                const size_t end_keyword = data_.find("endstream", stream_start);
                if (end_keyword == std::string::npos) return false;
                length = end_keyword - stream_start;
                while (length > 0 && (data_[stream_start + length - 1] == '\n' ||
                                      data_[stream_start + length - 1] == '\r')) {
                    --length;
                }
                stream_end = end_keyword;
            }

            out->stream = true;
            out->stream_off = stream_start;
            out->stream_len = length;
            after = stream_end + 9;
        } else {
            lex.set_pos(save);
            after = save;
        }
    }

    *num_out = static_cast<int>(number);
    *gen_out = static_cast<int>(generation);
    *after_out = after;
    return true;
}

void PdfParser::scan_objects() {
    size_t search = 0;
    while (search < data_.size()) {
        const size_t keyword = data_.find("obj", search);
        if (keyword == std::string::npos) break;
        const bool left_ok = keyword == 0 || is_delimiter(data_[keyword - 1]);
        const bool right_ok =
            keyword + 3 >= data_.size() || is_delimiter(data_[keyword + 3]);
        if (!left_ok || !right_ok) {
            search = keyword + 3;
            continue;
        }
        size_t start = 0;
        int number = 0;
        int generation = 0;
        if (!locate_object_header(keyword, &start, &number, &generation)) {
            search = keyword + 3;
            continue;
        }
        Value value;
        int parsed_number = 0;
        int parsed_generation = 0;
        size_t after = 0;
        if (parse_object_at(start, &parsed_number, &parsed_generation, &value, &after)) {
            if (objects_.find(parsed_number) == objects_.end()) {
                objects_[parsed_number] = std::move(value);
            }
            search = after > keyword ? after : keyword + 3;
        } else {
            search = keyword + 3;
        }
    }
}

// ---------------------------------------------------------------------------
// Xref (classic tables and xref streams)
// ---------------------------------------------------------------------------

void PdfParser::merge_trailer(const Value* trailer) {
    if (trailer == nullptr || trailer->kind != Value::Kind::Dict) return;
    static const char* keys[] = {"Root", "Info", "Encrypt", "Prev", "XRefStm", "Size"};
    for (const char* key : keys) {
        const auto it = trailer->dict.find(key);
        if (it != trailer->dict.end() && trailer_.find(key) == trailer_.end()) {
            trailer_[key] = it->second;
        }
    }
}

void PdfParser::parse_xref_table(size_t offset, int depth, std::set<size_t>& visited) {
    Lexer lex(data_, offset + 4);
    int sections = 0;
    bool has_trailer = false;
    while (sections < 10000) {
        lex.skip_space();
        if (lex.match_keyword("trailer")) {
            has_trailer = true;
            break;
        }
        long long first = 0;
        long long count = 0;
        if (!lex.read_int(&first) || !lex.read_int(&count)) break;
        if (first < 0 || count < 0 || count > 1000000) break;
        ++sections;
        for (long long i = 0; i < count; ++i) {
            long long entry_offset = 0;
            long long generation = 0;
            std::string flag;
            if (!lex.read_int(&entry_offset) || !lex.read_int(&generation) ||
                !lex.read_word(&flag)) {
                break;
            }
            const long long obj = first + i;
            if (obj < 0 || obj > INT_MAX) continue;
            if (flag == "n" && entry_offset >= 0) {
                XrefEntry entry;
                entry.type = 1;
                entry.offset = static_cast<size_t>(entry_offset);
                if (xref_.find(static_cast<int>(obj)) == xref_.end()) {
                    xref_[static_cast<int>(obj)] = entry;
                }
            }
        }
    }

    Value trailer;
    if (has_trailer && lex.parse_value(trailer, 0) && trailer.kind == Value::Kind::Dict) {
        merge_trailer(&trailer);
        if (const Value* prev = get_raw(&trailer, "Prev")) {
            const Value* resolved = resolve(prev);
            long long prev_offset = 0;
            if (value_int(resolved, &prev_offset) && prev_offset >= 0) {
                follow_xref(static_cast<size_t>(prev_offset), depth + 1, visited);
            }
        }
        if (const Value* xref_stm = get_raw(&trailer, "XRefStm")) {
            const Value* resolved = resolve(xref_stm);
            long long stream_offset = 0;
            if (value_int(resolved, &stream_offset) && stream_offset >= 0) {
                follow_xref(static_cast<size_t>(stream_offset), depth + 1, visited);
            }
        }
    }
}

void PdfParser::process_xref_stream(const Value& stream, int depth,
                                    std::set<size_t>& visited) {
    merge_trailer(&stream);

    const Value* widths = get(&stream, "W");
    if (widths == nullptr || widths->kind != Value::Kind::Array || widths->items.size() < 3) {
        return;
    }
    long long width[3] = {0, 0, 0};
    for (size_t i = 0; i < 3; ++i) {
        if (!value_int(resolve(&widths->items[i]), &width[i]) || width[i] < 0 || width[i] > 8) {
            return;
        }
    }
    const size_t row_size = static_cast<size_t>(width[0] + width[1] + width[2]);
    if (row_size == 0 || row_size > 64) return;

    std::vector<std::pair<long long, long long>> index;
    const Value* index_value = get(&stream, "Index");
    if (index_value != nullptr && index_value->kind == Value::Kind::Array) {
        for (size_t i = 0; i + 1 < index_value->items.size(); i += 2) {
            long long first = 0;
            long long count = 0;
            if (value_int(resolve(&index_value->items[i]), &first) &&
                value_int(resolve(&index_value->items[i + 1]), &count) && first >= 0 &&
                count >= 0) {
                index.emplace_back(first, count);
            }
        }
    }
    if (index.empty()) {
        long long size = 0;
        if (value_int(get(&stream, "Size"), &size) && size > 0) {
            index.emplace_back(0, size);
        }
    }
    if (index.empty()) return;

    std::string decoded;
    if (!decode_stream_bytes(&stream, &decoded) || decoded.empty()) return;

    size_t pos = 0;
    for (const auto& section : index) {
        for (long long i = 0; i < section.second; ++i) {
            if (pos + row_size > decoded.size()) return;
            unsigned long long fields[3] = {0, 0, 0};
            for (int field = 0; field < 3; ++field) {
                for (long long byte = 0; byte < width[field]; ++byte) {
                    fields[field] = (fields[field] << 8) |
                                    static_cast<unsigned char>(decoded[pos++]);
                }
            }
            const long long obj = section.first + i;
            if (obj < 0 || obj > INT_MAX) continue;
            int type = width[0] == 0 ? 1 : static_cast<int>(fields[0]);
            if (type == 1) {
                XrefEntry entry;
                entry.type = 1;
                entry.offset = static_cast<size_t>(fields[1]);
                if (xref_.find(static_cast<int>(obj)) == xref_.end()) {
                    xref_[static_cast<int>(obj)] = entry;
                }
            } else if (type == 2) {
                XrefEntry entry;
                entry.type = 2;
                entry.stream = static_cast<int>(fields[1]);
                entry.index = static_cast<int>(fields[2]);
                if (xref_.find(static_cast<int>(obj)) == xref_.end()) {
                    xref_[static_cast<int>(obj)] = entry;
                }
            }
        }
    }

    if (const Value* prev = get_raw(&stream, "Prev")) {
        const Value* resolved = resolve(prev);
        long long prev_offset = 0;
        if (value_int(resolved, &prev_offset) && prev_offset >= 0) {
            follow_xref(static_cast<size_t>(prev_offset), depth + 1, visited);
        }
    }
}

void PdfParser::follow_xref(size_t offset, int depth, std::set<size_t>& visited) {
    if (depth > 32 || offset >= data_.size()) return;
    if (!visited.insert(offset).second) return;

    Lexer probe(data_, offset);
    probe.skip_space();
    const size_t actual = probe.pos();
    if (actual + 4 <= data_.size() && data_.compare(actual, 4, "xref") == 0 &&
        (actual + 4 >= data_.size() || is_delimiter(data_[actual + 4]))) {
        parse_xref_table(actual, depth, visited);
        return;
    }

    int number = 0;
    int generation = 0;
    Value value;
    size_t after = 0;
    if (!parse_object_at(offset, &number, &generation, &value, &after)) return;
    if (value.kind == Value::Kind::Dict && type_name(&value) == "XRef") {
        process_xref_stream(value, depth, visited);
    }
}

void PdfParser::find_trailers() {
    size_t search = 0;
    int guard = 0;
    while (search < data_.size() && guard++ < 100000) {
        const size_t keyword = data_.find("trailer", search);
        if (keyword == std::string::npos) break;
        const bool left_ok = keyword == 0 || is_delimiter(data_[keyword - 1]);
        const bool right_ok =
            keyword + 7 >= data_.size() || is_delimiter(data_[keyword + 7]);
        if (!left_ok || !right_ok) {
            search = keyword + 7;
            continue;
        }
        Lexer lex(data_, keyword + 7);
        Value trailer;
        if (lex.parse_value(trailer, 0) && trailer.kind == Value::Kind::Dict) {
            merge_trailer(&trailer);
        }
        search = keyword + 7;
    }
}

void PdfParser::parse_xref_chain() {
    std::set<size_t> visited;
    const size_t startxref = data_.rfind("startxref");
    if (startxref != std::string::npos) {
        Lexer lex(data_, startxref + 9);
        long long offset = 0;
        if (lex.read_int(&offset) && offset >= 0 &&
            static_cast<size_t>(offset) < data_.size()) {
            follow_xref(static_cast<size_t>(offset), 0, visited);
        }
    }
    find_trailers();

    // Xref streams found by the raw scan also carry the trailer keys.
    for (const auto& entry : objects_) {
        if (entry.second.kind == Value::Kind::Dict && type_name(&entry.second) == "XRef") {
            merge_trailer(&entry.second);
        }
    }
}

void PdfParser::load_xref_objects() {
    for (const auto& entry : xref_) {
        if (entry.second.type != 1) continue;
        Value value;
        int number = 0;
        int generation = 0;
        size_t after = 0;
        if (!parse_object_at(entry.second.offset, &number, &generation, &value, &after)) {
            continue;
        }
        if (number != entry.first) continue;
        objects_[entry.first] = std::move(value);
    }
}

void PdfParser::load_object_streams() {
    std::vector<int> streams;
    for (const auto& entry : objects_) {
        if (entry.second.kind == Value::Kind::Dict && type_name(&entry.second) == "ObjStm") {
            streams.push_back(entry.first);
        }
    }
    for (int stream_number : streams) {
        const auto it = objects_.find(stream_number);
        if (it == objects_.end()) continue;
        const Value& stream = it->second;
        long long count = 0;
        long long first = 0;
        if (!value_int(get(&stream, "N"), &count) || count <= 0 || count > 100000) continue;
        if (!value_int(get(&stream, "First"), &first) || first < 0) continue;

        std::string decoded;
        if (!decode_stream_bytes(&stream, &decoded)) continue;
        if (static_cast<size_t>(first) >= decoded.size()) continue;

        Lexer header(decoded, 0);
        std::vector<std::pair<int, size_t>> objects;
        bool ok = true;
        for (long long i = 0; i < count; ++i) {
            long long number = 0;
            long long offset = 0;
            if (!header.read_int(&number) || !header.read_int(&offset)) {
                ok = false;
                break;
            }
            if (number < 0 || number > INT_MAX || offset < 0) {
                ok = false;
                break;
            }
            objects.emplace_back(static_cast<int>(number), static_cast<size_t>(offset));
        }
        if (!ok) continue;

        for (const auto& item : objects) {
            if (objects_.find(item.first) != objects_.end()) continue;
            const size_t position = static_cast<size_t>(first) + item.second;
            if (position >= decoded.size()) continue;
            Lexer lex(decoded, position);
            Value value;
            if (lex.parse_value(value, 0)) {
                objects_[item.first] = std::move(value);
            }
        }
    }
}

void PdfParser::fix_stream_lengths() {
    for (auto& entry : objects_) {
        Value& value = entry.second;
        if (!value.stream) continue;
        const Value* length_raw = get_raw(&value, "Length");
        if (length_raw == nullptr) continue;
        const Value* resolved = resolve(length_raw);
        double length = 0.0;
        if (!value_number(resolved, &length) || length < 0.0 || length > 100000000.0) continue;
        const size_t candidate = static_cast<size_t>(length);
        if (value.stream_off > data_.size()) continue;
        if (candidate > data_.size() - value.stream_off) continue;
        size_t check = value.stream_off + candidate;
        while (check < data_.size() && is_space_char(data_[check])) ++check;
        if (check + 9 <= data_.size() && data_.compare(check, 9, "endstream") == 0) {
            value.stream_len = candidate;
        }
    }
}

// ---------------------------------------------------------------------------
// Stream decoding
// ---------------------------------------------------------------------------

bool PdfParser::inflate_bytes(const std::string& input, std::string* out) const {
    if (input.empty()) {
        out->clear();
        return true;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int flags = attempt == 0 ? kTinflParseZlibHeader : 0;
        for (size_t capacity = 64u * 1024u; capacity <= kMaxStreamBytes; capacity *= 2) {
            std::string buffer;
            buffer.resize(capacity);
            const size_t produced = lgfx_tinfl_decompress_mem_to_mem(
                &buffer[0], capacity, input.data(), input.size(), flags);
            if (produced != static_cast<size_t>(-1)) {
                buffer.resize(produced);
                *out = std::move(buffer);
                return true;
            }
            if (capacity > kMaxStreamBytes / 2) break;
        }
    }
    return false;
}

bool PdfParser::decode_stream_bytes(const Value* stream, std::string* out) const {
    stream = resolve(stream);
    if (stream == nullptr || !stream->stream) return false;
    if (stream->stream_off > data_.size()) return false;
    const size_t available = data_.size() - stream->stream_off;
    const size_t length = stream->stream_len < available ? stream->stream_len : available;

    bool flate = false;
    const Value* filter = get(stream, "Filter");
    if (filter != nullptr) {
        if (filter->kind == Value::Kind::Name) {
            if (filter->str == "FlateDecode" || filter->str == "Fl") {
                flate = true;
            } else {
                return false;
            }
        } else if (filter->kind == Value::Kind::Array) {
            for (const Value& item : filter->items) {
                const Value* name = resolve(&item);
                if (name == nullptr || name->kind != Value::Kind::Name) return false;
                if (name->str == "FlateDecode" || name->str == "Fl") {
                    flate = true;
                } else {
                    return false;
                }
            }
        } else {
            return false;
        }
    }

    if (!flate) {
        if (length > kMaxStreamBytes) return false;
        out->assign(data_, stream->stream_off, length);
        return true;
    }
    const std::string input = data_.substr(stream->stream_off, length);
    return inflate_bytes(input, out);
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

FontInfo PdfParser::build_font(const Value* font_dict) const {
    FontInfo font;
    const Value* dict = resolve(font_dict);
    if (dict == nullptr || dict->kind != Value::Kind::Dict) {
        font.unsupported = true;
        return font;
    }
    const Value* subtype = get(dict, "Subtype");
    if (subtype != nullptr && subtype->kind == Value::Kind::Name && subtype->str == "Type0") {
        font.type0 = true;
        font.code_bytes = 2;
    }

    if (const Value* to_unicode = get(dict, "ToUnicode")) {
        std::string cmap;
        if (decode_stream_bytes(to_unicode, &cmap)) {
            parse_cmap(cmap, &font.to_unicode, &font.code_bytes);
        }
    }

    if (font.type0) {
        font.unsupported = font.to_unicode.empty();
        return font;
    }

    std::string base_encoding = "WinAnsiEncoding";
    const Value* encoding = get(dict, "Encoding");
    const Value* differences = nullptr;
    if (encoding != nullptr) {
        if (encoding->kind == Value::Kind::Name) {
            base_encoding = encoding->str;
        } else if (encoding->kind == Value::Kind::Dict) {
            if (const Value* base = get(encoding, "BaseEncoding")) {
                if (base->kind == Value::Kind::Name) base_encoding = base->str;
            }
            differences = get(encoding, "Differences");
        }
    }
    fill_encoding(font.simple_map, base_encoding);

    if (differences != nullptr && differences->kind == Value::Kind::Array) {
        int code = 0;
        for (const Value& item : differences->items) {
            const Value* value = resolve(&item);
            if (value == nullptr) continue;
            if (value->kind == Value::Kind::Num) {
                code = static_cast<int>(value->num);
            } else if (value->kind == Value::Kind::Name) {
                if (code >= 0 && code <= 255) {
                    const uint32_t codepoint = glyph_to_unicode(value->str);
                    if (codepoint != 0) font.simple_map[code] = codepoint;
                }
                ++code;
            }
        }
    }
    return font;
}

// Content-stream interpreter (defined after PdfParser because it calls it back).

class ContentParser {
  public:
    ContentParser(const PdfParser& pdf, const Value* resources)
        : pdf_(pdf), resources_(resources) {}

    void run(const std::string& content);

    const std::vector<TextFrag>& fragments() const { return fragments_; }
    bool unsupported() const { return unsupported_; }

  private:
    struct TextState {
        double font_size = 0.0;
        const FontInfo* font = nullptr;
        double char_spacing = 0.0;
        double word_spacing = 0.0;
        double horizontal_scale = 1.0;
        double leading = 0.0;
        double rise = 0.0;
        Mat text_matrix;
        Mat line_matrix;
        Mat ctm;
    };

    const FontInfo* load_font(const std::string& name);
    void execute(const std::string& op, const std::vector<Value>& operands);
    void show_text(const std::string& raw);
    void move_text(double tx, double ty);
    void next_line();
    int code_count(const FontInfo& font, const std::string& raw) const;

    const PdfParser& pdf_;
    const Value* resources_;
    std::map<std::string, FontInfo> fonts_;
    std::vector<TextFrag> fragments_;
    bool unsupported_ = false;
    bool in_text_ = false;
    TextState state_;
    std::vector<Mat> saved_ctm_;
};

const FontInfo* ContentParser::load_font(const std::string& name) {
    const auto cached = fonts_.find(name);
    if (cached != fonts_.end()) return &cached->second;

    FontInfo font;
    const Value* font_resources = pdf_.get(resources_, "Font");
    if (font_resources != nullptr && font_resources->kind == Value::Kind::Dict) {
        const auto it = font_resources->dict.find(name);
        if (it != font_resources->dict.end()) {
            font = pdf_.build_font(&it->second);
        } else {
            font.unsupported = true;
        }
    } else {
        font.unsupported = true;
    }
    const auto inserted = fonts_.emplace(name, std::move(font));
    return &inserted.first->second;
}

int ContentParser::code_count(const FontInfo& font, const std::string& raw) const {
    if (!font.type0) return static_cast<int>(raw.size());
    const int bytes = font.code_bytes < 1 ? 1 : font.code_bytes;
    return static_cast<int>(raw.size() / static_cast<size_t>(bytes));
}

void ContentParser::move_text(double tx, double ty) {
    state_.line_matrix = mat_mul(mat_translate(tx, ty), state_.line_matrix);
    state_.text_matrix = state_.line_matrix;
}

void ContentParser::next_line() { move_text(0.0, -state_.leading); }

void ContentParser::show_text(const std::string& raw) {
    const FontInfo* font = state_.font;
    if (font == nullptr) return;
    if (font->unsupported) {
        unsupported_ = true;
        return;
    }
    const int count = code_count(*font, raw);
    const double size = std::fabs(state_.font_size);
    const double glyph_width = static_cast<double>(count) * 0.5 * size * state_.horizontal_scale;

    const std::string text = decode_font_text(*font, raw);
    if (!text.empty()) {
        const Mat rise_matrix = mat_mul(mat_translate(0.0, state_.rise), state_.text_matrix);
        const Mat rendering = mat_mul(rise_matrix, state_.ctm);
        double scale = std::hypot(rendering.a, rendering.b);
        if (!(scale > 0.0001)) scale = 1.0;
        double device_size = size * scale;
        if (!(device_size > 0.1)) device_size = 12.0;

        TextFrag fragment;
        fragment.x = rendering.e;
        fragment.y = rendering.f;
        fragment.size = device_size;
        fragment.width = glyph_width * scale;
        fragment.text = text;
        if (fragments_.size() < kMaxFragments) fragments_.push_back(std::move(fragment));
    }

    int spaces = 0;
    for (char c : text) {
        if (c == ' ') ++spaces;
    }
    const double advance = static_cast<double>(count) * 0.5 * state_.font_size +
                           state_.char_spacing * static_cast<double>(count) +
                           state_.word_spacing * static_cast<double>(spaces);
    state_.text_matrix =
        mat_mul(mat_translate(advance * state_.horizontal_scale, 0.0), state_.text_matrix);
}

void ContentParser::execute(const std::string& op, const std::vector<Value>& operands) {
    auto number_at = [&operands](size_t index, double* out) {
        if (index >= operands.size()) return false;
        return value_number(&operands[index], out);
    };

    if (op == "BT") {
        state_.text_matrix = Mat();
        state_.line_matrix = Mat();
        in_text_ = true;
        return;
    }
    if (op == "ET") {
        in_text_ = false;
        return;
    }
    if (op == "Tf") {
        if (operands.size() >= 2 && operands[0].kind == Value::Kind::Name) {
            double size = 0.0;
            if (number_at(1, &size)) state_.font_size = size;
            state_.font = load_font(operands[0].str);
        }
        return;
    }
    if (op == "Td") {
        double tx = 0.0;
        double ty = 0.0;
        if (number_at(0, &tx) && number_at(1, &ty)) move_text(tx, ty);
        return;
    }
    if (op == "TD") {
        double tx = 0.0;
        double ty = 0.0;
        if (number_at(0, &tx) && number_at(1, &ty)) {
            state_.leading = -ty;
            move_text(tx, ty);
        }
        return;
    }
    if (op == "Tm") {
        double values[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        for (size_t i = 0; i < 6; ++i) {
            if (!number_at(i, &values[i])) return;
        }
        Mat matrix;
        matrix.a = values[0];
        matrix.b = values[1];
        matrix.c = values[2];
        matrix.d = values[3];
        matrix.e = values[4];
        matrix.f = values[5];
        state_.line_matrix = matrix;
        state_.text_matrix = matrix;
        return;
    }
    if (op == "T*") {
        next_line();
        return;
    }
    if (op == "Tj") {
        if (!operands.empty() && operands[0].kind == Value::Kind::Str) {
            show_text(operands[0].str);
        }
        return;
    }
    if (op == "TJ") {
        if (operands.empty() || operands[0].kind != Value::Kind::Array) return;
        for (const Value& item : operands[0].items) {
            if (item.kind == Value::Kind::Str) {
                show_text(item.str);
            } else if (item.kind == Value::Kind::Num) {
                const double shift =
                    -item.num / 1000.0 * std::fabs(state_.font_size) * state_.horizontal_scale;
                state_.text_matrix = mat_mul(mat_translate(shift, 0.0), state_.text_matrix);
            }
        }
        return;
    }
    if (op == "'") {
        next_line();
        if (!operands.empty() && operands[0].kind == Value::Kind::Str) {
            show_text(operands[0].str);
        }
        return;
    }
    if (op == "\"") {
        double word_spacing = 0.0;
        double char_spacing = 0.0;
        if (number_at(0, &word_spacing)) state_.word_spacing = word_spacing;
        if (number_at(1, &char_spacing)) state_.char_spacing = char_spacing;
        next_line();
        if (operands.size() >= 3 && operands[2].kind == Value::Kind::Str) {
            show_text(operands[2].str);
        }
        return;
    }
    if (op == "TL") {
        double leading = 0.0;
        if (number_at(0, &leading)) state_.leading = leading;
        return;
    }
    if (op == "Tc") {
        double spacing = 0.0;
        if (number_at(0, &spacing)) state_.char_spacing = spacing;
        return;
    }
    if (op == "Tw") {
        double spacing = 0.0;
        if (number_at(0, &spacing)) state_.word_spacing = spacing;
        return;
    }
    if (op == "Tz") {
        double scale = 0.0;
        if (number_at(0, &scale)) state_.horizontal_scale = scale / 100.0;
        return;
    }
    if (op == "Ts") {
        double rise = 0.0;
        if (number_at(0, &rise)) state_.rise = rise;
        return;
    }
    if (op == "cm") {
        double values[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        for (size_t i = 0; i < 6; ++i) {
            if (!number_at(i, &values[i])) return;
        }
        Mat matrix;
        matrix.a = values[0];
        matrix.b = values[1];
        matrix.c = values[2];
        matrix.d = values[3];
        matrix.e = values[4];
        matrix.f = values[5];
        state_.ctm = mat_mul(matrix, state_.ctm);
        return;
    }
    if (op == "q") {
        saved_ctm_.push_back(state_.ctm);
        return;
    }
    if (op == "Q") {
        if (!saved_ctm_.empty()) {
            state_.ctm = saved_ctm_.back();
            saved_ctm_.pop_back();
        }
        return;
    }
}

void ContentParser::run(const std::string& content) {
    Lexer lex(content);
    std::vector<Value> operands;
    operands.reserve(16);

    while (true) {
        lex.skip_space();
        if (lex.eof()) break;
        const size_t start = lex.pos();
        const char c = lex.peek();

        if (c == '[' || c == '<' || c == '/' || c == '(' || is_number_start(c)) {
            Value value;
            if (lex.parse_value(value, 0)) {
                operands.push_back(std::move(value));
                continue;
            }
            lex.set_pos(start + 1);
            operands.clear();
            continue;
        }

        std::string keyword;
        if (!lex.read_word(&keyword)) {
            lex.set_pos(start + 1);
            continue;
        }
        if (keyword == "BI") {
            // Skip inline image: BI <dict> ID <binary> EI.
            const size_t id = content.find("ID", lex.pos());
            if (id == std::string::npos) break;
            size_t position = id + 2;
            if (position < content.size() && is_space_char(content[position])) ++position;
            bool found = false;
            size_t limit = position;
            while (limit < content.size()) {
                const size_t ei = content.find("EI", limit);
                if (ei == std::string::npos) break;
                const bool left_ok =
                    ei == 0 || is_space_char(content[ei - 1]) || is_delimiter(content[ei - 1]);
                const bool right_ok = ei + 2 >= content.size() ||
                                      is_space_char(content[ei + 2]) ||
                                      is_delimiter(content[ei + 2]);
                if (left_ok && right_ok) {
                    lex.set_pos(ei + 2);
                    found = true;
                    break;
                }
                limit = ei + 2;
            }
            if (!found) break;
            operands.clear();
            continue;
        }
        execute(keyword, operands);
        operands.clear();
    }
}

// ---------------------------------------------------------------------------
// Page tree
// ---------------------------------------------------------------------------

std::string PdfParser::gather_contents(const PageNode& page) const {
    std::string result;
    const Value* contents = get(page.dict, "Contents");
    if (contents == nullptr) return result;

    auto append_stream = [&](const Value* stream) {
        std::string decoded;
        if (!decode_stream_bytes(stream, &decoded) || decoded.empty()) return;
        if (result.size() >= kMaxContentBytes) return;
        const size_t room = kMaxContentBytes - result.size();
        if (decoded.size() > room) decoded.resize(room);
        result += decoded;
        if (result.size() < kMaxContentBytes) result.push_back('\n');
    };

    if (contents->kind == Value::Kind::Array) {
        for (const Value& item : contents->items) append_stream(&item);
    } else {
        append_stream(contents);
    }
    return result;
}

void PdfParser::walk_pages(const Value& node, const Value* inherited_resources,
                           const Value* inherited_media_box, int depth) {
    if (depth > 64 || pages_.size() >= kMaxPages) return;
    if (node.kind == Value::Kind::Ref) {
        if (!visited_page_nodes_.insert(node.ref).second) return;
    }
    const Value* dict = resolve(&node);
    if (dict == nullptr || dict->kind != Value::Kind::Dict) return;

    const Value* resources = get(dict, "Resources");
    if (resources == nullptr) resources = inherited_resources;
    const Value* media_box = get(dict, "MediaBox");
    if (media_box == nullptr) media_box = inherited_media_box;

    const Value* kids = get(dict, "Kids");
    if (kids != nullptr && kids->kind == Value::Kind::Array) {
        for (const Value& kid : kids->items) {
            walk_pages(kid, resources, media_box, depth + 1);
            if (pages_.size() >= kMaxPages) return;
        }
        return;
    }

    const bool is_page = type_name(dict) == "Page";
    if (is_page || get(dict, "Contents") != nullptr) {
        PageNode page;
        page.dict = dict;
        page.resources = resources;
        page.media_box = media_box;
        pages_.push_back(page);
    }
}

void PdfParser::collect_pages_fallback() {
    for (const auto& entry : objects_) {
        const Value& value = entry.second;
        if (value.kind != Value::Kind::Dict) continue;
        if (type_name(&value) != "Page") continue;
        PageNode page;
        page.dict = &value;
        page.resources = get(&value, "Resources");
        page.media_box = get(&value, "MediaBox");
        pages_.push_back(page);
        if (pages_.size() >= kMaxPages) break;
    }
}

// ---------------------------------------------------------------------------
// Paragraph assembly
// ---------------------------------------------------------------------------

std::vector<Paragraph> make_paragraphs(std::vector<TextFrag> fragments, size_t page_index) {
    std::vector<Paragraph> paragraphs;
    fragments.erase(std::remove_if(fragments.begin(), fragments.end(),
                                   [](const TextFrag& fragment) {
                                       return fragment.text.empty();
                                   }),
                    fragments.end());
    if (fragments.empty()) return paragraphs;

    std::sort(fragments.begin(), fragments.end(), [](const TextFrag& left, const TextFrag& right) {
        if (left.y != right.y) return left.y > right.y;
        return left.x < right.x;
    });

    struct Row {
        double y = 0.0;
        double size = 12.0;
        std::vector<TextFrag> items;
        std::string text;
    };
    std::vector<Row> rows;
    rows.reserve(32);
    for (TextFrag& fragment : fragments) {
        Row* target = nullptr;
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            const double tolerance = 0.5 * std::max(1.0, std::min(it->size, fragment.size));
            if (std::fabs(it->y - fragment.y) <= tolerance) {
                target = &*it;
                break;
            }
            if (it->y - fragment.y > 4.0 * std::max(it->size, fragment.size)) break;
        }
        if (target == nullptr) {
            Row row;
            row.y = fragment.y;
            row.size = fragment.size;
            row.items.push_back(std::move(fragment));
            rows.push_back(std::move(row));
        } else {
            const double weight = static_cast<double>(target->items.size());
            target->y = (target->y * weight + fragment.y) / (weight + 1.0);
            target->size = std::max(target->size, fragment.size);
            target->items.push_back(std::move(fragment));
        }
    }

    std::sort(rows.begin(), rows.end(),
              [](const Row& left, const Row& right) { return left.y > right.y; });

    std::vector<Row> lines;
    lines.reserve(rows.size());
    for (Row& row : rows) {
        std::sort(row.items.begin(), row.items.end(),
                  [](const TextFrag& left, const TextFrag& right) { return left.x < right.x; });
        std::string text;
        double last_end = -1e30;
        double line_size = 12.0;
        bool first = true;
        for (const TextFrag& fragment : row.items) {
            if (first) {
                text = fragment.text;
                line_size = fragment.size;
                last_end = fragment.x + fragment.width;
                first = false;
                continue;
            }
            const double gap = fragment.x - last_end;
            const double reference = std::max(1.0, std::min(line_size, fragment.size));
            if (gap > 0.2 * reference && !text.empty() && text.back() != ' ' &&
                !fragment.text.empty() && fragment.text.front() != ' ') {
                text.push_back(' ');
            }
            text += fragment.text;
            line_size = std::max(line_size, fragment.size);
            last_end = std::max(last_end, fragment.x + fragment.width);
        }
        const size_t begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) continue;
        const size_t end = text.find_last_not_of(" \t\r\n");
        text = text.substr(begin, end - begin + 1);
        if (text.empty()) continue;
        Row line;
        line.y = row.y;
        line.size = row.size;
        line.text = std::move(text);
        lines.push_back(std::move(line));
    }
    if (lines.empty()) return paragraphs;

    std::vector<double> gaps;
    gaps.reserve(lines.size());
    double average_size = 0.0;
    for (size_t i = 0; i < lines.size(); ++i) {
        average_size += lines[i].size;
        if (i > 0) {
            const double gap = lines[i - 1].y - lines[i].y;
            if (gap > 0.1) gaps.push_back(gap);
        }
    }
    average_size /= static_cast<double>(lines.size());
    if (!(average_size > 0.1)) average_size = 12.0;

    double line_spacing = 1.2 * average_size;
    if (!gaps.empty()) {
        std::vector<double> sorted = gaps;
        const size_t middle = sorted.size() / 2;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(middle), sorted.end());
        const double median = sorted[middle];
        if (median > 0.75 * line_spacing && median < 2.0 * line_spacing) {
            line_spacing = median;
        }
    }
    const double paragraph_gap = 1.6 * line_spacing;

    std::string current;
    auto flush = [&]() {
        if (current.empty()) return;
        Paragraph paragraph;
        paragraph.kind = ParaKind::Text;
        Run run;
        run.style.face = Face::Regular;
        run.text = current;
        paragraph.runs.push_back(std::move(run));
        paragraph.token = (static_cast<uint64_t>(page_index) << 32) |
                          static_cast<uint64_t>(paragraphs.size());
        paragraphs.push_back(std::move(paragraph));
        current.clear();
    };

    double previous_y = lines[0].y;
    current = lines[0].text;
    for (size_t i = 1; i < lines.size(); ++i) {
        const double gap = previous_y - lines[i].y;
        if (gap > paragraph_gap) {
            flush();
            current = lines[i].text;
        } else {
            bool joined = false;
            if (!current.empty() && current.back() == '-' && !lines[i].text.empty()) {
                size_t last_start = 0;
                const uint32_t before = utf8_last_codepoint(
                    current.substr(0, current.size() - 1), &last_start);
                (void)last_start;
                const uint32_t first = utf8_first_codepoint(lines[i].text);
                if (codepoint_is_letter(before) && codepoint_is_lowercase(first)) {
                    current.pop_back();
                    current += lines[i].text;
                    joined = true;
                }
            }
            if (!joined) {
                if (!current.empty() && current.back() != ' ' && lines[i].text.front() != ' ') {
                    current.push_back(' ');
                }
                current += lines[i].text;
            }
        }
        previous_y = lines[i].y;
    }
    flush();
    return paragraphs;
}

std::vector<Paragraph> PdfParser::extract_page(const PageNode& page, size_t page_index,
                                              size_t& total_bytes) {
    std::vector<Paragraph> result;
    const std::string content = gather_contents(page);
    if (content.empty()) return result;

    ContentParser parser(*this, page.resources);
    parser.run(content);
    if (parser.unsupported()) return result;

    std::vector<TextFrag> fragments = parser.fragments();
    result = make_paragraphs(std::move(fragments), page_index);

    std::vector<Paragraph> kept;
    size_t page_bytes = 0;
    for (Paragraph& paragraph : result) {
        const size_t length = paragraph_byte_length(paragraph);
        if (page_bytes + length > kMaxPageText) break;
        if (total_bytes + page_bytes + length > kMaxTotalText) break;
        page_bytes += length;
        kept.push_back(std::move(paragraph));
    }
    total_bytes += page_bytes;
    return kept;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

bool PdfParser::load(std::string* error) {
    if (data_.size() < 8) {
        set_error(error, "PDF: file is too small");
        return false;
    }
    const size_t header = data_.find("%PDF-");
    if (header == std::string::npos || header > 1024) {
        set_error(error, "PDF: bad file header");
        return false;
    }

    scan_objects();
    parse_xref_chain();
    load_xref_objects();
    load_object_streams();
    fix_stream_lengths();

    if (trailer_.find("Encrypt") != trailer_.end()) encrypted_ = true;

    return true;
}

const Value* PdfParser::find_catalog() const {
    const Value* fallback = nullptr;
    for (const auto& entry : objects_) {
        const Value& value = entry.second;
        if (value.kind != Value::Kind::Dict) continue;
        if (type_name(&value) != "Catalog") continue;
        if (get(&value, "Pages") != nullptr) return &value;
        if (fallback == nullptr) fallback = &value;
    }
    return fallback;
}

void PdfParser::fill_info() {
    const auto info_it = trailer_.find("Info");
    if (info_it == trailer_.end()) return;
    const Value* info = resolve(&info_it->second);
    if (info == nullptr || info->kind != Value::Kind::Dict) return;
    const Value* title = get(info, "Title");
    if (title != nullptr && title->kind == Value::Kind::Str) {
        info_.title = pdf_string_to_utf8(title->str);
    }
    const Value* author = get(info, "Author");
    if (author != nullptr && author->kind == Value::Kind::Str) {
        info_.author = pdf_string_to_utf8(author->str);
    }
}

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

class PdfDocument final : public Document {
  public:
    PdfDocument(DocInfo info, std::vector<std::vector<Paragraph>> pages)
        : info_(std::move(info)), pages_(std::move(pages)) {
        for (const auto& page : pages_) {
            for (const Paragraph& paragraph : page) {
                for (const Run& run : paragraph.runs) chars_ += utf8_length(run.text);
            }
        }
    }

    const DocInfo& info() const override { return info_; }

    bool rewind() override {
        page_ = 0;
        paragraph_ = 0;
        return true;
    }

    bool next(Paragraph& out) override {
        while (page_ < pages_.size()) {
            if (paragraph_ < pages_[page_].size()) {
                out = pages_[page_][paragraph_++];
                return true;
            }
            ++page_;
            paragraph_ = 0;
        }
        return false;
    }

    bool seek(uint64_t token) override {
        const size_t page = static_cast<size_t>(token >> 32);
        const size_t paragraph = static_cast<size_t>(token & 0xFFFFFFFFull);
        if (page >= pages_.size()) {
            page_ = pages_.size();
            paragraph_ = 0;
            return false;
        }
        if (paragraph < pages_[page].size()) {
            page_ = page;
            paragraph_ = paragraph;
            return true;
        }
        for (size_t i = page + 1; i < pages_.size(); ++i) {
            if (!pages_[i].empty()) {
                page_ = i;
                paragraph_ = 0;
                return true;
            }
        }
        page_ = pages_.size();
        paragraph_ = 0;
        return false;
    }

    uint32_t permille(uint64_t token) const override {
        if (pages_.empty()) return 0;
        size_t page = static_cast<size_t>(token >> 32);
        if (page >= pages_.size()) page = pages_.size() - 1;
        return static_cast<uint32_t>(page * 1000 / pages_.size());
    }

    uint64_t total_chars() const override { return chars_; }

  private:
    DocInfo info_;
    std::vector<std::vector<Paragraph>> pages_;
    size_t page_ = 0;
    size_t paragraph_ = 0;
    uint64_t chars_ = 0;
};

std::unique_ptr<Document> PdfParser::build(std::string* error) {
    if (encrypted_) {
        set_error(error, "PDF is encrypted");
        return nullptr;
    }

    const Value* root = nullptr;
    const auto root_it = trailer_.find("Root");
    if (root_it != trailer_.end()) root = resolve(&root_it->second);
    if (root == nullptr || root->kind != Value::Kind::Dict) root = find_catalog();
    if (root == nullptr) {
        set_error(error, "PDF: document structure not found");
        return nullptr;
    }

    visited_page_nodes_.clear();
    pages_.clear();
    if (get(root, "Pages") != nullptr) {
        const Value* raw_root = get_raw(root, "Pages");
        if (raw_root != nullptr) walk_pages(*raw_root, nullptr, nullptr, 0);
    }
    if (pages_.empty()) collect_pages_fallback();
    if (pages_.empty()) {
        set_error(error, "PDF: pages not found");
        return nullptr;
    }

    fill_info();

    std::vector<std::vector<Paragraph>> page_paragraphs;
    page_paragraphs.reserve(pages_.size());
    size_t total_bytes = 0;
    size_t non_empty = 0;
    for (size_t i = 0; i < pages_.size(); ++i) {
        if (total_bytes >= kMaxTotalText) break;
        std::vector<Paragraph> paragraphs = extract_page(pages_[i], i, total_bytes);
        for (const Paragraph& paragraph : paragraphs) {
            if (paragraph_byte_length(paragraph) > 0) {
                ++non_empty;
                break;
            }
        }
        page_paragraphs.push_back(std::move(paragraphs));
    }
    if (non_empty == 0) {
        set_error(error, "PDF has no text layer (scans are not supported)");
        return nullptr;
    }

    return std::unique_ptr<Document>(
        new PdfDocument(std::move(info_), std::move(page_paragraphs)));
}

} // namespace

std::unique_ptr<Document> open_pdf(std::unique_ptr<ByteSource> source, std::string* error) {
    if (error != nullptr) error->clear();
    if (!source || !source->ok()) {
        set_error(error, "PDF: cannot open the file");
        return nullptr;
    }
    std::string data;
    std::string read_error;
    if (!read_all(*source, kMaxPdfBytes, &data, &read_error)) {
        set_error(error, "PDF: file is too large or unreadable");
        return nullptr;
    }
    source.reset();
    PdfParser parser(std::move(data));
    if (!parser.load(error)) return nullptr;
    return parser.build(error);
}

} // namespace reader
