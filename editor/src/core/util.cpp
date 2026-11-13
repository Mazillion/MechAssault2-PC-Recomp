#include "util.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <system_error>
#include <exception>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>

namespace ma2 {

static uint32_t g_crc_table[256];
static bool g_crc_ready = false;

uint32_t crc_acc(const void* data, size_t size, uint32_t acc) {
    if (!g_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            g_crc_table[i] = c;
        }
        g_crc_ready = true;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; i++) acc = g_crc_table[(acc ^ p[i]) & 0xFF] ^ (acc >> 8);
    return acc;
}

bool read_file(const fs::path& path, Bytes& out, std::string* err) {
    out.clear();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { if (err) *err = "cannot open " + path_utf8(path); return false; }
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); if (err) *err = "cannot size " + path_utf8(path); return false; }
    out.resize(size_t(n));
    bool ok = n == 0 || fread(out.data(), 1, size_t(n), f) == size_t(n);
    fclose(f);
    if (!ok && err) *err = "short read " + path_utf8(path);
    return ok;
}

bool write_file(const fs::path& path, const void* data, size_t size, std::string* err) {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += L".part";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) { if (err) *err = "cannot create " + path_utf8(tmp); return false; }
    bool ok = size == 0 || fwrite(data, 1, size, f) == size;
    // The data must reach the disk before the rename (MOVEFILE_WRITE_THROUGH covers only the rename): a power cut just
    // after Save could otherwise leave a zero-filled scenario.
    ok = ok && fflush(f) == 0 && _commit(_fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok) { fs::remove(tmp, ec); if (err) *err = "write failed " + path_utf8(path); return false; }
    // OneDrive (Documents is often synced) and antivirus hold the target briefly: retry before giving up.
    BOOL moved = FALSE;
    for (int k = 0; k < 5 && !moved; k++) {
        moved = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!moved && k < 4) Sleep(100);
    }
    if (!moved) {
        fs::remove(tmp, ec);
        if (err) *err = "cannot replace " + path_utf8(path);
        return false;
    }
    return true;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return r;
}

static inline char lc(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (lc(a[i]) != lc(b[i])) return false;
    return true;
}
bool istarts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && iequals(s.substr(0, p.size()), p); }
bool iends_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && iequals(s.substr(s.size() - p.size()), p); }

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == sep) { out.emplace_back(s.substr(a, i - a)); a = i + 1; }
    }
    return out;
}

std::string norm_path(std::string_view p) {
    std::string r = lower(p);
    std::replace(r.begin(), r.end(), '\\', '/');
    size_t a = 0;
    while (a < r.size() && r[a] == '/') a++;
    r.erase(0, a);
    // collapse duplicate slashes
    std::string out;
    out.reserve(r.size());
    for (char c : r) { if (c == '/' && !out.empty() && out.back() == '/') continue; out += c; }
    return out;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string path_utf8(const fs::path& p) { return narrow(p.native()); }

static std::wstring lower_w(std::wstring_view s) {
    std::wstring r(s);
    for (auto& c : r) if (c >= L'A' && c <= L'Z') c = wchar_t(c + 32);
    return r;
}
static bool name_ends(wchar_t c) { return c == L'\\' || c == L'/' || c == L'"' || c == L' ' || c == L'\''; }
static bool is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

static std::wstring redact_env(const std::wstring& text, const wchar_t* var) {
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(var, buf, 2048);
    if (n == 0 || n >= 2048) return text;
    std::wstring val(buf, n);
    while (val.size() > 3 && is_sep(val.back())) val.pop_back();
    if (val.size() < 3) return text;
    std::wstring token = std::wstring(L"%") + var + L"%", lv = lower_w(val), lt = lower_w(text), out;
    size_t pos = 0;
    while (true) {
        size_t at = lt.find(lv, pos);
        if (at == std::wstring::npos) break;
        size_t end = at + lv.size();
        if (end == lt.size() || name_ends(lt[end])) {
            out.append(text, pos, at - pos);
            out += token;
            pos = end;
        } else {
            out.append(text, pos, at + 1 - pos);
            pos = at + 1;
        }
    }
    out.append(text, pos, std::wstring::npos);
    return out;
}

std::wstring redact_user_paths(std::wstring_view in) {
    std::wstring t = redact_env(redact_env(redact_env(std::wstring(in), L"LOCALAPPDATA"), L"APPDATA"), L"USERPROFILE");
    // Remaining "X:\Users\<name>" (either slash).
    std::wstring out, lt = lower_w(t);
    size_t pos = 0;
    while (true) {
        size_t at = lt.find(L':', pos);
        if (at == std::wstring::npos) break;
        bool hit = at >= 1 && at + 8 < lt.size() && lt[at - 1] >= L'a' && lt[at - 1] <= L'z' && (at < 2 || !std::iswalnum(lt[at - 2]))
                   && is_sep(lt[at + 1]) && lt.compare(at + 2, 5, L"users") == 0 && is_sep(lt[at + 7]);
        if (!hit) { out.append(t, pos, at + 1 - pos); pos = at + 1; continue; }
        size_t ns = at + 8, ne = ns;
        while (ne < t.size() && !is_sep(t[ne]) && std::wstring_view(L"\"<>|*?\n\r").find(t[ne]) == std::wstring_view::npos) ne++;
        std::wstring name = lt.substr(ns, ne - ns);
        out.append(t, pos, ns - pos);
        if (name != L"public" && !name.empty() && name[0] != L'%') out += L"<user>"; else out.append(t, ns, ne - ns);
        pos = ne;
    }
    out.append(t, pos, std::wstring::npos);
    return out;
}
std::string redact_user_paths(std::string_view s) { return narrow(redact_user_paths(widen(s))); }

void log_stderr(std::string_view text) {
    std::string r = redact_user_paths(text);
    fwrite(r.data(), 1, r.size(), stderr);
}

namespace {
struct TimeoutJob {
    std::function<bool()> fn;
    HANDLE done = nullptr;
    volatile LONG refs = 2;  // the waiter and the thread
    bool result = false;
};
void job_release(TimeoutJob* j) {
    if (InterlockedDecrement(&j->refs) == 0) { CloseHandle(j->done); delete j; }
}
bool job_call_cpp(TimeoutJob* j) {
    try { return j->fn(); } catch (...) { return false; }
}
bool job_call_seh(TimeoutJob* j) {  // no C++ objects here: __try cannot be mixed with unwinding
    __try { return job_call_cpp(j); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
DWORD WINAPI job_thread(LPVOID p) {
    TimeoutJob* j = static_cast<TimeoutJob*>(p);
    j->result = job_call_seh(j);
    SetEvent(j->done);
    job_release(j);
    return 0;
}
}  // namespace

bool run_with_timeout(std::function<bool()> fn, unsigned ms, bool* finished) {
    if (finished) *finished = false;
    TimeoutJob* j = new TimeoutJob{std::move(fn)};
    j->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE th = j->done ? CreateThread(nullptr, 0, job_thread, j, 0, nullptr) : nullptr;
    if (!th) {  // no thread: run it here rather than not at all
        bool r = job_call_seh(j);
        if (finished) *finished = true;
        if (j->done) CloseHandle(j->done);
        delete j;
        return r;
    }
    CloseHandle(th);
    bool done = WaitForSingleObject(j->done, ms) == WAIT_OBJECT_0;
    bool r = done && j->result;
    if (finished) *finished = done;
    job_release(j);
    return r;
}

float dpi_to_scale(unsigned dpi) {
    if (dpi < 24 || dpi > 1536) return 1.f;  // 0.25x .. 16x of 96
    return float(dpi) / 96.f;
}

std::string safe_file_stem(std::string_view s) {
    std::string r;
    for (unsigned char ch : s) r += (ch < 32 || std::strchr("\\/:*?\"<>|", ch)) ? '_' : char(ch);
    if (r.size() > 120) { r.resize(120); while (!r.empty() && (static_cast<unsigned char>(r.back()) & 0xC0) == 0x80) r.pop_back(); }
    while (!r.empty() && (r.back() == '.' || r.back() == ' ')) r.pop_back();
    size_t lead = r.find_first_not_of(' ');
    r = lead == std::string::npos ? std::string() : r.substr(lead);
    if (r.empty()) return "Scenario";
    std::string stem = lower(r.substr(0, r.find('.')));  // "con.txt" is reserved too
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    static const char* kReserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8",
                                      "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* k : kReserved) if (stem == k) return "_" + r;
    return r;
}

}  // namespace ma2
