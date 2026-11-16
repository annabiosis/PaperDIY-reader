// Format dispatch and per-format factories. Each parser only depends on
// ByteSource / Document and the text model, so it builds for host tests too.
#pragma once

#include <memory>
#include <string>

#include "core/byte_source.h"
#include "text/doc.h"

namespace reader {

std::unique_ptr<Document> open_txt(std::unique_ptr<ByteSource> source, std::string* error);
std::unique_ptr<Document> open_markdown(std::unique_ptr<ByteSource> source, std::string* error);
std::unique_ptr<Document> open_fb2(std::unique_ptr<ByteSource> source, std::string* error);
std::unique_ptr<Document> open_epub(std::unique_ptr<ByteSource> source, std::string* error);
std::unique_ptr<Document> open_pdf(std::unique_ptr<ByteSource> source, std::string* error);

} // namespace reader
