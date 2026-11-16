#include "formats/formats.h"

#include <algorithm>
#include <cctype>

namespace reader {

Format format_from_name(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return Format::Unknown;
    std::string extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == "txt") return Format::Txt;
    if (extension == "md" || extension == "markdown") return Format::Markdown;
    if (extension == "fb2") return Format::Fb2;
    if (extension == "epub") return Format::Epub;
    if (extension == "pdf") return Format::Pdf;
    return Format::Unknown;
}

const char* format_name(Format format) {
    switch (format) {
    case Format::Txt:
        return "TXT";
    case Format::Markdown:
        return "MD";
    case Format::Fb2:
        return "FB2";
    case Format::Epub:
        return "EPUB";
    case Format::Pdf:
        return "PDF";
    default:
        return "?";
    }
}

std::unique_ptr<Document> open_document(Format format, std::unique_ptr<ByteSource> source, std::string* error) {
    switch (format) {
    case Format::Txt:
        return open_txt(std::move(source), error);
    case Format::Markdown:
        return open_markdown(std::move(source), error);
    case Format::Fb2:
        return open_fb2(std::move(source), error);
    case Format::Epub:
        return open_epub(std::move(source), error);
    case Format::Pdf:
        return open_pdf(std::move(source), error);
    default:
        if (error) *error = "unsupported format";
        return nullptr;
    }
}

} // namespace reader
