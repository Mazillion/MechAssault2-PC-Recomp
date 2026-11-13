// Minimal JSON value, parser and writer for the editor's project data (editor/project.json).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ma2 {

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o;  // insertion order kept

    Json() = default;
    static Json boolean(bool v) { Json j; j.type = Bool; j.b = v; return j; }
    static Json number(double v) { Json j; j.type = Number; j.n = v; return j; }
    static Json string(std::string v) { Json j; j.type = String; j.s = std::move(v); return j; }
    static Json array() { Json j; j.type = Array; return j; }
    static Json object() { Json j; j.type = Object; return j; }

    Json& set(const std::string& k, Json v);
    // set() returns the new member; with() returns this object, for chaining: o.with("a", x).with("b", y).
    Json& with(const std::string& k, Json v) { set(k, std::move(v)); return *this; }
    const Json* get(const std::string& k) const;
    std::string str(const std::string& k, const std::string& def = {}) const;
    double num(const std::string& k, double def = 0) const;
    bool flag(const std::string& k, bool def = false) const;
    void push(Json v) { type = Array; a.push_back(std::move(v)); }

    std::string dump(int indent = 0) const;
    static bool parse(const std::string& text, Json& out, std::string* err = nullptr);
};

}  // namespace ma2
