// MGF v4 archives (MechAssault 2 resource archives): reader for plain and block-compressed
// archives, and a writer for plain archives.
//
// Layout (see bringup/inspect_mgf.py, MGF-Explorer):
//   header: 'mgf ', u32 0x5A5A0104 (version byte 4), u32 flags, then
//           file_count, file_bytes(=32n), file_offset, dir_count, dir_bytes(=24n), dir_offset,
//           strings_bytes, strings_offset
//   file record (8 u32): dir_row, crc_acc(payload), unknown, size, size, timestamp, offset, unknown
//   dir row (6 i32): hash, parent, first_child, next_sibling, name_offset, file_index (-1 = folder)
//   strings: 4-byte aligned NUL-terminated names; row 0 is the root "\".
// Compressed archives wrap that byte stream in independently zlib-compressed blocks.
#pragma once
#include <array>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "util.h"

namespace ma2 {

class MgfReader {
public:
    struct File {
        std::string path;   // as stored, '/' separated, original case: "Terrain/BoggedDown_ColorMap.tif"
        uint32_t rec[8];
        int row;
        uint32_t size() const { return rec[3]; }
        uint32_t offset() const { return rec[6]; }
    };

    MgfReader() = default;
    ~MgfReader();
    MgfReader(const MgfReader&) = delete;
    MgfReader& operator=(const MgfReader&) = delete;

    bool open(const fs::path& path, std::string* err = nullptr);
    void close();
    bool is_open() const { return f_ != nullptr; }
    bool compressed() const { return compressed_; }
    const fs::path& path() const { return path_; }
    uint64_t stream_size() const { return size_; }

    const std::vector<File>& files() const { return files_; }
    int find(std::string_view path) const;  // case-insensitive; '\' or '/'; -1 if missing
    // .ma2map archives (not the game's own, whose records do not all carry this checksum) can be read with the stored checksum
    // checked: a file whose bytes do not match fails to read and is listed in damaged().
    void set_check_crc(bool on) { check_crc_ = on; }
    const std::vector<std::string>& damaged() const { return damaged_; }
    bool read_file(int index, Bytes& out, std::string* err = nullptr);
    bool read_file(std::string_view path, Bytes& out, std::string* err = nullptr);
    bool read(uint64_t addr, size_t size, uint8_t* dst);

    // Raw directory, for tests and tools.
    uint32_t header[16] = {};
    std::vector<std::array<int32_t, 6>> rows;
    std::vector<std::string> row_names;

private:
    bool read_block(uint32_t number);
    std::mutex mu_;
    FILE* f_ = nullptr;
    fs::path path_;
    bool compressed_ = false;
    uint64_t size_ = 0;
    uint32_t block_size_ = 0;
    std::vector<uint32_t> chunks_;  // start, length, crc triples
    struct Block { uint32_t number; Bytes data; uint64_t used; };
    std::vector<Block> cache_;
    uint64_t tick_ = 0;
    bool check_crc_ = false;
    std::vector<std::string> damaged_;
    std::vector<File> files_;
    std::unordered_map<std::string, int> index_;  // norm_path -> files_ index
};

// Builds a plain (uncompressed) archive the way bringup/build_pc_assets.py lays one out.
class MgfWriter {
public:
    // Path uses '/' or '\'; case is preserved for the stored names. Later adds replace earlier ones.
    void add(std::string_view path, Bytes data, uint32_t timestamp = 0);
    bool has(std::string_view path) const;
    Bytes build() const;
    size_t count() const { return files_.size(); }

private:
    struct Entry { std::string path; Bytes data; uint32_t timestamp; };
    std::vector<Entry> files_;
};

}  // namespace ma2
