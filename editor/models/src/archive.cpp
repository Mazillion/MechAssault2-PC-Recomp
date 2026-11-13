// MGF v4 archive reader (MechAssault 2 retail format).
//
// Container: either a plain archive ('mgf ' at offset 0) or a compressed one
// whose header word 1 is 0xC5735A0F, followed by a table of independently
// zlib-compressed blocks (start, length, checksum) that together form the
// plain archive. The plain archive header holds file records (32 bytes),
// directory rows (24 bytes: parent, name offset, file index) and a string table.
#include "internal.h"

#include <windows.h>

#include <algorithm>
#include <list>
#include <mutex>
#include <unordered_map>

namespace ma2m {

namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string KeyOf(const std::string& path) { return detail::Lower(NormalizePath(path)); }

struct FileEntry {
    std::string path;     // as spelled in the archive
    uint32_t offset = 0, length = 0;
};

class Archive {
public:
    std::string path, name;
    std::vector<FileEntry> files;
    std::unordered_map<std::string, uint32_t> index;  // lower-case path -> files[]

    ~Archive() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }

    bool Open(const std::string& p, std::string* error) {
        path = p;
        name = detail::FileName(NormalizePath(p));
        h_ = CreateFileW(Widen(p).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return Fail(error, "cannot open " + p);
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h_, &sz) || sz.QuadPart < 44 || sz.QuadPart > 0x7FFFFFFF) return Fail(error, "bad file size: " + p);
        file_size_ = (uint32_t)sz.QuadPart;
        uint8_t head[32];
        if (!RawRead(0, head, 32)) return Fail(error, "cannot read header: " + p);
        uint32_t w[8];
        memcpy(w, head, 32);
        if (!memcmp(head, "mgf ", 4)) {
            logical_size_ = file_size_;
        } else if (w[1] == 0xC5735A0Fu) {
            uint32_t count = w[2];
            logical_size_ = w[3];
            block_size_ = w[5];
            if (!count || count > 1000000 || !block_size_ || block_size_ > (64u << 20) ||
                (uint64_t)count * block_size_ < logical_size_)
                return Fail(error, "invalid compressed archive header: " + p);
            chunks_.resize((size_t)count * 3);
            if (!RawRead(32, chunks_.data(), count * 12)) return Fail(error, "cannot read block table: " + p);
            compressed_ = true;
        } else {
            return Fail(error, "not an MGF archive: " + p);
        }
        uint32_t h[11];
        if (!Read(0, sizeof(h), h)) return Fail(error, "cannot read archive header: " + p);
        uint8_t version;
        if (h[0] != 0x2066676Du || !Read(4, 1, &version) || version != 4)
            return Fail(error, "not an MGF v4 archive: " + p);
        uint32_t nf = h[3], lf = h[4], of = h[5], nd = h[6], ld = h[7], od = h[8], ls = h[9], os = h[10];
        if (lf != nf * 32 || ld != nd * 24 || nf > 1000000 || nd > 1000000 || ls > (64u << 20))
            return Fail(error, "invalid archive directory sizes: " + p);
        std::vector<uint32_t> records((size_t)nf * 8);
        std::vector<int32_t> rows((size_t)nd * 6);
        std::vector<char> strings((size_t)ls + 1, 0);
        if ((nf && !Read(of, lf, records.data())) || (nd && !Read(od, ld, rows.data())) ||
            (ls && !Read(os, ls, strings.data())))
            return Fail(error, "cannot read archive directory: " + p);
        files.resize(nf);
        for (uint32_t i = 0; i < nf; i++) {
            files[i].length = records[(size_t)i * 8 + 3];
            files[i].offset = records[(size_t)i * 8 + 6];
        }
        auto row_name = [&](int r) -> const char* {
            uint32_t off = (uint32_t)rows[(size_t)r * 6 + 4];
            return off < ls ? strings.data() + off : "";
        };
        for (uint32_t r = 0; r < nd; r++) {
            int32_t fi = rows[(size_t)r * 6 + 5];
            if (fi < 0 || (uint32_t)fi >= nf) continue;
            std::vector<const char*> parts;
            int cur = (int)r, guard = 0;
            bool ok = true;
            while (cur >= 0) {
                if ((uint32_t)cur >= nd || ++guard > 256) {
                    ok = false;
                    break;
                }
                parts.push_back(row_name(cur));
                cur = rows[(size_t)cur * 6 + 1];
            }
            if (!ok) continue;
            std::string full;
            for (size_t k = parts.size(); k-- > 0;) {
                if (!*parts[k]) continue;
                if (!full.empty()) full += '/';
                full += parts[k];
            }
            full = NormalizePath(full);
            files[fi].path = full;
            index.emplace(detail::Lower(full), (uint32_t)fi);
        }
        // Drop records no directory row names.
        return true;
    }

    bool ReadEntry(uint32_t fi, std::vector<uint8_t>& out, std::string* error) {
        const FileEntry& f = files[fi];
        out.resize(f.length);
        if (f.length && !Read(f.offset, f.length, out.data())) {
            out.clear();
            if (error) *error = "read failed for " + f.path + " in " + name;
            return false;
        }
        return true;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint32_t file_size_ = 0, logical_size_ = 0, block_size_ = 0;
    bool compressed_ = false;
    std::vector<uint32_t> chunks_;
    std::mutex mu_;
    // Small LRU of decompressed blocks.
    struct Block {
        uint32_t number;
        std::vector<uint8_t> data;
    };
    std::list<Block> cache_;

    static bool Fail(std::string* error, const std::string& msg) {
        if (error) *error = msg;
        return false;
    }

    bool RawRead(uint32_t offset, void* dst, uint32_t size) {
        if ((uint64_t)offset + size > file_size_) return false;
        OVERLAPPED ov = {};
        ov.Offset = offset;
        DWORD got = 0;
        uint8_t* d = (uint8_t*)dst;
        while (size) {
            DWORD want = size > (1u << 30) ? (1u << 30) : size;
            if (!::ReadFile(h_, d, want, &got, &ov) || got == 0) return false;
            d += got;
            size -= got;
            offset += got;
            ov.Offset = offset;
        }
        return true;
    }

    const std::vector<uint8_t>* GetBlock(uint32_t number) {
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->number == number) {
                cache_.splice(cache_.begin(), cache_, it);
                return &cache_.front().data;
            }
        }
        if ((size_t)number * 3 + 2 >= chunks_.size()) return nullptr;
        uint32_t start = chunks_[(size_t)number * 3], length = chunks_[(size_t)number * 3 + 1];
        if (!length || length > (64u << 20)) return nullptr;
        std::vector<uint8_t> packed(length);
        if (!RawRead(start, packed.data(), length)) return nullptr;
        Block b;
        b.number = number;
        b.data.resize(block_size_);
        long got = detail::ZlibInflate(packed.data(), length, b.data.data(), block_size_);
        if (got <= 0) return nullptr;
        b.data.resize((size_t)got);
        cache_.push_front(std::move(b));
        if (cache_.size() > 24) cache_.pop_back();
        return &cache_.front().data;
    }

public:
    bool Read(uint32_t address, uint32_t size, void* dst) {
        if ((uint64_t)address + size > logical_size_) return false;
        std::lock_guard<std::mutex> lock(mu_);
        if (!compressed_) return RawRead(address, dst, size);
        uint8_t* d = (uint8_t*)dst;
        while (size) {
            uint32_t number = address / block_size_, offset = address % block_size_;
            const std::vector<uint8_t>* block = GetBlock(number);
            if (!block || offset >= block->size()) return false;
            uint32_t take = (uint32_t)std::min<size_t>(block->size() - offset, size);
            memcpy(d, block->data() + offset, take);
            d += take;
            address += take;
            size -= take;
        }
        return true;
    }
};

}  // namespace

struct ArchiveSet::Impl {
    std::vector<std::unique_ptr<Archive>> archives;
    struct Hit {
        uint32_t archive, file;
    };
    std::unordered_map<std::string, Hit> global;  // first archive wins
};

ArchiveSet::ArchiveSet() : impl_(new Impl) {}
ArchiveSet::~ArchiveSet() = default;

bool ArchiveSet::Add(const std::string& path_utf8, std::string* error) {
    std::unique_ptr<Archive> a(new Archive);
    if (!a->Open(path_utf8, error)) return false;
    uint32_t ai = (uint32_t)impl_->archives.size();
    for (const auto& kv : a->index) impl_->global.emplace(kv.first, Impl::Hit{ai, kv.second});
    impl_->archives.push_back(std::move(a));
    return true;
}

size_t ArchiveSet::Count() const { return impl_->archives.size(); }
std::string ArchiveSet::ArchivePath(size_t i) const { return i < Count() ? impl_->archives[i]->path : std::string(); }
std::string ArchiveSet::ArchiveName(size_t i) const { return i < Count() ? impl_->archives[i]->name : std::string(); }
size_t ArchiveSet::FileCount(size_t i) const { return i < Count() ? impl_->archives[i]->index.size() : 0; }

int ArchiveSet::Find(const std::string& path) const {
    auto it = impl_->global.find(KeyOf(path));
    return it == impl_->global.end() ? -1 : (int)it->second.archive;
}

bool ArchiveSet::ExistsIn(size_t ai, const std::string& path) const {
    return ai < Count() && impl_->archives[ai]->index.count(KeyOf(path)) != 0;
}

std::string ArchiveSet::CanonicalPath(const std::string& path) const {
    auto it = impl_->global.find(KeyOf(path));
    if (it == impl_->global.end()) return std::string();
    return impl_->archives[it->second.archive]->files[it->second.file].path;
}

uint32_t ArchiveSet::FileSize(const std::string& path) const {
    auto it = impl_->global.find(KeyOf(path));
    if (it == impl_->global.end()) return 0;
    return impl_->archives[it->second.archive]->files[it->second.file].length;
}

bool ArchiveSet::Read(const std::string& path, std::vector<uint8_t>& out, int* archive_index,
                      std::string* error) const {
    auto it = impl_->global.find(KeyOf(path));
    if (it == impl_->global.end()) {
        if (error) *error = "not found: " + path;
        return false;
    }
    if (archive_index) *archive_index = (int)it->second.archive;
    return impl_->archives[it->second.archive]->ReadEntry(it->second.file, out, error);
}

bool ArchiveSet::ReadFrom(size_t ai, const std::string& path, std::vector<uint8_t>& out, std::string* error) const {
    if (ai >= Count()) {
        if (error) *error = "bad archive index";
        return false;
    }
    Archive& a = *impl_->archives[ai];
    auto it = a.index.find(KeyOf(path));
    if (it == a.index.end()) {
        if (error) *error = "not found in " + a.name + ": " + path;
        return false;
    }
    return a.ReadEntry(it->second, out, error);
}

std::vector<std::string> ArchiveSet::List(size_t ai) const {
    std::vector<std::string> out;
    if (ai >= Count()) return out;
    for (const auto& f : impl_->archives[ai]->files)
        if (!f.path.empty()) out.push_back(f.path);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> ArchiveSet::ListAll(const std::string& substring) const {
    std::vector<std::string> out;
    std::string needle = detail::Lower(substring);
    for (const auto& kv : impl_->global) {
        if (!needle.empty() && kv.first.find(needle) == std::string::npos) continue;
        out.push_back(impl_->archives[kv.second.archive]->files[kv.second.file].path);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace ma2m
