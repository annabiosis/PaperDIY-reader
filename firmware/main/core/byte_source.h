// Random-access byte source over a file or memory buffer.
// Parsers only rely on this interface, so they build and run on the host.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace reader {

class ByteSource {
  public:
    virtual ~ByteSource() = default;

    virtual uint64_t size() const = 0;

    // Reads up to `len` bytes at `offset`; returns bytes read (0 on EOF/error).
    virtual size_t read(uint64_t offset, void* dst, size_t len) = 0;

    virtual bool ok() const = 0;
};

class MemoryByteSource : public ByteSource {
  public:
    MemoryByteSource(std::string data) : _data(std::move(data)) {}

    uint64_t size() const override { return _data.size(); }
    size_t read(uint64_t offset, void* dst, size_t len) override;
    bool ok() const override { return true; }

    const std::string& data() const { return _data; }

  private:
    std::string _data;
};

// Reads a whole file into PSRAM/heap. Returns nullptr on error.
std::unique_ptr<ByteSource> open_file_source(const std::string& path, std::string* error);

// Reads up to `max_size` bytes from a source into a string. Fails when larger.
bool read_all(ByteSource& source, size_t max_size, std::string* out, std::string* error);

// Convenience: reads a small range as a string.
std::string read_range(ByteSource& source, uint64_t offset, size_t len);

} // namespace reader
