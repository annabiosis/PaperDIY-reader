// Document abstraction: sequential paragraph stream with seekable positions.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/byte_source.h"
#include "text/types.h"

namespace reader {

class Document {
  public:
    virtual ~Document() = default;

    virtual const DocInfo& info() const = 0;

    // Restarts iteration from the beginning.
    virtual bool rewind() = 0;

    // Produces the next paragraph; false at end of document.
    virtual bool next(Paragraph& out) = 0;

    // Positions the stream so the next paragraph is the one starting at `token`.
    virtual bool seek(uint64_t token) = 0;

    // Coarse progress for a token, 0..1000.
    virtual uint32_t permille(uint64_t token) const = 0;

    // Hint for the UI ("12%" estimate); 0 when unknown.
    virtual uint64_t total_chars() const { return 0; }

    // Footnote table: notes are numbered from 1, index 0 is unused. Documents that
    // have no notes return 0 from note_count().
    virtual size_t note_count() const { return 0; }
    // Body text of a note (index 0 = first note).
    virtual std::string note(size_t index) const {
        (void)index;
        return std::string();
    }
    // Optional heading of a note (FB2 keeps it in <title>); empty when absent.
    virtual std::string note_title(size_t index) const {
        (void)index;
        return std::string();
    }
};

enum class Format : uint8_t { Unknown, Txt, Markdown, Fb2, Epub, Pdf };

Format format_from_name(const std::string& path);
const char* format_name(Format format);

// Factory. `source` is consumed; error is filled when nullptr is returned.
std::unique_ptr<Document> open_document(Format format, std::unique_ptr<ByteSource> source,
                                        std::string* error);

} // namespace reader
