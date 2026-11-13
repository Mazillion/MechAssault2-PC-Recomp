// String, path, INI and XML helpers.
#include "internal.h"

#include <cctype>
#include <cstdlib>

namespace ma2m {

std::string NormalizePath(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty() || cur == ".") {
        } else if (cur == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (char c : path) {
        if (c == '\\' || c == '/') flush();
        else cur += c;
    }
    flush();
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

std::string ResolveRelative(const std::string& base_file, const std::string& relative) {
    std::string rel = detail::Trim(relative);
    if (!rel.empty() && (rel[0] == '\\' || rel[0] == '/')) return NormalizePath(rel);
    std::string folder = detail::FolderOf(NormalizePath(base_file));
    return NormalizePath(folder.empty() ? rel : folder + "/" + rel);
}

std::string Extension(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
    std::string e = path.substr(dot + 1);
    size_t brace = e.find('{');
    if (brace != std::string::npos) e.resize(brace);
    return detail::Lower(e);
}

namespace detail {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool IEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

bool EndsWithI(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && IEquals(s.substr(s.size() - suffix.size()), suffix);
}

bool StartsWithI(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && IEquals(s.substr(0, prefix.size()), prefix);
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string Unquote(const std::string& s) {
    std::string t = Trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
    if (!t.empty() && t.front() == '"') return t.substr(1);
    return t;
}

int ParseFloats(const std::string& s, float* out, int max_count) {
    int n = 0;
    const char* p = s.c_str();
    while (*p && n < max_count) {
        while (*p && !(isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.')) p++;
        if (!*p) break;
        char* end = nullptr;
        double v = strtod(p, &end);
        if (end == p) {
            p++;
            continue;
        }
        out[n++] = (float)v;
        p = end;
    }
    return n;
}

bool ParseBool(const std::string& s, bool fallback) {
    std::string t = Lower(Trim(s));
    if (t == "true" || t == "yes" || t == "1" || t == "on") return true;
    if (t == "false" || t == "no" || t == "0" || t == "off") return false;
    return fallback;
}

std::string FileName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string FileStem(const std::string& path) {
    std::string f = FileName(path);
    size_t dot = f.find_last_of('.');
    return dot == std::string::npos ? f : f.substr(0, dot);
}

std::string FolderOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// ---------------------------------------------------------------------------
// MAXPort INI: [section] then "key = value" lines; ';' comments; values may be
// quoted; multi-line values (bone_transform) continue on lines without '='.
// ---------------------------------------------------------------------------

const std::string* IniSection::Get(const char* key) const {
    for (const auto& kv : values)
        if (IEquals(kv.first, key)) return &kv.second;
    return nullptr;
}

std::string IniSection::GetOr(const char* key, const std::string& fallback) const {
    const std::string* v = Get(key);
    return v ? *v : fallback;
}

void ParseIni(const char* text, size_t len, std::vector<IniSection>& sections) {
    sections.clear();
    size_t i = 0;
    IniSection* cur = nullptr;
    while (i < len) {
        size_t e = i;
        while (e < len && text[e] != '\n' && text[e] != '\r') e++;
        std::string line(text + i, e - i);
        i = e;
        while (i < len && (text[i] == '\n' || text[i] == '\r')) i++;
        // Strip comments that are not inside quotes.
        bool q = false;
        for (size_t k = 0; k < line.size(); k++) {
            if (line[k] == '"') q = !q;
            else if (line[k] == ';' && !q) {
                line.resize(k);
                break;
            }
        }
        line = Trim(line);
        if (line.empty()) continue;
        if (line[0] == '[') {
            size_t close = line.find(']');
            sections.emplace_back();
            sections.back().name = Trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            cur = &sections.back();
            continue;
        }
        size_t eq = line.find('=');
        if (!cur) {
            sections.emplace_back();
            cur = &sections.back();
        }
        if (eq == std::string::npos) {
            if (!cur->values.empty()) cur->values.back().second += " " + line;
            continue;
        }
        cur->values.emplace_back(Trim(line.substr(0, eq)), Unquote(line.substr(eq + 1)));
    }
}

// ---------------------------------------------------------------------------
// XML (MGMODEL, mgparticle): elements, attributes, comments, <?...?>.
// ---------------------------------------------------------------------------

const std::string* XmlNode::Attr(const char* name) const {
    for (const auto& kv : attrs)
        if (IEquals(kv.first, name)) return &kv.second;
    return nullptr;
}

std::string XmlNode::AttrOr(const char* name, const std::string& fallback) const {
    const std::string* v = Attr(name);
    return v ? *v : fallback;
}

const XmlNode* XmlNode::Child(const char* name) const {
    for (const auto& c : children)
        if (IEquals(c.tag, name)) return &c;
    return nullptr;
}

namespace {

std::string DecodeEntities(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i < 8) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                char c = 0;
                if (ent == "amp") c = '&';
                else if (ent == "lt") c = '<';
                else if (ent == "gt") c = '>';
                else if (ent == "quot") c = '"';
                else if (ent == "apos") c = '\'';
                else if (!ent.empty() && ent[0] == '#') c = (char)strtol(ent.c_str() + (ent.size() > 1 && ent[1] == 'x' ? 2 : 1), nullptr, ent.size() > 1 && ent[1] == 'x' ? 16 : 10);
                if (c) {
                    out += c;
                    i = semi;
                    continue;
                }
            }
        }
        out += s[i];
    }
    return out;
}

struct XmlParser {
    const char* p;
    const char* end;
    std::string err;

    void SkipWs() {
        while (p < end && isspace((unsigned char)*p)) p++;
    }
    bool StartsWith(const char* s) const {
        size_t n = strlen(s);
        return (size_t)(end - p) >= n && memcmp(p, s, n) == 0;
    }
    void SkipMisc() {
        for (;;) {
            SkipWs();
            if (StartsWith("<?")) {
                const char* q = strstr_bounded("?>");
                p = q ? q + 2 : end;
            } else if (StartsWith("<!--")) {
                const char* q = strstr_bounded("-->");
                p = q ? q + 3 : end;
            } else if (StartsWith("<!")) {
                while (p < end && *p != '>') p++;
                if (p < end) p++;
            } else {
                return;
            }
        }
    }
    const char* strstr_bounded(const char* s) const {
        size_t n = strlen(s);
        for (const char* q = p; q + n <= end; q++)
            if (memcmp(q, s, n) == 0) return q;
        return nullptr;
    }
    std::string Name() {
        const char* s = p;
        while (p < end && !isspace((unsigned char)*p) && *p != '>' && *p != '/' && *p != '=') p++;
        return std::string(s, p - s);
    }
    bool Element(XmlNode& node, int depth) {
        if (depth > 256) {
            err = "XML nesting too deep";
            return false;
        }
        if (p >= end || *p != '<') {
            err = "expected '<'";
            return false;
        }
        p++;
        node.tag = Name();
        if (node.tag.empty()) {
            err = "empty tag name";
            return false;
        }
        for (;;) {
            SkipWs();
            if (p >= end) {
                err = "unterminated tag <" + node.tag + ">";
                return false;
            }
            if (*p == '/') {
                p++;
                if (p < end && *p == '>') p++;
                return true;  // self-closing
            }
            if (*p == '>') {
                p++;
                break;
            }
            std::string key = Name();
            SkipWs();
            std::string value;
            if (p < end && *p == '=') {
                p++;
                SkipWs();
                if (p < end && (*p == '"' || *p == '\'')) {
                    char qc = *p++;
                    const char* s = p;
                    while (p < end && *p != qc) p++;
                    value.assign(s, p - s);
                    if (p < end) p++;
                } else {
                    const char* s = p;
                    while (p < end && !isspace((unsigned char)*p) && *p != '>' && *p != '/') p++;
                    value.assign(s, p - s);
                }
            }
            if (key.empty()) {
                p++;
                continue;
            }
            node.attrs.emplace_back(key, DecodeEntities(value));
        }
        // Content: children and text (text is ignored).
        for (;;) {
            while (p < end && *p != '<') p++;
            if (p >= end) {
                err = "missing </" + node.tag + ">";
                return false;
            }
            if (StartsWith("</")) {
                p += 2;
                Name();
                while (p < end && *p != '>') p++;
                if (p < end) p++;
                return true;
            }
            if (StartsWith("<!--") || StartsWith("<?") || StartsWith("<!")) {
                SkipMisc();
                continue;
            }
            node.children.emplace_back();
            if (!Element(node.children.back(), depth + 1)) return false;
        }
    }
};

}  // namespace

bool ParseXml(const char* text, size_t len, XmlNode& root, std::string* error) {
    XmlParser px{text, text + len, {}};
    // Strip trailing NULs some archive files carry.
    while (px.end > px.p && px.end[-1] == 0) px.end--;
    px.SkipMisc();
    root = XmlNode();
    if (!px.Element(root, 0)) {
        if (error) *error = "XML: " + px.err;
        return false;
    }
    return true;
}

}  // namespace detail
}  // namespace ma2m
