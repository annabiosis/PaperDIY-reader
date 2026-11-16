#include "store/progress.h"

#include <sys/stat.h>

#include <cJSON.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace reader {
namespace {

constexpr char kReaderDir[] = "/sdcard/.reader";
constexpr char kProgressPath[] = "/sdcard/.reader/progress.json";
constexpr char kProgressTmp[] = "/sdcard/.reader/progress.tmp";
constexpr size_t kMaxEntries = 200;

bool read_file(const char* path, std::string& out) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) {
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t got = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    out.resize(got);
    return got > 0;
}

bool write_file(const char* path, const std::string& data) {
    FILE* file = std::fopen(path, "wb");
    if (file == nullptr) return false;
    const size_t written = std::fwrite(data.data(), 1, data.size(), file);
    std::fflush(file);
    std::fclose(file);
    return written == data.size();
}

std::string json_string(const char* value) {
    std::string out;
    out.push_back('"');
    for (const char* p = value; p != nullptr && *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

} // namespace

ProgressStore& progress_store() {
    static ProgressStore store;
    return store;
}

bool ProgressStore::load() {
    mkdir(kReaderDir, 0777);
    _entries.clear();
    _last_path.clear();
    _font_size = 1;
    _next_order = 1;

    std::string text;
    if (read_file(kProgressPath, text)) {
        cJSON* root = cJSON_Parse(text.c_str());
        if (root != nullptr) {
            const cJSON* reading = cJSON_GetObjectItemCaseSensitive(root, "reading");
    if (cJSON_IsBool(reading)) _reading = cJSON_IsTrue(reading);
    const cJSON* font = cJSON_GetObjectItemCaseSensitive(root, "font_size");
            if (cJSON_IsNumber(font)) {
                const int value = font->valueint;
                _font_size = value < 0 ? 0 : (value > 2 ? 2 : value);
            }
            const cJSON* last = cJSON_GetObjectItemCaseSensitive(root, "last");
            if (cJSON_IsString(last) && last->valuestring != nullptr) {
                _last_path = last->valuestring;
            }
            const cJSON* next_order = cJSON_GetObjectItemCaseSensitive(root, "next_order");
            if (cJSON_IsNumber(next_order) && next_order->valueint > 0) {
                _next_order = static_cast<uint32_t>(next_order->valueint);
            }
            const cJSON* entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
            if (cJSON_IsArray(entries)) {
                const cJSON* item = nullptr;
                cJSON_ArrayForEach(item, entries) {
                    const cJSON* path = cJSON_GetObjectItemCaseSensitive(item, "path");
                    if (!cJSON_IsString(path) || path->valuestring == nullptr) continue;
                    ProgressEntry entry;
                    entry.path = path->valuestring;
                    const cJSON* token = cJSON_GetObjectItemCaseSensitive(item, "token");
                    const cJSON* offset = cJSON_GetObjectItemCaseSensitive(item, "offset");
                    const cJSON* permille = cJSON_GetObjectItemCaseSensitive(item, "permille");
                    const cJSON* order = cJSON_GetObjectItemCaseSensitive(item, "order");
                    entry.token = cJSON_IsNumber(token) ? static_cast<uint64_t>(token->valuedouble) : 0;
                    entry.char_offset = cJSON_IsNumber(offset) ? static_cast<uint32_t>(offset->valueint) : 0;
                    entry.permille = cJSON_IsNumber(permille) ? static_cast<uint32_t>(permille->valueint) : 0;
                    entry.order = cJSON_IsNumber(order) ? static_cast<uint32_t>(order->valueint) : 0;
                    const cJSON* size = cJSON_GetObjectItemCaseSensitive(item, "size");
                    entry.file_size = cJSON_IsNumber(size) ? static_cast<uint64_t>(size->valuedouble) : 0;
                    const cJSON* page = cJSON_GetObjectItemCaseSensitive(item, "page");
                    entry.page = cJSON_IsNumber(page) ? page->valueint : 0;
                    const cJSON* layout = cJSON_GetObjectItemCaseSensitive(item, "layout");
                    entry.layout = cJSON_IsNumber(layout) ? static_cast<uint32_t>(layout->valuedouble) : 0;
                    _entries.push_back(std::move(entry));
                }
            }
            cJSON_Delete(root);
        }
    }

    if (_last_path.empty() && !_entries.empty()) {
        const ProgressEntry* newest = &_entries.front();
        for (const auto& entry : _entries) {
            if (entry.order > newest->order) newest = &entry;
        }
        _last_path = newest->path;
    }
    return true;
}

bool ProgressStore::save() const {
    mkdir(kReaderDir, 0777);

    std::string out;
    out.reserve(8192);
    out += "{\n";
    out += "  \"version\": 1,\n";
    out += "  \"font_size\": " + std::to_string(_font_size) + ",\n";
    out += std::string("  \"reading\": ") + (_reading ? "true" : "false") + ",\n";
    out += "  \"next_order\": " + std::to_string(_next_order) + ",\n";
    out += "  \"last\": " + json_string(_last_path.c_str()) + ",\n";
    out += "  \"entries\": [\n";
    for (size_t i = 0; i < _entries.size(); ++i) {
        const ProgressEntry& entry = _entries[i];
        out += "    {\"path\": " + json_string(entry.path.c_str()) + ", \"token\": " +
               std::to_string(static_cast<unsigned long long>(entry.token)) +
               ", \"offset\": " + std::to_string(entry.char_offset) +
               ", \"permille\": " + std::to_string(entry.permille) +
               ", \"order\": " + std::to_string(entry.order) +
               ", \"size\": " + std::to_string(static_cast<unsigned long long>(entry.file_size)) +
               ", \"page\": " + std::to_string(entry.page) +
               ", \"layout\": " + std::to_string(entry.layout) + "}";
        out += i + 1 < _entries.size() ? ",\n" : "\n";
    }
    out += "  ]\n}\n";

    if (!write_file(kProgressTmp, out)) return false;
    std::remove(kProgressPath);
    if (std::rename(kProgressTmp, kProgressPath) != 0) {
        // FATFS rename can fail when the target exists; fall back to direct write.
        return write_file(kProgressPath, out);
    }
    return true;
}

uint64_t ProgressStore::file_size_of(const std::string& path) {
    const std::string full = std::string("/sdcard/") + path;
    struct stat info = {};
    if (stat(full.c_str(), &info) != 0) return 0;
    return static_cast<uint64_t>(info.st_size);
}

void ProgressStore::update(const std::string& path, PagePos position, uint32_t permille, int page,
                           uint32_t layout) {
    const uint64_t size = file_size_of(path);
    for (auto& entry : _entries) {
        if (entry.path == path) {
            entry.token = position.token;
            entry.char_offset = position.char_offset;
            entry.permille = permille;
            entry.order = _next_order++;
            entry.page = page;
            entry.layout = layout;
            if (size != 0) entry.file_size = size;
            _last_path = path;
            return;
        }
    }
    if (_entries.size() >= kMaxEntries) {
        size_t oldest = 0;
        for (size_t i = 1; i < _entries.size(); ++i) {
            if (_entries[i].order < _entries[oldest].order) oldest = i;
        }
        _entries.erase(_entries.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    ProgressEntry entry;
    entry.path = path;
    entry.token = position.token;
    entry.char_offset = position.char_offset;
    entry.permille = permille;
    entry.order = _next_order++;
    entry.file_size = size;
    entry.page = page;
    entry.layout = layout;
    _entries.push_back(std::move(entry));
    _last_path = path;
}

void ProgressStore::forget(const std::string& path) {
    for (size_t i = 0; i < _entries.size(); ++i) {
        if (_entries[i].path != path) continue;
        if (_last_path == path) _last_path.clear();
        _entries.erase(_entries.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

const ProgressEntry* ProgressStore::find_valid(const std::string& path) const {
    const ProgressEntry* entry = find(path);
    if (entry == nullptr) return nullptr;
    // A file that changed on the card invalidates its saved position: the token
    // would point into different text and the book would open mid-sentence.
    const uint64_t size = file_size_of(path);
    if (entry->file_size != 0 && size != 0 && entry->file_size != size) return nullptr;
    return entry;
}

const ProgressEntry* ProgressStore::find(const std::string& path) const {
    for (const auto& entry : _entries) {
        if (entry.path == path) return &entry;
    }
    return nullptr;
}

std::string ProgressStore::last_path() const {
    return _last_path;
}

void ProgressStore::set_font_size(int size) {
    if (size < 0) size = 0;
    if (size > 2) size = 2;
    _font_size = size;
}

} // namespace reader
