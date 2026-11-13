#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ma2 {

Json& Json::set(const std::string& k, Json v) {
    type = Object;
    for (auto& kv : o) if (kv.first == k) { kv.second = std::move(v); return kv.second; }
    o.emplace_back(k, std::move(v));
    return o.back().second;
}

const Json* Json::get(const std::string& k) const {
    if (type != Object) return nullptr;
    for (auto& kv : o) if (kv.first == k) return &kv.second;
    return nullptr;
}

std::string Json::str(const std::string& k, const std::string& def) const {
    const Json* j = get(k);
    return j && j->type == String ? j->s : def;
}
double Json::num(const std::string& k, double def) const {
    const Json* j = get(k);
    return j && j->type == Number ? j->n : def;
}
bool Json::flag(const std::string& k, bool def) const {
    const Json* j = get(k);
    return j && j->type == Bool ? j->b : def;
}

static void escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += char(c);
        }
    }
    out += '"';
}

static void dump_to(const Json& j, std::string& out, int indent, int depth) {
    std::string pad = indent ? "\n" + std::string(size_t(indent * (depth + 1)), ' ') : "";
    std::string end = indent ? "\n" + std::string(size_t(indent * depth), ' ') : "";
    switch (j.type) {
        case Json::Null: out += "null"; break;
        case Json::Bool: out += j.b ? "true" : "false"; break;
        case Json::Number: {
            char b[64];
            if (std::fabs(j.n - std::round(j.n)) < 1e-9 && std::fabs(j.n) < 1e15) snprintf(b, sizeof b, "%lld", (long long)std::llround(j.n));
            else snprintf(b, sizeof b, "%.9g", j.n);
            out += b;
            break;
        }
        case Json::String: escape(out, j.s); break;
        case Json::Array:
            out += '[';
            for (size_t i = 0; i < j.a.size(); i++) { if (i) out += ','; out += pad; dump_to(j.a[i], out, indent, depth + 1); }
            if (!j.a.empty()) out += end;
            out += ']';
            break;
        case Json::Object:
            out += '{';
            for (size_t i = 0; i < j.o.size(); i++) {
                if (i) out += ',';
                out += pad;
                escape(out, j.o[i].first);
                out += indent ? ": " : ":";
                dump_to(j.o[i].second, out, indent, depth + 1);
            }
            if (!j.o.empty()) out += end;
            out += '}';
            break;
    }
}

std::string Json::dump(int indent) const {
    std::string s;
    dump_to(*this, s, indent, 0);
    return s;
}

namespace {
struct Parser {
    const std::string& t;
    size_t p = 0;
    std::string err;
    void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) p++; }
    bool lit(const char* s) {
        size_t n = strlen(s);
        if (t.compare(p, n, s) == 0) { p += n; return true; }
        return false;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 63)); }
        else { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
    }
    bool str(std::string& o) {
        if (t[p] != '"') return false;
        p++;
        while (p < t.size() && t[p] != '"') {
            char c = t[p++];
            if (c != '\\') { o += c; continue; }
            if (p >= t.size()) return false;
            char e = t[p++];
            switch (e) {
                case 'n': o += '\n'; break; case 'r': o += '\r'; break; case 't': o += '\t'; break;
                case 'b': o += '\b'; break; case 'f': o += '\f'; break;
                case 'u': {
                    if (p + 4 > t.size()) return false;
                    unsigned cp = unsigned(strtoul(t.substr(p, 4).c_str(), nullptr, 16));
                    p += 4;
                    utf8(o, cp);
                    break;
                }
                default: o += e;
            }
        }
        if (p >= t.size()) return false;
        p++;
        return true;
    }
    bool val(Json& j, int depth) {
        if (depth > 200) return false;
        ws();
        if (p >= t.size()) return false;
        char c = t[p];
        if (c == '{') {
            p++;
            j = Json::object();
            ws();
            if (p < t.size() && t[p] == '}') { p++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (p >= t.size() || !str(k)) return false;
                ws();
                if (p >= t.size() || t[p] != ':') return false;
                p++;
                Json v;
                if (!val(v, depth + 1)) return false;
                j.o.emplace_back(std::move(k), std::move(v));
                ws();
                if (p < t.size() && t[p] == ',') { p++; continue; }
                if (p < t.size() && t[p] == '}') { p++; return true; }
                return false;
            }
        }
        if (c == '[') {
            p++;
            j = Json::array();
            ws();
            if (p < t.size() && t[p] == ']') { p++; return true; }
            for (;;) {
                Json v;
                if (!val(v, depth + 1)) return false;
                j.a.push_back(std::move(v));
                ws();
                if (p < t.size() && t[p] == ',') { p++; continue; }
                if (p < t.size() && t[p] == ']') { p++; return true; }
                return false;
            }
        }
        if (c == '"') { j = Json::string(""); return str(j.s); }
        if (lit("true")) { j = Json::boolean(true); return true; }
        if (lit("false")) { j = Json::boolean(false); return true; }
        if (lit("null")) { j = Json(); return true; }
        char* end;
        double d = strtod(t.c_str() + p, &end);
        if (end == t.c_str() + p) return false;
        p = size_t(end - t.c_str());
        j = Json::number(d);
        return true;
    }
};
}  // namespace

bool Json::parse(const std::string& text, Json& out, std::string* err) {
    Parser ps{text};
    if (!ps.val(out, 0)) {
        if (err) *err = "JSON syntax error near byte " + std::to_string(ps.p);
        return false;
    }
    return true;
}

}  // namespace ma2
