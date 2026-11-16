// Plain text document: encoding detection, paragraph splitting, seekable tokens.
#include "core/byte_source.h"
#include "core/encoding.h"
#include "core/utf8.h"
#include "formats/formats.h"
#include "text/doc.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace reader {
namespace {

constexpr size_t kMaxSourceBytes = 8 * 1024 * 1024;
constexpr size_t kMaxDecodedBytes = 6 * 1024 * 1024;

struct TxtEntry {
    uint64_t token = 0;
    uint32_t offset = 0; // byte offset inside the paragraph text
    uint32_t length = 0;
    Paragraph paragraph;
};

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

class TxtDocument : public Document {
  public:
    TxtDocument(std::string text, DocInfo info) : _text(std::move(text)), _info(std::move(info)) {}

    bool build(std::string* error) {
        if (_text.size() > kMaxDecodedBytes) {
            if (error) *error = "file is too large";
            return false;
        }

        bool has_blank_line = false;
        for (size_t i = 0; i + 1 < _text.size(); ++i) {
            if (_text[i] == '\n' && _text[i + 1] == '\n') {
                has_blank_line = true;
                break;
            }
        }
        // Windows line endings: check \r\n\r\n as well.
        if (!has_blank_line) {
            for (size_t i = 0; i + 3 < _text.size(); ++i) {
                if (_text.compare(i, 4, "\r\n\r\n") == 0) {
                    has_blank_line = true;
                    break;
                }
            }
        }

        size_t pos = 0;
        std::string buffer;
        uint64_t buffer_offset = 0;

        auto flush = [&]() {
            std::string normalized;
            normalized.reserve(buffer.size());
            bool pending_space = false;
            for (char c : buffer) {
                const unsigned char byte = static_cast<unsigned char>(c);
                if (is_space(c) || (byte < 0x20)) {
                    pending_space = !normalized.empty();
                    continue;
                }
                if (pending_space) {
                    normalized.push_back(' ');
                    pending_space = false;
                }
                normalized.push_back(c);
            }
            if (!normalized.empty()) {
                Paragraph paragraph;
                paragraph.kind = ParaKind::Text;
                Run run;
                run.style.face = Face::Regular;
                run.text = std::move(normalized);
                paragraph.runs.push_back(std::move(run));
                paragraph.token = buffer_offset;
                TxtEntry entry;
                entry.token = buffer_offset;
                entry.offset = 0;
                entry.length = 0;
                entry.paragraph = std::move(paragraph);
                _entries.push_back(std::move(entry));
            }
            buffer.clear();
        };

        while (pos < _text.size()) {
            size_t line_end = _text.find('\n', pos);
            if (line_end == std::string::npos) line_end = _text.size();
            const size_t length = line_end - pos;
            bool blank = true;
            for (size_t i = pos; i < line_end; ++i) {
                if (!is_space(_text[i])) {
                    blank = false;
                    break;
                }
            }
            if (blank) {
                flush();
                buffer_offset = line_end + 1;
            } else {
                if (buffer.empty()) buffer_offset = pos;
                else buffer.push_back(' ');
                buffer.append(_text, pos, length);
            }
            pos = line_end + 1;
        }
        flush();

        // Books without blank lines: fall back to one paragraph per non-empty line.
        if (!has_blank_line && _entries.size() <= 1 && !_text.empty()) {
            _entries.clear();
            size_t line_start = 0;
            while (line_start < _text.size()) {
                size_t line_end = _text.find('\n', line_start);
                if (line_end == std::string::npos) line_end = _text.size();
                std::string normalized;
                for (size_t i = line_start; i < line_end; ++i) {
                    if (!is_space(_text[i])) {
                        normalized.append(_text, i, line_end - i);
                        break;
                    }
                }
                size_t last = normalized.size();
                while (last > 0 && is_space(normalized[last - 1])) --last;
                normalized.resize(last);
                if (!normalized.empty()) {
                    Paragraph paragraph;
                    paragraph.kind = ParaKind::Text;
                    Run run;
                    run.style.face = Face::Regular;
                    run.text = std::move(normalized);
                    paragraph.runs.push_back(std::move(run));
                    paragraph.token = line_start;
                    TxtEntry entry;
                    entry.token = line_start;
                    entry.paragraph = std::move(paragraph);
                    _entries.push_back(std::move(entry));
                }
                line_start = line_end + 1;
            }
        }

        if (_entries.empty()) {
            if (error) *error = "file contains no text";
            return false;
        }
        _cursor = 0;
        _info.title = _info.title.empty() ? "" : _info.title;
        return true;
    }

    const DocInfo& info() const override { return _info; }

    bool rewind() override {
        _cursor = 0;
        return true;
    }

    bool next(Paragraph& out) override {
        if (_cursor >= _entries.size()) return false;
        out = _entries[_cursor].paragraph;
        ++_cursor;
        return true;
    }

    bool seek(uint64_t token) override {
        size_t low = 0;
        size_t high = _entries.size();
        while (low < high) {
            const size_t mid = (low + high) / 2;
            if (_entries[mid].token < token) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        _cursor = low < _entries.size() ? low : _entries.size();
        return _cursor < _entries.size();
    }

    uint32_t permille(uint64_t token) const override {
        if (_text.empty()) return 0;
        if (token >= _text.size()) return 1000;
        return static_cast<uint32_t>(token * 1000 / _text.size());
    }

    uint64_t total_chars() const override { return _text.size(); }

  private:
    std::string _text;
    DocInfo _info;
    std::vector<TxtEntry> _entries;
    size_t _cursor = 0;
};

} // namespace

std::unique_ptr<Document> open_txt(std::unique_ptr<ByteSource> source, std::string* error) {
    if (!source) {
        if (error) *error = "no source";
        return nullptr;
    }
    std::string raw;
    if (!read_all(*source, kMaxSourceBytes, &raw, error)) return nullptr;

    const auto* bytes = reinterpret_cast<const uint8_t*>(raw.data());
    const TextEncoding encoding = detect_encoding(bytes, std::min<size_t>(raw.size(), 4096));
    std::string text = to_utf8(bytes, raw.size(), encoding);
    if (text.size() > kMaxDecodedBytes) {
        if (error) *error = "file is too large after decoding";
        return nullptr;
    }

    // Drop stray C0 control bytes (except line breaks and tabs) so binary
    // content does not turn into a "book" of unprintable characters.
    {
        std::string cleaned;
        cleaned.reserve(text.size());
        for (char c : text) {
            const unsigned char byte = static_cast<unsigned char>(c);
            if (byte < 0x20 && byte != '\n' && byte != '\r' && byte != '\t' && byte != '\f') continue;
            cleaned.push_back(c);
        }
        text.swap(cleaned);
    }

    DocInfo info;
    auto document = std::make_unique<TxtDocument>(std::move(text), std::move(info));
    if (!document->build(error)) return nullptr;
    return document;
}

} // namespace reader
