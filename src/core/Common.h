#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace cr {

enum class Lang { C, Cxx };

inline std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
inline std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}
inline std::string u8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
inline fs::path pathFromU8(const std::string& s) { return fs::u8path(s); }

inline bool readTextFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

inline fs::path normalize(const fs::path& p) {
    std::error_code ec;
    fs::path r = fs::absolute(p, ec);
    if (ec) r = p;
    return r.lexically_normal();
}

// Translation-unit extensions.
inline bool sourceLang(const fs::path& p, Lang& out) {
    std::string e = lower(u8(p.extension()));
    if (e == ".c") { out = Lang::C; return true; }
    if (e == ".cpp" || e == ".cc" || e == ".cxx" || e == ".c++" || e == ".cp") { out = Lang::Cxx; return true; }
    return false;
}
inline bool isHeaderExt(const fs::path& p) {
    std::string e = lower(u8(p.extension()));
    return e == ".h" || e == ".hpp" || e == ".hh" || e == ".hxx" || e == ".inl" || e == ".ipp";
}

}  // namespace cr
