#include "core/byte_source.h"

#include <cstdio>
#include <cstring>

namespace reader {

size_t MemoryByteSource::read(uint64_t offset, void* dst, size_t len) {
    if (offset >= _data.size() || len == 0) return 0;
    const size_t available = _data.size() - static_cast<size_t>(offset);
    const size_t count = len < available ? len : available;
    std::memcpy(dst, _data.data() + offset, count);
    return count;
}

namespace {

class FileByteSource : public ByteSource {
  public:
    explicit FileByteSource(FILE* file) : _file(file) {
        if (std::fseek(_file, 0, SEEK_END) == 0) {
            const long end = std::ftell(_file);
            _size = end > 0 ? static_cast<uint64_t>(end) : 0;
        }
        std::fseek(_file, 0, SEEK_SET);
    }
    ~FileByteSource() override {
        if (_file) std::fclose(_file);
    }

    uint64_t size() const override { return _size; }

    size_t read(uint64_t offset, void* dst, size_t len) override {
        if (_file == nullptr || len == 0) return 0;
        if (std::fseek(_file, static_cast<long>(offset), SEEK_SET) != 0) {
            _ok = false;
            return 0;
        }
        const size_t got = std::fread(dst, 1, len, _file);
        if (got == 0 && std::ferror(_file)) _ok = false;
        return got;
    }

    bool ok() const override { return _ok; }

  private:
    FILE* _file = nullptr;
    uint64_t _size = 0;
    bool _ok = true;
};

} // namespace

std::unique_ptr<ByteSource> open_file_source(const std::string& path, std::string* error) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        if (error) *error = "cannot open file";
        return nullptr;
    }
    auto source = std::make_unique<FileByteSource>(file);
    if (source->size() == 0) {
        if (error) *error = "file is empty";
        return nullptr;
    }
    return source;
}

bool read_all(ByteSource& source, size_t max_size, std::string* out, std::string* error) {
    const uint64_t size = source.size();
    if (size > max_size) {
        if (error) *error = "file is too large";
        return false;
    }
    out->resize(static_cast<size_t>(size));
    if (size == 0) return true;
    const size_t got = source.read(0, out->data(), static_cast<size_t>(size));
    if (got != size) {
        if (error) *error = "read failed";
        return false;
    }
    return true;
}

std::string read_range(ByteSource& source, uint64_t offset, size_t len) {
    std::string out;
    out.resize(len);
    const size_t got = source.read(offset, out.data(), len);
    out.resize(got);
    return out;
}

} // namespace reader
