// Small shared helpers for the MechAssault 2 scenario editor format libraries.
#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace ma2 {

// CRC-32 (zlib polynomial) accumulator: starts at ~0 and is NOT complemented at the end.
// This is both the MGF record checksum and the world-file key hash: key(name) = crc_acc(name).
uint32_t crc_acc(const void* data, size_t size, uint32_t acc = 0xFFFFFFFFu);
inline uint32_t key_of(std::string_view name) { return crc_acc(name.data(), name.size()); }

bool read_file(const fs::path& path, Bytes& out, std::string* err = nullptr);
// Writes to "<path>.part" then renames over the target.
bool write_file(const fs::path& path, const void* data, size_t size, std::string* err = nullptr);
inline bool write_file(const fs::path& path, const Bytes& data, std::string* err = nullptr) {
    return write_file(path, data.data(), data.size(), err);
}

// Runs fn on a new thread and waits at most `ms` for it. Returns fn's result; false when it did not finish in time (*finished
// = false) or threw / faulted (the fault is swallowed, *finished stays true). A timed-out thread is left running and cleans
// up after itself. Used by the crash handler, whose save must not be able to hang the process (a lock the faulting thread held).
bool run_with_timeout(std::function<bool()> fn, unsigned ms, bool* finished = nullptr);
// Windows per-monitor DPI (96 = 100 %) to a UI scale; 0 or a nonsense value gives 1.
float dpi_to_scale(unsigned dpi);
// A string fit to be a Windows file name stem: backslash slash : * ? " < > | and control characters become '_', trailing dots and spaces are dropped,
// reserved device names (CON, NUL, COM1 ...) get a leading '_', never empty ("Scenario"), at most 120 bytes.
std::string safe_file_stem(std::string_view s);

std::string lower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool istarts_with(std::string_view s, std::string_view prefix);
bool iends_with(std::string_view s, std::string_view suffix);
std::string trim(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep);
// Backslash/forward slash agnostic, case-folded, without leading separators: "models/a/b.mgmodel".
std::string norm_path(std::string_view p);

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
std::string path_utf8(const fs::path& p);

// Log privacy: text for a log / diagnostic file or stderr must not carry the Windows user name. Replaces (case-insensitive) the
// values of %LOCALAPPDATA%, %APPDATA% and %USERPROFILE% (when followed by the end, a slash, a quote or a space) with those literal
// tokens, then the name in any remaining "X:\Users\<name>" with "<user>" ("Public" kept). Only the logged copy changes.
std::wstring redact_user_paths(std::wstring_view text);
std::string redact_user_paths(std::string_view utf8);
// Writes text to stderr through redact_user_paths.
void log_stderr(std::string_view text);

template <class T> inline T rd(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }
template <class T> inline void wr(uint8_t* p, T v) { std::memcpy(p, &v, sizeof(T)); }
template <class T> inline void put(Bytes& b, T v) {
    size_t n = b.size(); b.resize(n + sizeof(T)); std::memcpy(b.data() + n, &v, sizeof(T));
}
inline void put_bytes(Bytes& b, const void* p, size_t n) {
    const uint8_t* s = static_cast<const uint8_t*>(p); b.insert(b.end(), s, s + n);
}

// Simple deterministic RNG (xorshift) for noise, scatter and ids.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull) : s(seed ? seed : 1) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    float unit() { return float(next() >> 40) / float(1ull << 24); }  // [0,1)
    float range(float a, float b) { return a + (b - a) * unit(); }
};

}  // namespace ma2
