#include "mgf.h"

#include <algorithm>
#include <ctime>
#include <map>

#include "inflate.h"

namespace ma2 {

MgfReader::~MgfReader() { close(); }

void MgfReader::close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (f_) fclose(f_);
    f_ = nullptr;
    compressed_ = false;
    size_ = 0;
    block_size_ = 0;
    chunks_.clear();
    cache_.clear();
    files_.clear();
    index_.clear();
    rows.clear();
    row_names.clear();
}

bool MgfReader::open(const fs::path& path, std::string* err) {
    close();
    path_ = path;
    f_ = _wfopen(path.c_str(), L"rb");
    if (!f_) { if (err) *err = "cannot open " + path_utf8(path); return false; }
    auto fail = [&](const std::string& why) {
        if (err) *err = why + ": " + path_utf8(path);
        if (f_) fclose(f_);
        f_ = nullptr;
        return false;
    };
    uint8_t h[32];
    if (fread(h, 1, 32, f_) != 32) return fail("short archive");
    if (!std::memcmp(h, "mgf ", 4)) {
        _fseeki64(f_, 0, SEEK_END);
        size_ = uint64_t(_ftelli64(f_));
    } else {
        uint32_t w[8];
        std::memcpy(w, h, 32);
        if (w[1] != 0xC5735A0Fu || !w[2] || w[2] > 200000 || !w[5] || w[5] > (64u << 20))
            return fail("unknown archive header");
        compressed_ = true;
        size_ = w[3];
        block_size_ = w[5];
        chunks_.resize(size_t(w[2]) * 3);
        if (_fseeki64(f_, 32, SEEK_SET) || fread(chunks_.data(), 12, w[2], f_) != w[2])
            return fail("short chunk table");
    }
    uint8_t hb[64] = {};
    if (!read(0, 44, hb)) return fail("short header");
    std::memcpy(header, hb, 44);
    if (header[0] != 0x2066676Du || hb[4] != 4) return fail("not an MA2 v4 archive");
    uint32_t nf = header[3], lf = header[4], of = header[5], nd = header[6], ld = header[7], od = header[8],
             ls = header[9], os = header[10];
    if (lf != nf * 32 || ld != nd * 24 || nf > 1000000 || nd > 1000000 || ls > (64u << 20))
        return fail("invalid directory sizes");
    std::vector<uint32_t> recs(size_t(nf) * 8);
    rows.resize(nd);
    std::string strings(ls, '\0');
    if ((nf && !read(of, lf, reinterpret_cast<uint8_t*>(recs.data()))) ||
        (nd && !read(od, ld, reinterpret_cast<uint8_t*>(rows.data()))) ||
        (ls && !read(os, ls, reinterpret_cast<uint8_t*>(strings.data()))))
        return fail("short directory");
    row_names.resize(nd);
    for (uint32_t i = 0; i < nd; i++) {
        uint32_t off = uint32_t(rows[i][4]);
        row_names[i] = off < ls ? std::string(strings.c_str() + off) : std::string();
    }
    for (uint32_t i = 0; i < nd; i++) {
        int fi = rows[i][5];
        if (fi < 0) continue;
        if (uint32_t(fi) >= nf) return fail("bad file index");
        std::vector<std::string> parts;
        int at = int(i), guard = 0;
        while (at >= 0) {
            if (uint32_t(at) >= nd || ++guard > 4096) return fail("bad directory ancestry");
            parts.push_back(row_names[at]);
            at = rows[at][1];
        }
        std::string p;
        for (size_t k = parts.size(); k-- > 0;) {
            std::string part = parts[k];
            std::replace(part.begin(), part.end(), '\\', '/');
            if (!p.empty() && p.back() != '/') p += '/';
            p += part;
        }
        size_t a = 0;
        while (a < p.size() && p[a] == '/') a++;
        File file;
        file.path = p.substr(a);
        std::memcpy(file.rec, &recs[size_t(fi) * 8], 32);
        file.row = int(i);
        index_.emplace(norm_path(file.path), int(files_.size()));
        files_.push_back(std::move(file));
    }
    return true;
}

bool MgfReader::read_block(uint32_t number) {
    for (auto& b : cache_) if (b.number == number) { b.used = ++tick_; return true; }
    if (size_t(number) * 3 + 2 >= chunks_.size()) return false;
    uint32_t start = chunks_[number * 3], length = chunks_[number * 3 + 1];
    if (!length || length > (128u << 20)) return false;
    Bytes packed(length);
    if (_fseeki64(f_, start, SEEK_SET) || fread(packed.data(), 1, length, f_) != length) return false;
    Block blk;
    blk.number = number;
    blk.data.resize(block_size_);
    long got = zlib_inflate(packed.data(), length, blk.data.data(), block_size_);
    if (got <= 0) return false;
    blk.data.resize(size_t(got));
    blk.used = ++tick_;
    if (cache_.size() >= 24) {
        auto oldest = std::min_element(cache_.begin(), cache_.end(),
                                       [](const Block& a, const Block& b) { return a.used < b.used; });
        *oldest = std::move(blk);
    } else {
        cache_.push_back(std::move(blk));
    }
    return true;
}

bool MgfReader::read(uint64_t addr, size_t size, uint8_t* dst) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!f_ || addr + size > size_) return false;
    if (!compressed_) {
        return _fseeki64(f_, int64_t(addr), SEEK_SET) == 0 && fread(dst, 1, size, f_) == size;
    }
    while (size) {
        uint32_t number = uint32_t(addr / block_size_), offset = uint32_t(addr % block_size_);
        if (!read_block(number)) return false;
        const Block* blk = nullptr;
        for (auto& b : cache_) if (b.number == number) blk = &b;
        if (!blk || offset >= blk->data.size()) return false;
        size_t take = std::min(size, blk->data.size() - offset);
        std::memcpy(dst, blk->data.data() + offset, take);
        dst += take; addr += take; size -= take;
    }
    return true;
}

int MgfReader::find(std::string_view path) const {
    auto it = index_.find(norm_path(path));
    return it == index_.end() ? -1 : it->second;
}

bool MgfReader::read_file(int index, Bytes& out, std::string* err) {
    out.clear();
    if (index < 0 || size_t(index) >= files_.size()) { if (err) *err = "no such file"; return false; }
    const File& f = files_[size_t(index)];
    // A damaged or hostile shared file can name a 4 GB entry: check it fits the archive before allocating.
    if (uint64_t(f.offset()) + f.size() > size_) { if (err) *err = "bad entry " + f.path; return false; }
    out.resize(f.size());
    if (f.size() && !read(f.offset(), f.size(), out.data())) {
        if (err) *err = "cannot read " + f.path;
        out.clear();
        return false;
    }
    if (check_crc_ && crc_acc(out.data(), out.size()) != f.rec[1]) {
        if (std::find(damaged_.begin(), damaged_.end(), f.path) == damaged_.end()) damaged_.push_back(f.path);
        if (err) *err = "damaged " + f.path;
        out.clear();
        return false;
    }
    return true;
}

bool MgfReader::read_file(std::string_view path, Bytes& out, std::string* err) {
    int i = find(path);
    if (i < 0) { if (err) *err = "missing " + std::string(path); out.clear(); return false; }
    return read_file(i, out, err);
}

// ---------------------------------------------------------------------------------------------

void MgfWriter::add(std::string_view path, Bytes data, uint32_t timestamp) {
    std::string p(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    std::string key = norm_path(p);
    for (auto& e : files_) {
        if (norm_path(e.path) == key) { e.data = std::move(data); e.timestamp = timestamp; return; }
    }
    files_.push_back({p, std::move(data), timestamp});
}

bool MgfWriter::has(std::string_view path) const {
    std::string key = norm_path(path);
    for (auto& e : files_) if (norm_path(e.path) == key) return true;
    return false;
}

Bytes MgfWriter::build() const {
    // Directory tree. Folders and files sorted case-insensitively; children linked in that order.
    struct Node { std::string name; int parent; std::vector<int> kids; int file; };
    std::vector<Node> nodes;
    nodes.push_back({"\\", -1, {}, -1});
    std::vector<int> order(files_.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = int(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return norm_path(files_[a].path) < norm_path(files_[b].path); });
    std::map<std::string, int> folder_index;  // lower(folder path) -> node
    std::vector<int> file_node(files_.size(), -1);
    for (int fi : order) {
        auto parts = split(files_[fi].path, '/');
        int parent = 0;
        std::string acc;
        for (size_t k = 0; k + 1 < parts.size(); k++) {
            acc += lower(parts[k]) + "/";
            auto it = folder_index.find(acc);
            if (it == folder_index.end()) {
                nodes.push_back({parts[k], parent, {}, -1});
                int id = int(nodes.size()) - 1;
                nodes[parent].kids.push_back(id);
                folder_index[acc] = id;
                parent = id;
            } else {
                parent = it->second;
            }
        }
        nodes.push_back({parts.back(), parent, {}, fi});
        int id = int(nodes.size()) - 1;
        nodes[parent].kids.push_back(id);
        file_node[fi] = id;
    }
    // Rows in depth-first order (as retail: a folder, then its subtree).
    std::vector<int> row_of(nodes.size(), -1), seq;
    std::vector<int> stack{0};
    while (!stack.empty()) {
        int n = stack.back();
        stack.pop_back();
        row_of[n] = int(seq.size());
        seq.push_back(n);
        auto kids = nodes[n].kids;
        std::sort(kids.begin(), kids.end(), [&](int a, int b) { return lower(nodes[a].name) < lower(nodes[b].name); });
        nodes[n].kids = kids;
        for (size_t k = kids.size(); k-- > 0;) stack.push_back(kids[k]);
    }
    // File records in row order.
    std::vector<int> file_order;  // file index (files_) per record
    std::vector<int> rec_of_node(nodes.size(), -1);
    for (int n : seq) if (nodes[n].file >= 0) { rec_of_node[n] = int(file_order.size()); file_order.push_back(nodes[n].file); }
    // Strings.
    Bytes strings;
    put_bytes(strings, "MGI\0", 4);
    std::vector<uint32_t> name_off(nodes.size());
    for (int n : seq) {
        name_off[n] = uint32_t(strings.size());
        put_bytes(strings, nodes[n].name.data(), nodes[n].name.size());
        strings.push_back(0);
        while (strings.size() % 4) strings.push_back(0);
    }
    const uint32_t nf = uint32_t(file_order.size()), nd = uint32_t(seq.size());
    const uint32_t file_offset = 0x40, dir_offset = file_offset + nf * 32, str_offset = dir_offset + nd * 24;
    uint32_t data_offset = (str_offset + uint32_t(strings.size()) + 15) & ~15u;
    Bytes out(data_offset, 0);
    // Header words as in retail archives (game/ma2map.py, verified by the maintainer).
    uint32_t h[16] = {0x2066676Du, 0x5A5A0104u, 0x10, nf, nf * 32, file_offset, nd, nd * 24, dir_offset,
                      uint32_t(strings.size()), str_offset, 1, 0x10000, 0, 0, 0};
    std::memcpy(out.data(), h, sizeof(h));
    // Full "\dir\name" path of a node, lower case: the key source for folder rows and file records.
    auto full_path = [&](int n) {
        std::vector<std::string> parts;
        while (n > 0) { parts.push_back(nodes[size_t(n)].name); n = nodes[size_t(n)].parent; }
        std::string p;
        for (size_t k = parts.size(); k-- > 0;) p += "\\" + parts[k];
        return lower(p.empty() ? std::string("\\") : p);
    };
    std::memcpy(out.data() + str_offset, strings.data(), strings.size());
    for (uint32_t r = 0; r < nd; r++) {
        int n = seq[r];
        const Node& node = nodes[n];
        int32_t row[6];
        row[0] = node.file >= 0 ? 0 : int32_t(key_of(full_path(n)));
        row[1] = node.parent >= 0 ? row_of[node.parent] : -1;
        row[2] = (node.file < 0 && !node.kids.empty()) ? row_of[node.kids.front()] : -1;
        row[3] = -1;
        if (node.parent >= 0) {
            const auto& sib = nodes[node.parent].kids;
            auto it = std::find(sib.begin(), sib.end(), n);
            if (it != sib.end() && it + 1 != sib.end()) row[3] = row_of[*(it + 1)];
        }
        row[4] = int32_t(name_off[n]);
        row[5] = rec_of_node[n];
        std::memcpy(out.data() + dir_offset + r * 24, row, 24);
    }
    for (uint32_t k = 0; k < nf; k++) {
        const Entry& e = files_[size_t(file_order[k])];
        while (out.size() % 16) out.push_back(0);
        uint32_t offset = uint32_t(out.size());
        put_bytes(out, e.data.data(), e.data.size());
        int fnode = file_node[size_t(file_order[k])];
        uint32_t rec[8] = {uint32_t(row_of[fnode]), crc_acc(e.data.data(), e.data.size()), key_of(full_path(fnode)),
                           uint32_t(e.data.size()), uint32_t(e.data.size()), e.timestamp ? e.timestamp : uint32_t(time(nullptr)),
                           offset, 0x12F700u};
        std::memcpy(out.data() + file_offset + k * 32, rec, 32);
    }
    while (out.size() % 16) out.push_back(0);
    return out;
}

}  // namespace ma2
