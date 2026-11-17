// Small pure text helpers: no ImGui, no App, so fmt_test can run them.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace ed {

// A heading as the screen shows it: 0 up to 359.99 degrees, never negative and never 360. Stored values are not changed.
inline float heading_display(float deg) {
    float h = std::fmod(std::fmod(deg, 360.f) + 360.f, 360.f);
    if (h >= 359.995f) h = 0.f;
    return h;
}

// "1 other object moves" / "2 other objects move": the count, "other", the noun with its plural and the verb that agrees.
inline std::string n_things(size_t n, const char* noun, const char* verb_one = "moves", const char* verb_many = "move") {
    return std::to_string(n) + " other " + noun + (n == 1 ? "" : "s") + " " + (n == 1 ? verb_one : verb_many);
}

// A size in a raw template name ("Building base 34x34 generic ...") is the author's; the model may measure otherwise. The first
// "<W>x<L>" token is replaced by the measured whole metres when either differs by more than 1 m (in either order); else unchanged.
inline std::string fix_size_token(const std::string& name, float dx, float dz) {
    const int a = int(std::lround(std::min(dx, dz))), b = int(std::lround(std::max(dx, dz)));
    for (size_t i = 0; i < name.size(); i++) {
        if (!std::isdigit(uint8_t(name[i])) || (i > 0 && (std::isalnum(uint8_t(name[i - 1])) || name[i - 1] == '.' || name[i - 1] == '_'))) continue;
        size_t j = i;
        while (j < name.size() && std::isdigit(uint8_t(name[j]))) j++;
        if (j >= name.size() || name[j] != 'x' || j + 1 >= name.size() || !std::isdigit(uint8_t(name[j + 1]))) continue;
        size_t k = j + 1;
        while (k < name.size() && std::isdigit(uint8_t(name[k]))) k++;
        if (k < name.size() && (std::isalnum(uint8_t(name[k])) || name[k] == '_')) continue;
        const int w = std::stoi(name.substr(i, j - i)), l = std::stoi(name.substr(j + 1, k - j - 1));
        const int lo = std::min(w, l), hi = std::max(w, l);
        if (std::abs(lo - a) <= 1 && std::abs(hi - b) <= 1) return name;
        return name.substr(0, i) + std::to_string(a) + "x" + std::to_string(b) + name.substr(k);
    }
    return name;
}

}  // namespace ed
