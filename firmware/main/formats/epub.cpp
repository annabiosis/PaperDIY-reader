// EPUB 2/3 reader.
//
// The container is read with a small self-contained ZIP reader (stored/deflate
// only), metadata and reading order come from META-INF/container.xml + the OPF
// package, and each spine document is turned into paragraphs by a tolerant
// hand-written markup tokenizer. No XML/HTML library, no exceptions.
//
// Memory policy: the archive itself (<= 8 MiB) stays in RAM, but only the
// currently read spine chapter is expanded and tokenized. Plain-text lengths of
// the other chapters are cached lazily so permille()/total_chars() stay cheap.
//
// Paragraph tokens are (spine_index << 32) | byte_offset_in_chapter_text, where
// chapter text is the paragraph texts joined with '\n'. seek() maps a token back
// to the nearest paragraph at/after that offset.

#include "formats/formats.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/byte_source.h"
#include "core/encoding.h"
#include "core/utf8.h"
#include "text/doc.h"
#include "text/types.h"

// miniz v1.15 lives in components/miniz and is built for the firmware as well;
// the amalgamated symbols are prefixed with lgfx_. ZIP entries use raw deflate,
// so no zlib header flag is passed.
extern "C" size_t lgfx_tinfl_decompress_mem_to_mem(void* pOut, size_t out_len, const void* pSrc,
                                                   size_t src_len, int flags);

namespace reader {
namespace {

constexpr size_t kNpos = static_cast<size_t>(-1);
constexpr size_t kMaxArchiveBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxEntries = 4096u;
constexpr size_t kMaxEntryName = 512u;
constexpr size_t kMaxChapterBytes = 4u * 1024u * 1024u;
constexpr uint64_t kMaxUncompressedBytes = 32ull * 1024ull * 1024ull;
constexpr uint64_t kUnknownLength = ~static_cast<uint64_t>(0);

// "[image]" (UTF-8) is emitted for every <img> element.
constexpr char kImageLabel[] = "[\xD0\xB8\xD0\xB7\xD0\xBE\xD0\xB1\xD1\x80\xD0\xB0\xD0\xB6\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5]";

// ---------------------------------------------------------------- byte helpers

inline bool has_bytes(const std::string& data, size_t at, size_t count) {
    return at <= data.size() && count <= data.size() - at;
}

uint16_t rd16(const std::string& data, size_t at) {
    return static_cast<uint16_t>(static_cast<uint8_t>(data[at]) |
                                 (static_cast<uint8_t>(data[at + 1]) << 8));
}

uint32_t rd32(const std::string& data, size_t at) {
    return static_cast<uint32_t>(static_cast<uint8_t>(data[at])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[at + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[at + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[at + 3])) << 24);
}

inline char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string to_lower(std::string text) {
    for (char& c : text) c = lower_ascii(c);
    return text;
}

inline bool is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

inline bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

inline bool is_name_start(char c) {
    return is_ascii_alpha(c) || c == '_' || c == ':';
}

inline bool is_name_char(char c) {
    return is_ascii_alpha(c) || is_ascii_digit(c) || c == '_' || c == ':' || c == '-' || c == '.';
}

inline bool is_collapsible_space(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v' ||
           cp == 0x00A0u || (cp >= 0x2000u && cp <= 0x200Au) || cp == 0x2028u || cp == 0x2029u ||
           cp == 0x202Fu || cp == 0x205Fu || cp == 0x3000u;
}

// Case-insensitive substring search, used for raw-text elements and closing tags.
size_t find_ci(const std::string& haystack, size_t from, const std::string& needle) {
    if (needle.empty() || from >= haystack.size() || needle.size() > haystack.size() - from) {
        return kNpos;
    }
    const char first = lower_ascii(needle[0]);
    for (size_t i = from; i + needle.size() <= haystack.size(); ++i) {
        if (lower_ascii(haystack[i]) != first) continue;
        size_t j = 1;
        while (j < needle.size() && lower_ascii(haystack[i + j]) == lower_ascii(needle[j])) ++j;
        if (j == needle.size()) return i;
    }
    return kNpos;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// -------------------------------------------------------------------- entities

const std::map<std::string, std::string>& named_entities() {
    // Source file is UTF-8; values below are UTF-8 sequences.
    static const std::map<std::string, std::string> kEntities = {
        {"amp", "&"},
        {"apos", "'"},
        {"bdquo", "\xE2\x80\x9E"},
        {"bull", "\xE2\x80\xA2"},
        {"copy", "\xC2\xA9"},
        {"dagger", "\xE2\x80\xA0"},
        {"Dagger", "\xE2\x80\xA1"},
        {"deg", "\xC2\xB0"},
        {"divide", "\xC3\xB7"},
        {"emsp", " "},
        {"ensp", " "},
        {"euro", "\xE2\x82\xAC"},
        {"frac12", "\xC2\xBD"},
        {"frac14", "\xC2\xBC"},
        {"frac34", "\xC2\xBE"},
        {"gt", ">"},
        {"hellip", "\xE2\x80\xA6"},
        {"laquo", "\xC2\xAB"},
        {"ldquo", "\xE2\x80\x9C"},
        {"lsquo", "\xE2\x80\x98"},
        {"lt", "<"},
        {"lrm", ""},
        {"mdash", "\xE2\x80\x94"},
        {"micro", "\xC2\xB5"},
        {"middot", "\xC2\xB7"},
        {"nbsp", "\xC2\xA0"},
        {"ndash", "\xE2\x80\x93"},
        {"para", "\xC2\xB6"},
        {"permil", "\xE2\x80\xB0"},
        {"plusmn", "\xC2\xB1"},
        {"quot", "\""},
        {"raquo", "\xC2\xBB"},
        {"rdquo", "\xE2\x80\x9D"},
        {"reg", "\xC2\xAE"},
        {"rlm", ""},
        {"rsquo", "\xE2\x80\x99"},
        {"sbquo", "\xE2\x80\x9A"},
        {"sect", "\xC2\xA7"},
        {"shy", ""},
        {"sup2", "\xC2\xB2"},
        {"sup3", "\xC2\xB3"},
        {"thinsp", " "},
        {"times", "\xC3\x97"},
        {"trade", "\xE2\x84\xA2"},
        {"zwnj", ""},
        {"zwj", ""},
    };
    return kEntities;
}

// Decodes the entity set the reader cares about; unknown entities disappear.
std::string decode_entities(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') {
            out.push_back(raw[i]);
            continue;
        }
        const size_t semi = raw.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) {
            out.push_back('&');
            continue;
        }
        const std::string body = raw.substr(i + 1, semi - i - 1);
        if (!body.empty() && body[0] == '#') {
            const bool hex = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
            size_t index = hex ? 2 : 1;
            uint32_t codepoint = 0;
            bool valid = index < body.size();
            for (; valid && index < body.size(); ++index) {
                const int digit = hex ? hex_digit(body[index]) : (is_ascii_digit(body[index]) ? body[index] - '0' : -1);
                if (digit < 0) {
                    valid = false;
                    break;
                }
                codepoint = codepoint * (hex ? 16u : 10u) + static_cast<uint32_t>(digit);
                if (codepoint > 0x10FFFFu) valid = false;
            }
            if (valid && codepoint != 0 && !(codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
                utf8_append(out, codepoint);
            } else {
                out.push_back('&'); // malformed reference: keep the ampersand
            }
            i = semi;
            continue;
        }
        auto exact = named_entities().find(body);
        if (exact == named_entities().end()) exact = named_entities().find(to_lower(body));
        if (exact != named_entities().end()) {
            out += exact->second;
        }
        // Unknown named entity: drop it.
        i = semi;
    }
    return out;
}

std::string collapse_ws(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pending = false;
    for (size_t i = 0; i < text.size();) {
        size_t used = 1;
        const uint32_t cp = utf8_decode(text.data() + i, text.size() - i, &used);
        if (used == 0) used = 1;
        if (is_collapsible_space(cp)) {
            if (!out.empty()) pending = true;
        } else {
            if (pending) {
                out.push_back(' ');
                pending = false;
            }
            out.append(text, i, used);
        }
        i += used;
    }
    return out;
}

// ------------------------------------------------------------------- ZIP layer

// Normalizes an archive path: backslashes to slashes, dot segments resolved,
// leading/trailing slashes dropped. Returns "" for paths escaping the root.
std::string normalize_path(const std::string& raw) {
    std::string text = raw;
    for (char& c : text) {
        if (c == '\\') c = '/';
    }
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t slash = text.find('/', pos);
        if (slash == std::string::npos) slash = text.size();
        const std::string segment = text.substr(pos, slash - pos);
        if (segment == "..") {
            if (parts.empty()) return std::string();
            parts.pop_back();
        } else if (!segment.empty() && segment != ".") {
            parts.push_back(segment);
        }
        if (slash >= text.size()) break;
        pos = slash + 1;
    }
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) out.push_back('/');
        out += part;
    }
    return out;
}

std::string percent_decode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = hex_digit(text[i + 1]);
            const int low = hex_digit(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string dirname_of(const std::string& path) {
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Resolves a manifest href against the OPF directory (handles ../, %20, #frag).
std::string resolve_href(const std::string& base_dir, const std::string& href) {
    std::string clean = href;
    const size_t fragment = clean.find('#');
    if (fragment != std::string::npos) clean.resize(fragment);
    const size_t query = clean.find('?');
    if (query != std::string::npos) clean.resize(query);
    clean = percent_decode(clean);
    if (clean.empty()) return std::string();
    if (clean[0] == '/') return normalize_path(clean);
    if (base_dir.empty()) return normalize_path(clean);
    return normalize_path(base_dir + "/" + clean);
}

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint16_t flags = 0;
    uint32_t comp_size = 0;
    uint32_t uncomp_size = 0;
    uint32_t local_offset = 0;
};

class EpubArchive {
  public:
    std::string data;
    std::vector<ZipEntry> entries;

    bool parse(std::string* error) {
        if (data.size() < 22) {
            if (error) *error = "epub: archive is too small";
            return false;
        }
        size_t eocd = kNpos;
        size_t candidate = kNpos;
        const size_t lowest = data.size() > 22 + 65535 ? data.size() - 22 - 65535 : 0;
        for (size_t i = data.size() - 22 + 1; i-- > lowest;) {
            if (rd32(data, i) != 0x06054b50u) continue;
            const uint16_t comment = rd16(data, i + 20);
            if (i + 22u + comment > data.size()) continue;
            if (i + 22u + comment == data.size()) {
                eocd = i;
                break;
            }
            if (candidate == kNpos) candidate = i;
        }
        if (eocd == kNpos) eocd = candidate;
        if (eocd == kNpos) {
            if (error) *error = "epub: end of central directory not found";
            return false;
        }

        const uint16_t entries_disk = rd16(data, eocd + 8);
        const uint16_t entries_total = rd16(data, eocd + 10);
        const uint32_t cd_size = rd32(data, eocd + 12);
        const uint32_t cd_offset = rd32(data, eocd + 16);
        if (entries_disk == 0xFFFFu || entries_total == 0xFFFFu || cd_size == 0xFFFFFFFFu ||
            cd_offset == 0xFFFFFFFFu) {
            if (error) *error = "epub: ZIP64 archives are not supported";
            return false;
        }
        const size_t count = entries_total != 0 ? entries_total : entries_disk;
        if (count == 0) {
            if (error) *error = "epub: ZIP archive has no entries";
            return false;
        }
        if (count > kMaxEntries) {
            if (error) *error = "epub: too many ZIP entries";
            return false;
        }
        if (static_cast<uint64_t>(cd_offset) + cd_size > data.size()) {
            if (error) *error = "epub: central directory is out of range";
            return false;
        }

        size_t pos = cd_offset;
        uint64_t uncompressed_total = 0;
        for (size_t n = 0; n < count; ++n) {
            if (!has_bytes(data, pos, 46) || rd32(data, pos) != 0x02014b50u) break;
            const uint16_t flags = rd16(data, pos + 8);
            const uint16_t method = rd16(data, pos + 10);
            const uint32_t comp_size = rd32(data, pos + 20);
            const uint32_t uncomp_size = rd32(data, pos + 24);
            const uint16_t name_len = rd16(data, pos + 28);
            const uint16_t extra_len = rd16(data, pos + 30);
            const uint16_t comment_len = rd16(data, pos + 32);
            const uint16_t disk_start = rd16(data, pos + 34);
            const uint32_t local_offset = rd32(data, pos + 42);
            if (name_len == 0 || name_len > kMaxEntryName || !has_bytes(data, pos + 46u, name_len)) break;
            const std::string raw_name = data.substr(pos + 46, name_len);
            const uint64_t next = static_cast<uint64_t>(pos) + 46u + name_len + extra_len + comment_len;
            if (next > data.size()) break;
            pos = static_cast<size_t>(next);

            if ((flags & 0x0001u) != 0) continue; // encrypted
            if (method != 0 && method != 8) continue; // only stored/deflate
            if (comp_size == 0xFFFFFFFFu || uncomp_size == 0xFFFFFFFFu || local_offset == 0xFFFFFFFFu) {
                continue; // ZIP64 entry
            }
            if (disk_start != 0) continue;
            if (raw_name.back() == '/') continue; // directory entry
            const std::string name = normalize_path(raw_name);
            if (name.empty()) continue;
            if (uncomp_size > kMaxChapterBytes) continue; // never opened by this reader
            if (uncompressed_total + uncomp_size > kMaxUncompressedBytes) continue; // zip bomb guard
            if (!has_bytes(data, local_offset, 30u) ) continue;
            if (rd32(data, local_offset) != 0x04034b50u) continue;
            const uint16_t local_name_len = rd16(data, local_offset + 26);
            const uint16_t local_extra_len = rd16(data, local_offset + 28);
            const uint64_t data_offset = static_cast<uint64_t>(local_offset) + 30u + local_name_len + local_extra_len;
            if (data_offset + comp_size > data.size()) continue;

            uncompressed_total += uncomp_size;
            ZipEntry entry;
            entry.name = name;
            entry.method = method;
            entry.flags = flags;
            entry.comp_size = comp_size;
            entry.uncomp_size = uncomp_size;
            entry.local_offset = local_offset;
            _exact.emplace(entry.name, entries.size());
            _folded.emplace(to_lower(entry.name), entries.size());
            entries.push_back(std::move(entry));
        }

        if (entries.empty()) {
            if (error) *error = "epub: no usable entries in ZIP archive";
            return false;
        }
        return true;
    }

    bool find(const std::string& path, size_t* index) const {
        const std::string key = normalize_path(path);
        if (key.empty()) return false;
        auto exact = _exact.find(key);
        if (exact != _exact.end()) {
            *index = exact->second;
            return true;
        }
        auto folded = _folded.find(to_lower(key));
        if (folded != _folded.end()) {
            *index = folded->second;
            return true;
        }
        return false;
    }

    bool extract(size_t index, std::string* out, std::string* error) const {
        if (index >= entries.size() || out == nullptr) {
            if (error) *error = "invalid ZIP entry";
            return false;
        }
        const ZipEntry& entry = entries[index];
        if (entry.uncomp_size > kMaxChapterBytes) {
            if (error) *error = "entry is too large";
            return false;
        }
        if (!has_bytes(data, entry.local_offset, 30u) || rd32(data, entry.local_offset) != 0x04034b50u) {
            if (error) *error = "bad local file header";
            return false;
        }
        const uint16_t name_len = rd16(data, entry.local_offset + 26);
        const uint16_t extra_len = rd16(data, entry.local_offset + 28);
        const uint64_t data_offset =
            static_cast<uint64_t>(entry.local_offset) + 30u + name_len + extra_len;
        if (data_offset + entry.comp_size > data.size()) {
            if (error) *error = "truncated ZIP entry";
            return false;
        }
        const char* src = data.data() + data_offset;
        out->clear();
        if (entry.method == 0) {
            if (entry.comp_size < entry.uncomp_size) {
                if (error) *error = "stored entry is truncated";
                return false;
            }
            out->assign(src, entry.uncomp_size);
            return true;
        }
        out->assign(entry.uncomp_size, '\0');
        if (entry.uncomp_size == 0) return true;
        const size_t produced =
            lgfx_tinfl_decompress_mem_to_mem(&(*out)[0], out->size(), src, entry.comp_size, 0);
        if (produced == static_cast<size_t>(-1) || produced != entry.uncomp_size) {
            if (error) *error = "deflate stream is corrupt";
            return false;
        }
        return true;
    }

  private:
    std::map<std::string, size_t> _exact;
    std::map<std::string, size_t> _folded;
};

// ---------------------------------------------------------------- markup layer

struct XmlToken {
    enum class Kind : uint8_t { Text, Start, End };
    Kind kind = Kind::Text;
    std::string qname; // lowercased qualified name
    std::string name;  // lowercased local name (after ':')
    bool self_closing = false;
    bool raw_text = false; // CDATA: entities must not be decoded
    std::vector<std::pair<std::string, std::string>> attrs;
    std::string text;
};

class MarkupScanner {
  public:
    explicit MarkupScanner(const std::string& text) : _text(text) {}

    bool next(XmlToken& out) {
        while (_pos < _text.size()) {
            out = XmlToken();
            if (_text[_pos] != '<') {
                const size_t lt = _text.find('<', _pos);
                const size_t end = lt == kNpos ? _text.size() : lt;
                out.kind = XmlToken::Kind::Text;
                out.text.assign(_text, _pos, end - _pos);
                _pos = end;
                return true;
            }
            if (_text.compare(_pos, 4, "<!--") == 0) {
                const size_t end = _text.find("-->", _pos + 4);
                _pos = end == kNpos ? _text.size() : end + 3;
                continue;
            }
            if (_text.compare(_pos, 9, "<![CDATA[") == 0) {
                const size_t begin = _pos + 9;
                const size_t end = _text.find("]]>", begin);
                out.kind = XmlToken::Kind::Text;
                out.text.assign(_text, begin, (end == kNpos ? _text.size() : end) - begin);
                out.raw_text = true;
                _pos = end == kNpos ? _text.size() : end + 3;
                return true;
            }
            const char next_char = _pos + 1 < _text.size() ? _text[_pos + 1] : '\0';
            if (next_char == '!' || next_char == '?') {
                const size_t end = _text.find('>', _pos + 2);
                _pos = end == kNpos ? _text.size() : end + 1;
                continue;
            }
            const bool closing = next_char == '/';
            size_t pos = _pos + (closing ? 2 : 1);
            if (pos >= _text.size() || !is_name_start(_text[pos])) {
                // A bare '<' in running text.
                out.kind = XmlToken::Kind::Text;
                out.text.assign(1, '<');
                ++_pos;
                return true;
            }
            const size_t name_begin = pos;
            while (pos < _text.size() && is_name_char(_text[pos])) ++pos;
            out.qname = to_lower(_text.substr(name_begin, pos - name_begin));
            const size_t colon = out.qname.find(':');
            out.name = colon == std::string::npos ? out.qname : out.qname.substr(colon + 1);
            out.kind = closing ? XmlToken::Kind::End : XmlToken::Kind::Start;
            if (closing) {
                const size_t gt = _text.find('>', pos);
                _pos = gt == kNpos ? _text.size() : gt + 1;
                return true;
            }

            bool closed = false;
            while (pos < _text.size() && !closed) {
                while (pos < _text.size() && is_space_char(_text[pos])) ++pos;
                if (pos >= _text.size()) break;
                if (_text[pos] == '>') {
                    _pos = pos + 1;
                    closed = true;
                    break;
                }
                if (_text[pos] == '/') {
                    if (pos + 1 < _text.size() && _text[pos + 1] == '>') {
                        out.self_closing = true;
                        _pos = pos + 2;
                        closed = true;
                        break;
                    }
                    ++pos;
                    continue;
                }
                const size_t attr_begin = pos;
                while (pos < _text.size() && !is_space_char(_text[pos]) && _text[pos] != '=' &&
                       _text[pos] != '>' && _text[pos] != '/') {
                    ++pos;
                }
                std::string attr_name = to_lower(_text.substr(attr_begin, pos - attr_begin));
                while (pos < _text.size() && is_space_char(_text[pos])) ++pos;
                std::string attr_value;
                if (pos < _text.size() && _text[pos] == '=') {
                    ++pos;
                    while (pos < _text.size() && is_space_char(_text[pos])) ++pos;
                    if (pos < _text.size() && (_text[pos] == '"' || _text[pos] == '\'')) {
                        const char quote = _text[pos++];
                        const size_t value_end = _text.find(quote, pos);
                        if (value_end == kNpos) {
                            attr_value.assign(_text, pos, _text.size() - pos);
                            pos = _text.size();
                        } else {
                            attr_value.assign(_text, pos, value_end - pos);
                            pos = value_end + 1;
                        }
                    } else {
                        const size_t value_begin = pos;
                        while (pos < _text.size() && !is_space_char(_text[pos]) && _text[pos] != '>') ++pos;
                        attr_value.assign(_text, value_begin, pos - value_begin);
                    }
                }
                if (!attr_name.empty()) {
                    out.attrs.emplace_back(std::move(attr_name), decode_entities(attr_value));
                }
            }
            if (!closed) {
                _pos = _text.size();
                return false;
            }
            return true;
        }
        return false;
    }

    // Skips the content and the closing tag of a raw-text element.
    void skip_to_close(const std::string& name) {
        const std::string needle = "</" + name;
        const size_t at = find_ci(_text, _pos, needle);
        if (at == kNpos) {
            _pos = _text.size();
            return;
        }
        const size_t gt = _text.find('>', at + needle.size());
        _pos = gt == kNpos ? _text.size() : gt + 1;
    }

  private:
    const std::string& _text;
    size_t _pos = 0;
};

const std::string* find_attr(const XmlToken& token, const char* name) {
    for (const auto& attr : token.attrs) {
        if (attr.first == name) return &attr.second;
    }
    return nullptr;
}

std::string collect_element_text(MarkupScanner& scanner, const std::string& name) {
    std::string text;
    int depth = 1;
    XmlToken token;
    while (scanner.next(token)) {
        if (token.kind == XmlToken::Kind::Text) {
            text += token.raw_text ? token.text : decode_entities(token.text);
            continue;
        }
        if (token.kind == XmlToken::Kind::End) {
            if (token.name == name && --depth == 0) break;
            continue;
        }
        if (token.name == name && !token.self_closing) ++depth;
    }
    return collapse_ws(text);
}

// ------------------------------------------------------------ XHTML -> blocks

struct ChapterContent {
    std::vector<Paragraph> paragraphs;
    uint64_t text_length = 0;
};

std::string decode_source_text(const std::string& raw) {
    const auto* data = reinterpret_cast<const uint8_t*>(raw.data());
    const size_t size = raw.size();
    const TextEncoding encoding = detect_encoding(data, size);
    if (encoding == TextEncoding::Utf8) {
        const size_t skip = utf8_skip_bom(data, size);
        return raw.substr(skip);
    }
    return to_utf8(data, size, encoding);
}

class XhtmlParser {
  public:
    ChapterContent parse(const std::string& html) {
        MarkupScanner scanner(html);
        XmlToken token;
        while (scanner.next(token)) {
            if (token.kind == XmlToken::Kind::Text) {
                append_text(token.text, token.raw_text);
                continue;
            }
            if (token.kind == XmlToken::Kind::End) {
                handle_end(token.name);
                continue;
            }
            // Start tag: skip embedded code/styles and the whole head.
            if (token.name == "script" || token.name == "style" || token.name == "head") {
                if (!token.self_closing) scanner.skip_to_close(token.name);
                continue;
            }
            // Structural tags that are meaningless when empty.
            if (token.self_closing && token.name != "br" && token.name != "img" && token.name != "hr") {
                continue;
            }
            handle_start(token);
        }
        flush();
        return std::move(_result);
    }

  private:
    ChapterContent _result;
    Paragraph _current;
    bool _open = false;
    bool _pending_space = false;
    Face _pending_space_face = Face::Regular;
    bool _in_pre = false;
    int _bold = 0;
    int _italic = 0;
    int _mono = 0;
    int _quote_depth = 0;
    struct ListState {
        bool ordered = false;
        int next = 1;
    };
    std::vector<ListState> _lists;

    bool has_content() const {
        for (const Run& run : _current.runs) {
            if (!run.text.empty()) return true;
        }
        return false;
    }

    Face current_face() const {
        const bool bold = _bold > 0;
        const bool italic = _italic > 0;
        if (bold && italic) return Face::BoldItalic;
        if (bold) return Face::Bold;
        if (italic) return Face::Italic;
        if (_mono > 0 && !_in_pre) return Face::Mono;
        return Face::Regular;
    }

    void start_block(ParaKind kind, uint8_t level = 0, std::string marker = std::string()) {
        if (kind == ParaKind::Text && _quote_depth > 0) kind = ParaKind::Quote;
        // <li><p>text</p></li> and <blockquote><p> should not split the block
        // that was just opened by the list item / quote container.
        if (kind == ParaKind::Text && _open && !has_content() &&
            (_current.kind == ParaKind::ListItem || _current.kind == ParaKind::Quote)) {
            return;
        }
        flush();
        _current = Paragraph();
        _current.kind = kind;
        _current.level = level;
        _current.marker = std::move(marker);
        _open = true;
        _pending_space = false;
    }

    void append_with_face(Face face, const char* data, size_t size) {
        if (size == 0) return;
        if (!_open) start_block(ParaKind::Text);
        if (_current.runs.empty() || _current.runs.back().style.face != face) {
            Run run;
            run.style.face = face;
            _current.runs.push_back(std::move(run));
        }
        _current.runs.back().text.append(data, size);
    }

    void append_bytes(const char* data, size_t size) { append_with_face(current_face(), data, size); }

    void force_space() {
        if (_open && has_content()) {
            _pending_space = true;
            _pending_space_face = current_face();
        }
    }

    void append_text(const std::string& raw, bool literal) {
        if (raw.empty()) return;
        const std::string decoded = literal ? raw : decode_entities(raw);
        for (size_t pos = 0; pos < decoded.size();) {
            size_t used = 1;
            const uint32_t codepoint = utf8_decode(decoded.data() + pos, decoded.size() - pos, &used);
            if (used == 0) used = 1;
            if (_in_pre) {
                if (codepoint == '\r') {
                    pos += used;
                    continue;
                }
                if (codepoint == '\n') {
                    flush();
                    start_block(ParaKind::Code);
                    pos += used;
                    continue;
                }
                if (!_open) start_block(ParaKind::Code);
                append_bytes(decoded.data() + pos, used);
            } else if (is_collapsible_space(codepoint)) {
                if (has_content()) {
                    // Remember the face active when the separator was seen, so a
                    // whitespace at a style boundary stays in the previous run.
                    _pending_space = true;
                    _pending_space_face = current_face();
                }
            } else {
                if (!_open) start_block(ParaKind::Text);
                if (_pending_space) {
                    append_with_face(_pending_space_face, " ", 1);
                    _pending_space = false;
                }
                append_bytes(decoded.data() + pos, used);
            }
            pos += used;
        }
    }

    void start_list_item() {
        int level = static_cast<int>(_lists.size());
        if (level < 1) level = 1;
        if (level > 3) level = 3;
        std::string marker = "\xE2\x80\xA2"; // bullet
        if (!_lists.empty() && _lists.back().ordered) {
            marker = std::to_string(_lists.back().next++);
            marker.push_back('.');
        }
        flush();
        start_block(ParaKind::ListItem, static_cast<uint8_t>(level), std::move(marker));
    }

    void emit_image() {
        flush();
        start_block(ParaKind::Text);
        append_bytes(kImageLabel, sizeof(kImageLabel) - 1);
        flush();
    }

    void handle_start(const XmlToken& token) {
        const std::string& name = token.name;
        if (name == "b" || name == "strong") {
            ++_bold;
            return;
        }
        if (name == "i" || name == "em") {
            ++_italic;
            return;
        }
        if (name == "code") {
            if (!_in_pre) ++_mono;
            return;
        }
        if (name == "br") {
            if (_in_pre) {
                flush();
                start_block(ParaKind::Code);
            } else {
                force_space();
            }
            return;
        }
        if (name == "img") {
            emit_image();
            return;
        }
        if (name == "hr") {
            flush();
            start_block(ParaKind::Separator);
            flush();
            return;
        }
        if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
            start_block(ParaKind::Heading, static_cast<uint8_t>(name[1] - '0'));
            return;
        }
        if (name == "pre") {
            flush();
            _in_pre = true;
            start_block(ParaKind::Code);
            return;
        }
        if (name == "blockquote") {
            ++_quote_depth;
            start_block(ParaKind::Quote);
            return;
        }
        if (name == "ul" || name == "ol") {
            ListState state;
            state.ordered = name == "ol";
            _lists.push_back(state);
            return;
        }
        if (name == "li") {
            start_list_item();
            return;
        }
        if (name == "p" || name == "div" || name == "section" || name == "article") {
            start_block(ParaKind::Text);
        }
    }

    void handle_end(const std::string& name) {
        if (name == "pre") {
            flush();
            _in_pre = false;
            return;
        }
        if (name == "code") {
            if (_mono > 0) --_mono;
            return;
        }
        if (name == "b" || name == "strong") {
            if (_bold > 0) --_bold;
            return;
        }
        if (name == "i" || name == "em") {
            if (_italic > 0) --_italic;
            return;
        }
        if (name == "blockquote") {
            flush();
            if (_quote_depth > 0) --_quote_depth;
            return;
        }
        if (name == "ul" || name == "ol") {
            if (!_lists.empty()) _lists.pop_back();
            flush();
            return;
        }
        if (name == "li" || name == "p" || name == "div" || name == "section" || name == "article" ||
            name == "tr" || name == "td" || name == "th" ||
            (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6')) {
            flush();
        }
    }

    void flush() {
        if (!_open) return;
        _pending_space = false;
        for (auto it = _current.runs.rbegin(); it != _current.runs.rend(); ++it) {
            std::string& text = it->text;
            size_t length = text.size();
            while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                                  text[length - 1] == '\n' || text[length - 1] == '\r')) {
                --length;
            }
            text.resize(length);
            if (!text.empty()) break;
        }
        _current.runs.erase(std::remove_if(_current.runs.begin(), _current.runs.end(),
                                           [](const Run& run) { return run.text.empty(); }),
                            _current.runs.end());

        const bool has_text = has_content();
        _open = false;
        if (!has_text) {
            if (_current.kind != ParaKind::Separator) {
                _current = Paragraph();
                return;
            }
            // Horizontal rules are kept as empty separator paragraphs but still
            // consume one newline in the chapter text, so offsets stay stable.
            _current.runs.clear();
        }

        uint64_t length = 0;
        for (const Run& run : _current.runs) length += run.text.size();
        _current.token = _result.text_length;
        _result.text_length += length + 1;
        _result.paragraphs.push_back(std::move(_current));
        _current = Paragraph();
    }
};

ChapterContent parse_xhtml(const std::string& html) {
    XhtmlParser parser;
    return parser.parse(html);
}

// --------------------------------------------------------------- OPF metadata

struct ManifestItem {
    std::string id;
    std::string href;
    std::string media_type;
};

struct PackageData {
    std::string title;
    std::string author;
    std::vector<ManifestItem> items;
    std::vector<std::string> spine;
};

bool parse_package(const std::string& xml, PackageData* out, std::string* error) {
    MarkupScanner scanner(xml);
    XmlToken token;
    bool in_metadata = false;
    while (scanner.next(token)) {
        if (token.kind == XmlToken::Kind::Text) continue;
        if (token.kind == XmlToken::Kind::End) {
            if (token.name == "metadata") in_metadata = false;
            continue;
        }
        if (token.name == "metadata") {
            in_metadata = !token.self_closing;
            continue;
        }
        if (in_metadata && (token.name == "title" || token.name == "creator") && !token.self_closing) {
            const std::string value = collect_element_text(scanner, token.name);
            if (token.name == "title" && out->title.empty()) {
                out->title = value;
            } else if (token.name == "creator" && out->author.empty()) {
                out->author = value;
            }
            continue;
        }
        if (token.name == "item") {
            const std::string* id = find_attr(token, "id");
            const std::string* href = find_attr(token, "href");
            const std::string* media_type = find_attr(token, "media-type");
            if (id != nullptr && href != nullptr && !id->empty() && !href->empty()) {
                ManifestItem item;
                item.id = *id;
                item.href = *href;
                item.media_type = media_type != nullptr ? to_lower(*media_type) : std::string();
                out->items.push_back(std::move(item));
            }
            continue;
        }
        if (token.name == "itemref") {
            const std::string* idref = find_attr(token, "idref");
            if (idref != nullptr && !idref->empty()) out->spine.push_back(*idref);
        }
    }
    if (out->spine.empty()) {
        if (error) *error = "epub: OPF spine is empty";
        return false;
    }
    return true;
}

bool find_rootfile(const std::string& xml, std::string* rootfile) {
    MarkupScanner scanner(xml);
    XmlToken token;
    while (scanner.next(token)) {
        if (token.kind != XmlToken::Kind::Start || token.name != "rootfile") continue;
        const std::string* full_path = find_attr(token, "full-path");
        if (full_path != nullptr && !full_path->empty()) {
            *rootfile = *full_path;
            return true;
        }
    }
    return false;
}

bool suffix_ci(const std::string& text, const char* suffix) {
    const size_t length = std::char_traits<char>::length(suffix);
    if (text.size() < length) return false;
    const size_t offset = text.size() - length;
    for (size_t i = 0; i < length; ++i) {
        if (lower_ascii(text[offset + i]) != lower_ascii(suffix[i])) return false;
    }
    return true;
}

bool is_html_item(const ManifestItem& item) {
    const std::string& type = item.media_type;
    if (!type.empty()) {
        return type == "application/xhtml+xml" || type == "application/xhtml" ||
               type == "text/xhtml" || type == "text/html" || type == "application/html";
    }
    return suffix_ci(item.href, ".xhtml") || suffix_ci(item.href, ".html") ||
           suffix_ci(item.href, ".htm");
}

// ------------------------------------------------------------------- document

class EpubDocument final : public Document {
  public:
    EpubDocument(EpubArchive archive, std::vector<size_t> spine, DocInfo info)
        : _archive(std::move(archive)), _spine(std::move(spine)), _info(std::move(info)),
          _lengths(_spine.size(), kUnknownLength) {}

    const DocInfo& info() const override { return _info; }

    bool rewind() override {
        _chapter = 0;
        _index = 0;
        _loaded_chapter = kNpos;
        _paragraphs.clear();
        _chapter_length = 0;
        if (_spine.empty()) return false;
        if (load_chapter(0)) return true;
        // Tolerate a broken first document: start at the first readable chapter.
        for (size_t i = 1; i < _spine.size(); ++i) {
            if (load_chapter(i)) {
                _chapter = i;
                _index = 0;
                return true;
            }
        }
        return false;
    }

    bool next(Paragraph& out) override {
        for (size_t guard = 0; guard <= _spine.size(); ++guard) {
            if (_chapter >= _spine.size()) return false;
            if (_loaded_chapter != _chapter) {
                if (!load_chapter(_chapter)) {
                    ++_chapter;
                    _index = 0;
                    continue;
                }
            }
            if (_index < _paragraphs.size()) {
                out = _paragraphs[_index++];
                return true;
            }
            ++_chapter;
            _index = 0;
        }
        return false;
    }

    bool seek(uint64_t token) override {
        if (_spine.empty()) return false;
        size_t spine = static_cast<size_t>(token >> 32);
        const uint32_t offset = static_cast<uint32_t>(token & 0xFFFFFFFFull);
        if (spine >= _spine.size()) {
            // Position just past the document: next() reports the end.
            _chapter = _spine.size();
            _index = 0;
            _paragraphs.clear();
            _loaded_chapter = kNpos;
            _chapter_length = 0;
            return true;
        }
        if (!load_chapter(spine)) return false;
        size_t index = _paragraphs.size();
        for (size_t i = 0; i < _paragraphs.size(); ++i) {
            const uint32_t start = static_cast<uint32_t>(_paragraphs[i].token & 0xFFFFFFFFull);
            if (start >= offset) {
                index = i;
                break;
            }
        }
        _chapter = spine;
        _index = index;
        return true;
    }

    uint32_t permille(uint64_t token) const override {
        if (_spine.empty()) return 0;
        const size_t spine = static_cast<size_t>(token >> 32);
        const uint32_t offset = static_cast<uint32_t>(token & 0xFFFFFFFFull);
        if (spine >= _spine.size()) return 1000;
        const uint64_t length = chapter_length(spine);
        const uint64_t within = length == 0 ? 0 : std::min<uint64_t>(offset, length);
        const uint64_t fraction = length == 0 ? 0 : (within * 1000u) / length;
        uint64_t value = (static_cast<uint64_t>(spine) * 1000u + fraction) / _spine.size();
        if (value > 1000) value = 1000;
        return static_cast<uint32_t>(value);
    }

    uint64_t total_chars() const override {
        uint64_t total = 0;
        for (size_t i = 0; i < _spine.size(); ++i) total += chapter_length(i);
        return total;
    }

  private:
    bool load_chapter(size_t index) {
        if (index >= _spine.size()) return false;
        if (_loaded_chapter == index) return true;

        std::string bytes;
        std::string error;
        if (!_archive.extract(_spine[index], &bytes, &error)) return false;
        ChapterContent content = parse_xhtml(decode_source_text(bytes));
        for (Paragraph& paragraph : content.paragraphs) {
            paragraph.token |= static_cast<uint64_t>(index) << 32;
        }
        _paragraphs = std::move(content.paragraphs);
        _chapter_length = content.text_length;
        _lengths[index] = content.text_length;
        _loaded_chapter = index;
        return true;
    }

    uint64_t chapter_length(size_t index) const {
        if (index >= _spine.size()) return 0;
        if (_lengths[index] != kUnknownLength) return _lengths[index];
        std::string bytes;
        std::string error;
        uint64_t length = 0;
        if (_archive.extract(_spine[index], &bytes, &error)) {
            const ChapterContent content = parse_xhtml(decode_source_text(bytes));
            length = content.text_length;
        }
        _lengths[index] = length;
        return length;
    }

    EpubArchive _archive;
    std::vector<size_t> _spine;
    DocInfo _info;

    size_t _chapter = 0;
    size_t _index = 0;
    size_t _loaded_chapter = kNpos;
    std::vector<Paragraph> _paragraphs;
    uint64_t _chapter_length = 0;

    mutable std::vector<uint64_t> _lengths; // cached chapter text lengths, per spine item
};

} // namespace

// ---------------------------------------------------------------------- opener

std::unique_ptr<Document> open_epub(std::unique_ptr<ByteSource> source, std::string* error) {
    auto fail = [error](const std::string& message) -> std::unique_ptr<Document> {
        if (error != nullptr) *error = message;
        return nullptr;
    };

    if (!source) return fail("epub: no byte source");
    if (source->size() > kMaxArchiveBytes) return fail("epub: archive is too large (8 MiB limit)");

    std::string archive_bytes;
    if (!read_all(*source, kMaxArchiveBytes, &archive_bytes, error)) {
        if (error != nullptr && error->empty()) *error = "epub: cannot read archive";
        return nullptr;
    }

    EpubArchive archive;
    archive.data = std::move(archive_bytes);
    std::string archive_error;
    if (!archive.parse(&archive_error)) {
        return fail(archive_error.empty() ? "epub: corrupt ZIP archive" : archive_error);
    }

    // 1) META-INF/container.xml -> rootfile; 2) fallback: first *.opf in the ZIP.
    std::string rootfile;
    size_t index = 0;
    if (archive.find("META-INF/container.xml", &index)) {
        std::string container_xml;
        std::string extract_error;
        if (archive.extract(index, &container_xml, &extract_error)) {
            find_rootfile(decode_source_text(container_xml), &rootfile);
        }
    }
    if (!rootfile.empty()) rootfile = normalize_path(percent_decode(rootfile));
    if (rootfile.empty()) {
        for (const ZipEntry& entry : archive.entries) {
            if (suffix_ci(entry.name, ".opf")) {
                rootfile = entry.name;
                break;
            }
        }
    }
    if (rootfile.empty() || !archive.find(rootfile, &index)) {
        return fail("epub: package document (OPF) not found");
    }

    std::string opf_xml;
    {
        std::string extract_error;
        if (!archive.extract(index, &opf_xml, &extract_error)) {
            return fail("epub: cannot read package document: " + extract_error);
        }
    }

    PackageData package;
    std::string package_error;
    if (!parse_package(decode_source_text(opf_xml), &package, &package_error)) {
        return fail(package_error.empty() ? "epub: malformed package document" : package_error);
    }

    std::map<std::string, size_t> items_by_id;
    for (size_t i = 0; i < package.items.size(); ++i) {
        items_by_id.emplace(package.items[i].id, i);
    }

    const std::string base_dir = dirname_of(rootfile);
    std::vector<size_t> spine;
    for (const std::string& idref : package.spine) {
        auto item = items_by_id.find(idref);
        if (item == items_by_id.end()) continue;
        const ManifestItem& manifest_item = package.items[item->second];
        if (!is_html_item(manifest_item)) continue;
        const std::string path = resolve_href(base_dir, manifest_item.href);
        if (path.empty()) continue;
        size_t entry_index = 0;
        if (!archive.find(path, &entry_index)) continue;
        spine.push_back(entry_index);
    }
    if (spine.empty()) return fail("epub: no readable XHTML/HTML documents in the spine");

    DocInfo info;
    info.title = package.title;
    info.author = package.author;

    auto document = std::make_unique<EpubDocument>(std::move(archive), std::move(spine), std::move(info));
    if (!document->rewind()) return fail("epub: no readable chapters");
    return document;
}

} // namespace reader
