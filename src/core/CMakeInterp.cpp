#include <sstream>
#include <fstream>
#include "CMakeInterp.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace cr {
namespace {

struct CMakeError : std::runtime_error {
    bool located;
    explicit CMakeError(const std::string& m, bool loc = false) : std::runtime_error(m), located(loc) {}
};

struct Arg {
    std::string text;
    bool quoted = false;
};
struct Cmd {
    std::string name;
    std::vector<Arg> args;
    int line = 0;
};
struct Tok {
    std::string t;
    bool q = false;
};

// ---------------------------------------------------------------- parser ---
class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool parse(std::vector<Cmd>& out, std::string& err) {
        while (true) {
            skipSpaceAndComments();
            if (i_ >= s_.size()) return true;
            Cmd c;
            c.line = line_;
            size_t st = i_;
            while (i_ < s_.size() && (std::isalnum((unsigned char)s_[i_]) || s_[i_] == '_')) i_++;
            if (st == i_) {
                err = "line " + std::to_string(line_) + ": unexpected character '" + std::string(1, s_[i_]) + "'";
                return false;
            }
            c.name = s_.substr(st, i_ - st);
            skipSpaceAndComments();
            if (i_ >= s_.size() || s_[i_] != '(') {
                err = "line " + std::to_string(line_) + ": expected '(' after '" + c.name + "'";
                return false;
            }
            i_++;
            if (!parseArgs(c.args, err)) return false;
            out.push_back(std::move(c));
        }
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    int line_ = 1;

    void adv() {
        if (s_[i_] == '\n') line_++;
        i_++;
    }

    // Returns true and positions i_ after the bracket if s_[at] starts a bracket open "[=*[".
    bool bracketOpen(size_t at, size_t& contentStart, std::string& close) const {
        if (at >= s_.size() || s_[at] != '[') return false;
        size_t j = at + 1, eq = 0;
        while (j < s_.size() && s_[j] == '=') { eq++; j++; }
        if (j >= s_.size() || s_[j] != '[') return false;
        contentStart = j + 1;
        close = "]" + std::string(eq, '=') + "]";
        return true;
    }

    void skipSpaceAndComments() {
        while (i_ < s_.size()) {
            char ch = s_[i_];
            if (std::isspace((unsigned char)ch)) {
                adv();
            } else if (ch == '#') {
                size_t cs;
                std::string close;
                if (bracketOpen(i_ + 1, cs, close)) {
                    size_t e = s_.find(close, cs);
                    size_t stop = e == std::string::npos ? s_.size() : e + close.size();
                    while (i_ < stop) adv();
                    continue;
                }
                while (i_ < s_.size() && s_[i_] != '\n') i_++;
            } else {
                break;
            }
        }
    }

    bool parseArgs(std::vector<Arg>& out, std::string& err) {
        int depth = 0;
        while (true) {
            skipSpaceAndComments();
            if (i_ >= s_.size()) {
                err = "unterminated command at end of file";
                return false;
            }
            char ch = s_[i_];
            if (ch == ')') {
                i_++;
                if (depth == 0) return true;
                depth--;
                out.push_back({")", false});
                continue;
            }
            if (ch == '(') {
                i_++;
                depth++;
                out.push_back({"(", false});
                continue;
            }
            if (ch == '"') {
                i_++;
                std::string t;
                while (i_ < s_.size() && s_[i_] != '"') {
                    if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                        char n = s_[i_ + 1];
                        if (n == 'n') { t += '\n'; i_ += 2; }
                        else if (n == 't') { t += '\t'; i_ += 2; }
                        else if (n == '"' || n == '\\') { t += n; i_ += 2; }
                        else if (n == '\n') { line_++; i_ += 2; }
                        else { t += '\\'; t += n; i_ += 2; }  // keeps \$ \; \@ for later stages
                    } else {
                        if (s_[i_] == '\n') line_++;
                        t += s_[i_++];
                    }
                }
                if (i_ < s_.size()) i_++;
                out.push_back({t, true});
                continue;
            }
            size_t cs;
            std::string close;
            if (bracketOpen(i_, cs, close)) {
                size_t e = s_.find(close, cs);
                std::string t = s_.substr(cs, (e == std::string::npos ? s_.size() : e) - cs);
                size_t stop = e == std::string::npos ? s_.size() : e + close.size();
                while (i_ < stop) adv();
                if (!t.empty() && t[0] == '\n') t.erase(0, 1);
                out.push_back({t, true});
                continue;
            }
            std::string t;
            while (i_ < s_.size()) {
                char d = s_[i_];
                if (std::isspace((unsigned char)d) || d == '(' || d == ')') break;
                if (d == '"') {
                    i_++;
                    while (i_ < s_.size() && s_[i_] != '"') {
                        if (s_[i_] == '\\' && i_ + 1 < s_.size()) { t += s_[i_]; t += s_[i_ + 1]; i_ += 2; continue; }
                        if (s_[i_] == '\n') line_++;
                        t += s_[i_++];
                    }
                    if (i_ < s_.size()) i_++;
                    continue;
                }
                if (d == '\\' && i_ + 1 < s_.size()) {
                    char n = s_[i_ + 1];
                    if (n == ';' || n == '$' || n == '@') { t += d; t += n; }
                    else t += n;
                    i_ += 2;
                    continue;
                }
                t += d;
                i_++;
            }
            out.push_back({t, false});
        }
    }
};

// --------------------------------------------------------------- helpers ---
std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> r;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == ';') { cur += ';'; ++i; }
        else if (s[i] == ';') { if (!cur.empty()) r.push_back(cur); cur.clear(); }
        else cur += s[i];
    }
    if (!cur.empty()) r.push_back(cur);
    return r;
}
std::string joinList(const std::vector<std::string>& v, const std::string& glue = ";") {
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) { if (i) r += glue; r += v[i]; }
    return r;
}
size_t matchClose(const std::string& s, size_t from, char open, char close) {
    int depth = 1;
    for (size_t i = from; i < s.size(); ++i) {
        if (s[i] == open) depth++;
        else if (s[i] == close && --depth == 0) return i;
    }
    return std::string::npos;
}
bool wildMatch(const char* p, const char* t) {
    if (!*p) return !*t;
    if (*p == '*') {
        for (const char* q = t;; ++q) {
            if (wildMatch(p + 1, q)) return true;
            if (!*q) return false;
        }
    }
    if (*t && (*p == '?' || lower(std::string(1, *p)) == lower(std::string(1, *t)))) return wildMatch(p + 1, t + 1);
    return false;
}
bool falseConst(const std::string& v) {
    std::string u = upper(v);
    if (u.empty() || u == "0" || u == "OFF" || u == "NO" || u == "FALSE" || u == "N" || u == "IGNORE" ||
        u == "NOTFOUND")
        return true;
    return u.size() >= 9 && u.compare(u.size() - 9, 9, "-NOTFOUND") == 0;
}
bool trueConst(const std::string& v) {
    std::string u = upper(v);
    if (u == "1" || u == "ON" || u == "YES" || u == "TRUE" || u == "Y") return true;
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    return end && *end == 0 && !v.empty() && d != 0;
}
std::vector<int> verParts(const std::string& s) {
    std::vector<int> r;
    std::stringstream ss(s);
    std::string p;
    while (std::getline(ss, p, '.')) r.push_back(std::atoi(p.c_str()));
    return r;
}
int verCmp(const std::string& a, const std::string& b) {
    auto x = verParts(a), y = verParts(b);
    size_t n = std::max(x.size(), y.size());
    x.resize(n);
    y.resize(n);
    return x < y ? -1 : (x == y ? 0 : 1);
}

// ------------------------------------------------------------- the model ---
enum class Kind { Exe, Lib, Iface, Imported };
struct Target {
    std::string name;
    Kind kind = Kind::Lib;
    fs::path dir;
    std::vector<fs::path> sources;
    std::vector<fs::path> inc, incIface;
    std::vector<std::string> defs, defsIface, opts, optsIface;
    std::vector<fs::path> libDirs;
    std::vector<std::string> links;
    std::string cxxStd, cStd;
};
struct UserFunc {
    std::vector<std::string> params;
    std::vector<Cmd> body;
    bool macro = false;
};
enum class Flow { None, Return, Break, Continue };

const std::set<std::string>& linkKeywords() {
    static const std::set<std::string> k = {"PUBLIC", "PRIVATE", "INTERFACE", "LINK_PUBLIC", "LINK_PRIVATE",
                                            "general", "optimized", "debug"};
    return k;
}

class Interp {
public:
    Interp(ProjectSpec& spec) : spec_(spec) {}

    void run(const fs::path& file, const std::string& wanted) {
        rootDir_ = normalize(file).parent_path();
        curDir_ = rootDir_;
        scopes_.emplace_back();
        setVar("CMAKE_SOURCE_DIR", gen(rootDir_));
        setVar("CMAKE_CURRENT_SOURCE_DIR", gen(rootDir_));
        setVar("PROJECT_SOURCE_DIR", gen(rootDir_));
        setVar("CMAKE_BINARY_DIR", gen(rootDir_ / ".cruntime-build"));
        setVar("CMAKE_CURRENT_BINARY_DIR", gen(rootDir_ / ".cruntime-build"));
        setVar("PROJECT_BINARY_DIR", gen(rootDir_ / ".cruntime-build"));
        setVar("CMAKE_CURRENT_LIST_DIR", gen(rootDir_));
        setVar("CMAKE_CURRENT_LIST_FILE", gen(normalize(file)));
        setVar("WIN32", "1");
        setVar("CMAKE_HOST_WIN32", "1");
        setVar("CMAKE_SYSTEM_NAME", "Windows");
        setVar("CMAKE_SYSTEM_PROCESSOR", "AMD64");
        setVar("CMAKE_C_COMPILER_ID", "Clang");
        setVar("CMAKE_CXX_COMPILER_ID", "Clang");
        setVar("CMAKE_CXX_COMPILER_VERSION", "18.1");
        setVar("CMAKE_SIZEOF_VOID_P", "8");
        setVar("CMAKE_BUILD_TYPE", "Release");
        setVar("CMAKE_VERSION", "3.28.0");
        setVar("BUILD_SHARED_LIBS", "OFF");
        includeFile(normalize(file), true);
        resolve(wanted);
    }

private:
    ProjectSpec& spec_;
    fs::path rootDir_, curDir_;
    std::vector<std::map<std::string, std::string>> scopes_;
    std::map<std::string, Target> targets_;
    std::map<std::string, std::string> aliases_;
    std::vector<std::string> order_;
    std::map<std::string, UserFunc> funcs_;
    Flow flow_ = Flow::None;
    std::vector<fs::path> gInc_, gLibDirs_;
    std::vector<std::string> gDefs_, gOpts_, gLibs_;
    int depth_ = 0;

    static std::string gen(const fs::path& p) { return p.generic_string(); }
    void warn(const std::string& m) { spec_.warnings.push_back(m); }

    // ---- variables
    bool hasVar(const std::string& n) const { return scopes_.back().count(n) > 0; }
    std::string getVar(const std::string& n) const {
        auto it = scopes_.back().find(n);
        return it == scopes_.back().end() ? std::string() : it->second;
    }
    void setVar(const std::string& n, const std::string& v) { scopes_.back()[n] = v; }
    void unsetVar(const std::string& n) { scopes_.back().erase(n); }

    // ---- expansion
    std::string genex(const std::string& in) {
        size_t c = in.find(':');
        std::string head = c == std::string::npos ? in : in.substr(0, c);
        std::string tail = c == std::string::npos ? "" : in.substr(c + 1);
        if (head == "BUILD_INTERFACE" || head == "LINK_ONLY" || head == "1") return tail;
        if (head == "0" || head == "INSTALL_INTERFACE") return "";
        if (head == "CONFIG") return lower(tail).find("release") != std::string::npos ? "1" : "0";
        if (head == "CXX_COMPILER_ID" || head == "C_COMPILER_ID") return tail.find("Clang") != std::string::npos ? "1" : "0";
        if (head == "PLATFORM_ID") return tail.find("Windows") != std::string::npos ? "1" : "0";
        if (head == "COMPILE_LANGUAGE") return tail == "CXX" ? "1" : "0";
        if (head == "BOOL") return (trueConst(tail) ? "1" : "0");
        if (head == "NOT") return tail == "1" ? "0" : "1";
        auto parts = [&](const std::string& t) {
            std::vector<std::string> v;
            std::string cur;
            for (char ch : t) { if (ch == ',') { v.push_back(cur); cur.clear(); } else cur += ch; }
            v.push_back(cur);
            return v;
        };
        if (head == "IF") { auto p = parts(tail); return p.size() == 3 ? (p[0] == "1" ? p[1] : p[2]) : ""; }
        if (head == "AND") { for (auto& x : parts(tail)) if (x != "1") return "0"; return "1"; }
        if (head == "OR") { for (auto& x : parts(tail)) if (x == "1") return "1"; return "0"; }
        if (head == "STREQUAL") { auto p = parts(tail); return p.size() == 2 && p[0] == p[1] ? "1" : "0"; }
        if (head == "EQUAL") { auto p = parts(tail); return p.size() == 2 && p[0] == p[1] ? "1" : "0"; }
        if (head == "TARGET_EXISTS") return findTarget(tail) ? "1" : "0";
        if (head == "JOIN") { auto p = parts(tail); std::string r; for (size_t i = 1; i < p.size(); ++i) r += (i > 1 ? p[0] : "") + p[i]; return r; }
        return "";
    }
    std::string expand(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size();) {
            char c = s[i];
            if (c == '\\' && i + 1 < s.size() && (s[i + 1] == '$' || s[i + 1] == '@')) { out += s[i + 1]; i += 2; continue; }
            if (c == '$' && s.compare(i, 5, "$ENV{") == 0) {
                size_t e = matchClose(s, i + 5, '{', '}');
                if (e != std::string::npos) {
                    const char* v = std::getenv(expand(s.substr(i + 5, e - (i + 5))).c_str());
                    if (v) out += v;
                    i = e + 1;
                    continue;
                }
            }
            if (c == '$' && i + 1 < s.size() && s[i + 1] == '{') {
                size_t e = matchClose(s, i + 2, '{', '}');
                if (e != std::string::npos) {
                    out += getVar(expand(s.substr(i + 2, e - (i + 2))));
                    i = e + 1;
                    continue;
                }
            }
            if (c == '$' && i + 1 < s.size() && s[i + 1] == '<') {
                size_t e = matchClose(s, i + 2, '<', '>');
                if (e != std::string::npos) {
                    out += genex(expand(s.substr(i + 2, e - (i + 2))));
                    i = e + 1;
                    continue;
                }
            }
            out += c;
            i++;
        }
        return out;
    }
    std::vector<Tok> evalToks(const std::vector<Arg>& args) {
        std::vector<Tok> r;
        for (const auto& a : args) {
            std::string e = expand(a.text);
            if (a.quoted) r.push_back({e, true});
            else for (auto& p : splitList(e)) r.push_back({p, false});
        }
        return r;
    }
    std::vector<std::string> evalArgs(const std::vector<Arg>& args) {
        std::vector<std::string> r;
        for (auto& t : evalToks(args)) r.push_back(t.t);
        return r;
    }

    // ---- conditions
    struct Cond {
        Interp& I;
        std::vector<Tok> t;
        size_t p = 0;
        bool atEnd() const { return p >= t.size(); }
        bool isKw(const char* k) const { return !atEnd() && !t[p].q && t[p].t == k; }
        std::string val(const Tok& k) const { return (!k.q && I.hasVar(k.t)) ? I.getVar(k.t) : k.t; }
        bool truthy(const Tok& k) const {
            if (trueConst(k.t) && (k.q || !I.hasVar(k.t))) return true;
            if (k.q) return false;
            if (falseConst(k.t) && !I.hasVar(k.t)) return false;
            if (I.hasVar(k.t)) return !falseConst(I.getVar(k.t));
            return false;
        }
        bool parseOr() {
            bool v = parseAnd();
            while (isKw("OR")) { p++; bool r = parseAnd(); v = v || r; }
            return v;
        }
        bool parseAnd() {
            bool v = parseNot();
            while (isKw("AND")) { p++; bool r = parseNot(); v = v && r; }
            return v;
        }
        bool parseNot() {
            if (isKw("NOT")) { p++; return !parseNot(); }
            return parsePrimary();
        }
        bool parsePrimary() {
            if (atEnd()) return false;
            if (isKw("(")) {
                p++;
                bool v = parseOr();
                if (isKw(")")) p++;
                return v;
            }
            if (!t[p].q) {
                const std::string& k = t[p].t;
                if ((k == "EXISTS" || k == "IS_DIRECTORY" || k == "DEFINED" || k == "COMMAND" || k == "TARGET" ||
                     k == "IS_ABSOLUTE" || k == "POLICY") && p + 1 < t.size()) {
                    std::string op = k;
                    Tok a = t[p + 1];
                    p += 2;
                    std::error_code ec;
                    if (op == "EXISTS") return fs::exists(fs::path(a.t), ec);
                    if (op == "IS_DIRECTORY") return fs::is_directory(fs::path(a.t), ec);
                    if (op == "IS_ABSOLUTE") return fs::path(a.t).is_absolute();
                    if (op == "DEFINED") {
                        if (a.t.rfind("ENV{", 0) == 0 && a.t.back() == '}') return std::getenv(a.t.substr(4, a.t.size() - 5).c_str()) != nullptr;
                        return I.hasVar(a.t);
                    }
                    if (op == "TARGET") return I.targets_.count(a.t) || I.aliases_.count(a.t);
                    if (op == "COMMAND") return I.funcs_.count(lower(a.t)) > 0;
                    return false;
                }
            }
            Tok lhs = t[p++];
            if (!atEnd() && !t[p].q) {
                const std::string op = t[p].t;
                static const std::set<std::string> bin = {
                    "STREQUAL", "EQUAL", "LESS", "GREATER", "LESS_EQUAL", "GREATER_EQUAL", "MATCHES", "STRLESS",
                    "STRGREATER", "VERSION_LESS", "VERSION_GREATER", "VERSION_EQUAL", "VERSION_LESS_EQUAL",
                    "VERSION_GREATER_EQUAL", "IN_LIST"};
                if (bin.count(op) && p + 1 < t.size()) {
                    Tok rhs = t[p + 1];
                    p += 2;
                    std::string a = val(lhs), b = val(rhs);
                    if (op == "STREQUAL") return a == b;
                    if (op == "STRLESS") return a < b;
                    if (op == "STRGREATER") return a > b;
                    if (op == "EQUAL") return std::atof(a.c_str()) == std::atof(b.c_str());
                    if (op == "LESS") return std::atof(a.c_str()) < std::atof(b.c_str());
                    if (op == "GREATER") return std::atof(a.c_str()) > std::atof(b.c_str());
                    if (op == "LESS_EQUAL") return std::atof(a.c_str()) <= std::atof(b.c_str());
                    if (op == "GREATER_EQUAL") return std::atof(a.c_str()) >= std::atof(b.c_str());
                    if (op == "VERSION_LESS") return verCmp(a, b) < 0;
                    if (op == "VERSION_GREATER") return verCmp(a, b) > 0;
                    if (op == "VERSION_EQUAL") return verCmp(a, b) == 0;
                    if (op == "VERSION_LESS_EQUAL") return verCmp(a, b) <= 0;
                    if (op == "VERSION_GREATER_EQUAL") return verCmp(a, b) >= 0;
                    if (op == "MATCHES") {
                        try { return std::regex_search(a, std::regex(b)); } catch (...) { return false; }
                    }
                    if (op == "IN_LIST") {
                        for (auto& x : splitList(I.getVar(rhs.t))) if (x == a) return true;
                        return false;
                    }
                }
            }
            return truthy(lhs);
        }
    };
    bool evalCond(const Cmd& c) {
        Cond cd{*this, evalToks(c.args)};
        return cd.parseOr();
    }

    // ---- block helpers
    static size_t findEnd(const std::vector<Cmd>& v, size_t start, const char* open, const char* close) {
        int depth = 0;
        for (size_t i = start; i < v.size(); ++i) {
            std::string n = lower(v[i].name);
            if (n == open) depth++;
            else if (n == close && --depth == 0) return i;
        }
        throw CMakeError(std::string("unterminated ") + open + "() block at line " + std::to_string(v[start].line));
    }

    void exec(const std::vector<Cmd>& v, size_t b, size_t e) {
        for (size_t i = b; i < e && flow_ == Flow::None; ++i) {
            const Cmd& c = v[i];
            std::string n = lower(c.name);
            try {
                if (n == "if") i = execIf(v, i);
                else if (n == "foreach") i = execForeach(v, i);
                else if (n == "while") i = execWhile(v, i);
                else if (n == "function" || n == "macro") i = defineFunc(v, i, n == "macro");
                else if (n == "return") flow_ = Flow::Return;
                else if (n == "break") flow_ = Flow::Break;
                else if (n == "continue") flow_ = Flow::Continue;
                else command(c, n);
            } catch (CMakeError& ex) {
                if (ex.located) throw;
                throw CMakeError(gen(curFile_) + ":" + std::to_string(c.line) + ": " + ex.what(), true);
            }
        }
    }
    fs::path curFile_;

    size_t execIf(const std::vector<Cmd>& v, size_t i) {
        size_t end = findEnd(v, i, "if", "endif");
        struct Br { const Cmd* cond; size_t b, e; };
        std::vector<Br> brs;
        brs.push_back({&v[i], i + 1, 0});
        int depth = 0;
        for (size_t j = i + 1; j < end; ++j) {
            std::string n = lower(v[j].name);
            if (n == "if") depth++;
            else if (n == "endif") depth--;
            else if (depth == 0 && (n == "elseif" || n == "else")) {
                brs.back().e = j;
                brs.push_back({n == "elseif" ? &v[j] : nullptr, j + 1, 0});
            }
        }
        brs.back().e = end;
        for (auto& br : brs) {
            bool take = br.cond ? evalCond(*br.cond) : true;
            if (take) { exec(v, br.b, br.e); break; }
        }
        return end;
    }
    size_t execForeach(const std::vector<Cmd>& v, size_t i) {
        size_t end = findEnd(v, i, "foreach", "endforeach");
        auto toks = evalArgs(v[i].args);
        if (toks.empty()) return end;
        std::string var = toks[0];
        std::vector<std::string> items;
        if (toks.size() > 1 && toks[1] == "RANGE") {
            int a = 0, b = 0, st = 1;
            if (toks.size() == 3) b = std::atoi(toks[2].c_str());
            else if (toks.size() >= 4) { a = std::atoi(toks[2].c_str()); b = std::atoi(toks[3].c_str()); if (toks.size() > 4) st = std::max(1, std::atoi(toks[4].c_str())); }
            for (int k = a; k <= b; k += st) items.push_back(std::to_string(k));
        } else if (toks.size() > 1 && toks[1] == "IN") {
            int mode = 0;
            for (size_t k = 2; k < toks.size(); ++k) {
                if (toks[k] == "LISTS") mode = 1;
                else if (toks[k] == "ITEMS") mode = 2;
                else if (mode == 1) for (auto& x : splitList(getVar(toks[k]))) items.push_back(x);
                else items.push_back(toks[k]);
            }
        } else {
            for (size_t k = 1; k < toks.size(); ++k) items.push_back(toks[k]);
        }
        for (auto& it : items) {
            setVar(var, it);
            exec(v, i + 1, end);
            if (flow_ == Flow::Break) { flow_ = Flow::None; break; }
            if (flow_ == Flow::Continue) flow_ = Flow::None;
            if (flow_ == Flow::Return) break;
        }
        return end;
    }
    size_t execWhile(const std::vector<Cmd>& v, size_t i) {
        size_t end = findEnd(v, i, "while", "endwhile");
        for (int guard = 0; guard < 100000 && evalCond(v[i]); ++guard) {
            exec(v, i + 1, end);
            if (flow_ == Flow::Break) { flow_ = Flow::None; break; }
            if (flow_ == Flow::Continue) flow_ = Flow::None;
            if (flow_ == Flow::Return) break;
        }
        return end;
    }
    size_t defineFunc(const std::vector<Cmd>& v, size_t i, bool macro) {
        const char* open = macro ? "macro" : "function";
        size_t end = findEnd(v, i, open, macro ? "endmacro" : "endfunction");
        std::vector<std::string> raw;
        for (auto& a : v[i].args) raw.push_back(a.text);
        if (raw.empty()) return end;
        UserFunc f;
        f.macro = macro;
        f.params.assign(raw.begin() + 1, raw.end());
        f.body.assign(v.begin() + i + 1, v.begin() + end);
        funcs_[lower(raw[0])] = std::move(f);
        return end;
    }
    void callUser(const UserFunc& f, const std::vector<std::string>& A) {
        if (++depth_ > 200) throw CMakeError("function recursion too deep");
        if (!f.macro) scopes_.push_back(scopes_.back());
        setVar("ARGC", std::to_string(A.size()));
        setVar("ARGV", joinList(A));
        std::vector<std::string> rest;
        for (size_t i = 0; i < A.size(); ++i) {
            setVar("ARGV" + std::to_string(i), A[i]);
            if (i < f.params.size()) setVar(f.params[i], A[i]);
            else rest.push_back(A[i]);
        }
        setVar("ARGN", joinList(rest));
        exec(f.body, 0, f.body.size());
        if (flow_ == Flow::Return) flow_ = Flow::None;
        if (!f.macro) scopes_.pop_back();
        depth_--;
    }

    // ---- file inclusion
    void includeFile(const fs::path& file, bool top = false) {
        std::string text;
        if (!readTextFile(file, text)) throw CMakeError("cannot read " + gen(file));
        std::vector<Cmd> cmds;
        std::string perr;
        Parser ps(text);
        if (!ps.parse(cmds, perr)) throw CMakeError(gen(file) + ": " + perr, true);
        fs::path saveFile = curFile_;
        std::string saveList = getVar("CMAKE_CURRENT_LIST_DIR"), saveListFile = getVar("CMAKE_CURRENT_LIST_FILE");
        curFile_ = file;
        setVar("CMAKE_CURRENT_LIST_DIR", gen(file.parent_path()));
        setVar("CMAKE_CURRENT_LIST_FILE", gen(file));
        exec(cmds, 0, cmds.size());
        if (flow_ == Flow::Return && !top) flow_ = Flow::None;
        flow_ = Flow::None;
        setVar("CMAKE_CURRENT_LIST_DIR", saveList);
        setVar("CMAKE_CURRENT_LIST_FILE", saveListFile);
        curFile_ = saveFile;
    }
    void addSubdirectory(const std::string& dir) {
        fs::path d = fs::path(dir).is_absolute() ? fs::path(dir) : curDir_ / dir;
        d = normalize(d);
        fs::path f = d / "CMakeLists.txt";
        std::error_code ec;
        if (!fs::exists(f, ec)) f = d / "CMake.txt";
        if (!fs::exists(f, ec)) throw CMakeError("add_subdirectory: no CMakeLists.txt in " + gen(d));
        fs::path saveDir = curDir_;
        scopes_.push_back(scopes_.back());
        curDir_ = d;
        setVar("CMAKE_CURRENT_SOURCE_DIR", gen(d));
        includeFile(f);
        scopes_.pop_back();
        curDir_ = saveDir;
    }

    // ---- paths / targets
    static bool cmTrue(std::string v) {
        for (auto& ch : v) ch = (char)std::toupper((unsigned char)ch);
        if (v.empty() || v == "0" || v == "OFF" || v == "NO" || v == "FALSE" || v == "N" || v == "IGNORE" || v == "NOTFOUND") return false;
        return !(v.size() > 9 && v.compare(v.size() - 9, 9, "-NOTFOUND") == 0);
    }
    std::string substVars(const std::string& line, bool atOnly) {
        static const std::regex at(R"(@([A-Za-z0-9_./+\-]+)@)");
        static const std::regex dl(R"(\$\{([A-Za-z0-9_./+\-]+)\})");
        auto apply = [&](const std::string& in, const std::regex& re) {
            std::string out;
            auto b = in.cbegin();
            for (std::sregex_iterator it(in.begin(), in.end(), re), end; it != end; ++it) {
                out.append(b, in.cbegin() + it->position());
                out += getVar((*it)[1].str());
                b = in.cbegin() + it->position() + it->length();
            }
            out.append(b, in.cend());
            return out;
        };
        std::string r = apply(line, at);
        return atOnly ? r : apply(r, dl);
    }
    // configure_file(<in> <out> [COPYONLY] [@ONLY]): the result lives in memory only (spec_.virtualFiles) and is served
    // to Clang through an in-memory file system overlay.
    void configureFile(const std::vector<std::string>& A) {
        if (A.size() < 2) return;
        bool copyOnly = false, atOnly = false;
        for (size_t i = 2; i < A.size(); ++i) { if (A[i] == "COPYONLY") copyOnly = true; else if (A[i] == "@ONLY") atOnly = true; }
        fs::path in = absPath(A[0]);
        fs::path out(A[1]);
        if (!out.is_absolute()) out = fs::path(getVar("CMAKE_CURRENT_BINARY_DIR")) / out;
        out = normalize(out);
        std::ifstream f(in, std::ios::binary);
        if (!f) { warn("configure_file: cannot read " + gen(in)); return; }
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!copyOnly) {
            static const std::regex def(R"(^(\s*)#cmakedefine(01)?\s+([A-Za-z0-9_]+)(.*)$)");
            std::string res, line;
            std::istringstream ss(text);
            while (std::getline(ss, line)) {
                bool cr = !line.empty() && line.back() == '\r';
                if (cr) line.pop_back();
                std::smatch m;
                if (std::regex_match(line, m, def)) {
                    bool on = cmTrue(getVar(m[3].str()));
                    if (m[2].matched && m[2].length()) line = m[1].str() + "#define " + m[3].str() + (on ? " 1" : " 0");
                    else line = on ? m[1].str() + "#define " + m[3].str() + substVars(m[4].str(), atOnly)
                                   : m[1].str() + "/* #undef " + m[3].str() + " */";
                } else {
                    line = substVars(line, atOnly);
                }
                res += line + (cr ? "\r\n" : "\n");
            }
            text = res;
        }
        spec_.virtualFiles.emplace_back(out, std::move(text));
    }

    fs::path absPath(const std::string& s) const {
        fs::path p(s);
        return normalize(p.is_absolute() ? p : curDir_ / p);
    }
    Target* findTarget(const std::string& n) {
        auto a = aliases_.find(n);
        auto it = targets_.find(a != aliases_.end() ? a->second : n);
        return it == targets_.end() ? nullptr : &it->second;
    }
    Target& makeTarget(const std::string& n, Kind k) {
        if (!targets_.count(n)) order_.push_back(n);
        Target& t = targets_[n];
        t.name = n;
        t.kind = k;
        return t;
    }
    void addSources(Target& t, const std::vector<std::string>& A, size_t from) {
        static const std::set<std::string> skip = {"STATIC", "SHARED", "MODULE", "OBJECT", "INTERFACE", "WIN32",
                                                    "MACOSX_BUNDLE", "EXCLUDE_FROM_ALL", "UNKNOWN", "GLOBAL",
                                                    "PUBLIC", "PRIVATE"};
        for (size_t i = from; i < A.size(); ++i) {
            if (skip.count(A[i]) || A[i].empty()) continue;
            Lang l;
            fs::path p = absPath(A[i]);
            if (sourceLang(p, l)) t.sources.push_back(p);
        }
    }
    void addByScope(Target& t, const std::string& scope, const std::vector<std::string>& items, bool incs, bool defs,
                    bool opts) {
        for (auto& it : items) {
            if (incs) {
                fs::path p = absPath(it);
                if (scope != "INTERFACE") t.inc.push_back(p);
                if (scope != "PRIVATE") t.incIface.push_back(p);
            } else if (defs) {
                std::string d = it.rfind("-D", 0) == 0 ? it.substr(2) : it;
                if (scope != "INTERFACE") t.defs.push_back(d);
                if (scope != "PRIVATE") t.defsIface.push_back(d);
            } else if (opts) {
                if (scope != "INTERFACE") t.opts.push_back(it);
                if (scope != "PRIVATE") t.optsIface.push_back(it);
            }
        }
    }
    void scoped(const std::vector<std::string>& A, bool incs, bool defs, bool opts) {
        if (A.empty()) return;
        Target* t = findTarget(A[0]);
        if (!t) { warn("unknown target '" + A[0] + "' (ignored)"); return; }
        std::string scope = "PUBLIC";
        if (t->kind == Kind::Iface) scope = "INTERFACE";
        else if (t->kind == Kind::Exe) scope = "PRIVATE";
        std::vector<std::string> items;
        auto flush = [&]() { addByScope(*t, scope, items, incs, defs, opts); items.clear(); };
        for (size_t i = 1; i < A.size(); ++i) {
            if (A[i] == "PUBLIC" || A[i] == "PRIVATE" || A[i] == "INTERFACE") { flush(); scope = A[i]; }
            else if (A[i] == "SYSTEM" || A[i] == "BEFORE" || A[i] == "AFTER") continue;
            else items.push_back(A[i]);
        }
        flush();
    }

    void globFiles(const std::string& pattern, bool recurse, std::vector<std::string>& out) {
        fs::path p = fs::path(pattern).is_absolute() ? fs::path(pattern) : curDir_ / pattern;
        fs::path dir = p.parent_path();
        std::string pat = u8(p.filename());
        std::error_code ec;
        std::vector<std::string> found;
        if (recurse) {
            for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && wildMatch(pat.c_str(), u8(it->path().filename()).c_str()))
                    found.push_back(it->path().generic_string());
        } else {
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && wildMatch(pat.c_str(), u8(it->path().filename()).c_str()))
                    found.push_back(it->path().generic_string());
        }
        std::sort(found.begin(), found.end());
        for (auto& f : found) out.push_back(f);
    }

    // ---- commands
    void command(const Cmd& c, const std::string& n) {
        std::vector<std::string> A = evalArgs(c.args);

        auto setOut = [&](const std::string& var, const std::string& val) { setVar(var, val); };

        if (n == "set") {
            if (A.empty()) return;
            const std::string& name = A[0];
            if (name.rfind("ENV{", 0) == 0) return;
            bool parent = false, cache = false, force = false;
            std::vector<std::string> vals;
            for (size_t i = 1; i < A.size(); ++i) {
                if (A[i] == "PARENT_SCOPE") parent = true;
                else if (A[i] == "CACHE") { cache = true; for (size_t j = i + 1; j < A.size(); ++j) if (A[j] == "FORCE") force = true; break; }
                else vals.push_back(A[i]);
            }
            if (cache) {
                if (!vals.empty() && (force || !hasVar(name))) setVar(name, joinList(vals));
                return;
            }
            if (parent) {
                if (scopes_.size() >= 2) {
                    if (vals.empty()) scopes_[scopes_.size() - 2].erase(name);
                    else scopes_[scopes_.size() - 2][name] = joinList(vals);
                }
                return;
            }
            if (vals.empty()) unsetVar(name);
            else setVar(name, joinList(vals));
        } else if (n == "unset") {
            if (!A.empty()) unsetVar(A[0]);
        } else if (n == "option") {
            if (!A.empty() && !hasVar(A[0])) setVar(A[0], A.size() > 2 ? A[2] : "OFF");
        } else if (n == "message") {
            if (A.empty()) return;
            std::string mode = A[0];
            std::vector<std::string> rest(A.begin() + (mode == "STATUS" || mode == "WARNING" || mode == "FATAL_ERROR" || mode == "NOTICE" || mode == "AUTHOR_WARNING" || mode == "SEND_ERROR" || mode == "DEPRECATION" || mode == "VERBOSE" || mode == "DEBUG" || mode == "TRACE" ? 1 : 0), A.end());
            std::string text = joinList(rest, "");
            if (mode == "FATAL_ERROR" || mode == "SEND_ERROR") throw CMakeError("message(" + mode + "): " + text);
            if (mode == "WARNING" || mode == "AUTHOR_WARNING" || mode == "NOTICE") warn("cmake: " + text);
        } else if (n == "cmake_minimum_required" || n == "cmake_policy" || n == "enable_language" ||
                   n == "mark_as_advanced" || n == "include_guard" || n == "enable_testing" || n == "add_test" ||
                   n == "install" || n == "set_property" || n == "set_directory_properties" ||
                   n == "source_group" || n == "add_dependencies" || n == "target_precompile_headers" ||
                   n == "target_compile_features" || n == "set_source_files_properties" || n == "export" ||
                   n == "define_property" || n == "get_property" || n == "set_tests_properties" ||
                   n == "include_external_msproject" || n == "cmake_language" || n == "get_cmake_property" ||
                   n == "get_target_property" || n == "get_directory_property") {
            // no effect on in-memory execution
        } else if (n == "project") {
            if (!A.empty()) {
                setVar("PROJECT_NAME", A[0]);
                setVar("CMAKE_PROJECT_NAME", A[0]);
                setVar(A[0] + "_SOURCE_DIR", gen(curDir_));
                setVar("PROJECT_SOURCE_DIR", gen(curDir_));
                if (spec_.name.empty()) spec_.name = A[0];
            }
        } else if (n == "add_custom_target" || n == "add_custom_command" || n == "add_library_dummy") {
            warn(c.name + "() ignored (would generate files on disk)");
        } else if (n == "configure_file") {
            configureFile(A);
        } else if (n == "include") {
            if (A.empty()) return;
            fs::path p = absPath(A[0]);
            std::error_code ec;
            if (fs::is_regular_file(p, ec)) includeFile(p);
        } else if (n == "add_subdirectory") {
            if (!A.empty()) addSubdirectory(A[0]);
        } else if (n == "find_package") {
            if (A.empty()) return;
            std::string pkg = A[0];
            bool required = std::find(A.begin(), A.end(), "REQUIRED") != A.end();
            if (pkg == "Threads") { setVar("Threads_FOUND", "TRUE"); setVar("CMAKE_THREAD_LIBS_INIT", ""); return; }
            setVar(pkg + "_FOUND", "FALSE");
            setVar(upper(pkg) + "_FOUND", "FALSE");
            warn("find_package(" + pkg + ") is not supported" + (required ? " [REQUIRED]" : "") +
                 "; supply headers/libs with -I / -L / -l");
        } else if (n == "find_library" || n == "find_path" || n == "find_program" || n == "find_file") {
            if (!A.empty()) setVar(A[0], A[0] + "-NOTFOUND");
        } else if (n == "include_directories") {
            for (auto& a : A) if (a != "SYSTEM" && a != "BEFORE" && a != "AFTER") gInc_.push_back(absPath(a));
        } else if (n == "link_directories") {
            for (auto& a : A) if (a != "BEFORE" && a != "AFTER") gLibDirs_.push_back(absPath(a));
        } else if (n == "link_libraries") {
            for (auto& a : A) if (!linkKeywords().count(a)) gLibs_.push_back(a);
        } else if (n == "add_definitions" || n == "add_compile_definitions") {
            for (auto& a : A) {
                if (a.rfind("-D", 0) == 0) gDefs_.push_back(a.substr(2));
                else if (n == "add_compile_definitions") gDefs_.push_back(a);
                else gOpts_.push_back(a);
            }
        } else if (n == "add_compile_options") {
            for (auto& a : A) gOpts_.push_back(a);
        } else if (n == "add_executable") {
            if (A.empty()) return;
            if (A.size() >= 3 && A[1] == "ALIAS") { aliases_[A[0]] = A[2]; return; }
            Target& t = makeTarget(A[0], Kind::Exe);
            t.dir = curDir_;
            addSources(t, A, 1);
        } else if (n == "add_library") {
            if (A.empty()) return;
            if (A.size() >= 3 && A[1] == "ALIAS") { aliases_[A[0]] = A[2]; return; }
            bool imported = std::find(A.begin(), A.end(), "IMPORTED") != A.end();
            bool iface = A.size() > 1 && A[1] == "INTERFACE";
            Target& t = makeTarget(A[0], imported ? Kind::Imported : (iface ? Kind::Iface : Kind::Lib));
            t.dir = curDir_;
            if (!imported) addSources(t, A, 1);
        } else if (n == "target_sources") {
            if (A.empty()) return;
            Target* t = findTarget(A[0]);
            if (t) addSources(*t, A, 1);
        } else if (n == "target_include_directories") {
            scoped(A, true, false, false);
        } else if (n == "target_compile_definitions") {
            scoped(A, false, true, false);
        } else if (n == "target_compile_options") {
            scoped(A, false, false, true);
        } else if (n == "target_link_directories") {
            if (A.empty()) return;
            Target* t = findTarget(A[0]);
            if (!t) return;
            for (size_t i = 1; i < A.size(); ++i)
                if (A[i] != "PUBLIC" && A[i] != "PRIVATE" && A[i] != "INTERFACE" && A[i] != "BEFORE")
                    t->libDirs.push_back(absPath(A[i]));
        } else if (n == "target_link_libraries") {
            if (A.empty()) return;
            Target* t = findTarget(A[0]);
            if (!t) { warn("target_link_libraries: unknown target '" + A[0] + "'"); return; }
            for (size_t i = 1; i < A.size(); ++i) {
                if (A[i] == "debug") { i++; continue; }
                if (linkKeywords().count(A[i])) continue;
                if (!A[i].empty() && A[i][0] == '-' ) { if (A[i].rfind("-l", 0) == 0) t->links.push_back(A[i].substr(2)); continue; }
                t->links.push_back(A[i]);
            }
        } else if (n == "set_target_properties") {
            size_t pi = 0;
            for (size_t i = 0; i < A.size(); ++i) if (A[i] == "PROPERTIES") { pi = i; break; }
            if (!pi) return;
            for (size_t k = 0; k < pi; ++k) {
                Target* t = findTarget(A[k]);
                if (!t) continue;
                for (size_t i = pi + 1; i + 1 < A.size(); i += 2) {
                    if (A[i] == "CXX_STANDARD") t->cxxStd = A[i + 1];
                    else if (A[i] == "C_STANDARD") t->cStd = A[i + 1];
                }
            }
        } else if (n == "list") {
            listCmd(A);
        } else if (n == "string") {
            stringCmd(A);
        } else if (n == "get_filename_component") {
            if (A.size() < 3) return;
            fs::path p(A[1]);
            const std::string& comp = A[2];
            std::string r;
            if (comp == "DIRECTORY" || comp == "PATH") r = gen(p.parent_path());
            else if (comp == "NAME") r = u8(p.filename());
            else if (comp == "NAME_WE") { std::string f = u8(p.filename()); size_t d = f.find('.'); r = d == std::string::npos ? f : f.substr(0, d); }
            else if (comp == "NAME_WLE") r = u8(p.stem());
            else if (comp == "EXT") { std::string f = u8(p.filename()); size_t d = f.find('.'); r = d == std::string::npos ? "" : f.substr(d); }
            else if (comp == "LAST_EXT") r = u8(p.extension());
            else if (comp == "ABSOLUTE" || comp == "REALPATH") r = gen(absPath(A[1]));
            setOut(A[0], r);
        } else if (n == "file") {
            fileCmd(A);
        } else if (n == "math") {
            if (A.size() >= 3 && A[0] == "EXPR") {
                std::regex re(R"(^\s*(-?\d+)\s*([+\-*/%])\s*(-?\d+)\s*$)");
                std::smatch m;
                if (std::regex_match(A[2], m, re)) {
                    long long a = std::stoll(m[1]), b = std::stoll(m[3]), r = 0;
                    char op = m[2].str()[0];
                    r = op == '+' ? a + b : op == '-' ? a - b : op == '*' ? a * b : (b ? (op == '/' ? a / b : a % b) : 0);
                    setOut(A[1], std::to_string(r));
                } else if (std::regex_match(A[2], m, std::regex(R"(^\s*-?\d+\s*$)"))) setOut(A[1], trim(A[2]));
                else warn("math(EXPR) supports only 'a op b' expressions");
            }
        } else if (n == "check_cxx_compiler_flag" || n == "check_c_compiler_flag") {
            if (A.size() >= 2) setVar(A[1], "1");
        } else if (funcs_.count(n)) {
            callUser(funcs_[n], A);
        } else {
            warn("unsupported command " + c.name + "() ignored");
        }
    }

    void listCmd(const std::vector<std::string>& A) {
        if (A.size() < 2) return;
        const std::string& sub = A[0];
        const std::string& lv = A[1];
        auto items = splitList(getVar(lv));
        if (sub == "APPEND") { for (size_t i = 2; i < A.size(); ++i) items.push_back(A[i]); setVar(lv, joinList(items)); }
        else if (sub == "PREPEND") { items.insert(items.begin(), A.begin() + 2, A.end()); setVar(lv, joinList(items)); }
        else if (sub == "REMOVE_ITEM") {
            for (size_t i = 2; i < A.size(); ++i) items.erase(std::remove(items.begin(), items.end(), A[i]), items.end());
            setVar(lv, joinList(items));
        } else if (sub == "REMOVE_DUPLICATES") {
            std::vector<std::string> r;
            for (auto& x : items) if (std::find(r.begin(), r.end(), x) == r.end()) r.push_back(x);
            setVar(lv, joinList(r));
        } else if (sub == "LENGTH" && A.size() >= 3) setVar(A[2], std::to_string(items.size()));
        else if (sub == "GET" && A.size() >= 4) {
            std::vector<std::string> r;
            for (size_t i = 2; i + 1 < A.size(); ++i) {
                int k = std::atoi(A[i].c_str());
                if (k < 0) k += (int)items.size();
                if (k >= 0 && k < (int)items.size()) r.push_back(items[k]);
            }
            setVar(A.back(), joinList(r));
        } else if (sub == "FIND" && A.size() >= 4) {
            auto it = std::find(items.begin(), items.end(), A[2]);
            setVar(A[3], it == items.end() ? "-1" : std::to_string(it - items.begin()));
        } else if (sub == "JOIN" && A.size() >= 4) setVar(A[3], joinList(items, A[2]));
        else if (sub == "SORT") { std::sort(items.begin(), items.end()); setVar(lv, joinList(items)); }
        else if (sub == "REVERSE") { std::reverse(items.begin(), items.end()); setVar(lv, joinList(items)); }
        else if (sub == "INSERT" && A.size() >= 4) {
            int k = std::atoi(A[2].c_str());
            if (k < 0) k += (int)items.size();
            k = std::max(0, std::min(k, (int)items.size()));
            items.insert(items.begin() + k, A.begin() + 3, A.end());
            setVar(lv, joinList(items));
        } else if (sub == "FILTER" && A.size() >= 5) {
            bool inc = A[2] == "INCLUDE";
            std::vector<std::string> r;
            try {
                std::regex re(A[4]);
                for (auto& x : items) if (std::regex_search(x, re) == inc) r.push_back(x);
            } catch (...) { r = items; }
            setVar(lv, joinList(r));
        } else warn("list(" + sub + ") is not supported");
    }

    void stringCmd(const std::vector<std::string>& A) {
        if (A.empty()) return;
        const std::string& sub = A[0];
        if (sub == "REPLACE" && A.size() >= 5) {
            std::string in = joinList(std::vector<std::string>(A.begin() + 4, A.end()), "");
            std::string r;
            size_t pos = 0;
            if (A[1].empty()) { setVar(A[3], in); return; }
            for (size_t f; (f = in.find(A[1], pos)) != std::string::npos; pos = f + A[1].size()) { r += in.substr(pos, f - pos); r += A[2]; }
            r += in.substr(pos);
            setVar(A[3], r);
        } else if (sub == "TOUPPER" && A.size() >= 3) setVar(A[2], upper(A[1]));
        else if (sub == "TOLOWER" && A.size() >= 3) setVar(A[2], lower(A[1]));
        else if ((sub == "APPEND" || sub == "PREPEND") && A.size() >= 2) {
            std::string add = joinList(std::vector<std::string>(A.begin() + 2, A.end()), "");
            setVar(A[1], sub == "APPEND" ? getVar(A[1]) + add : add + getVar(A[1]));
        } else if (sub == "CONCAT" && A.size() >= 2) setVar(A[1], joinList(std::vector<std::string>(A.begin() + 2, A.end()), ""));
        else if (sub == "LENGTH" && A.size() >= 3) setVar(A[2], std::to_string(A[1].size()));
        else if (sub == "STRIP" && A.size() >= 3) setVar(A[2], trim(A[1]));
        else if (sub == "SUBSTRING" && A.size() >= 5) {
            size_t b = (size_t)std::max(0, std::atoi(A[2].c_str()));
            int len = std::atoi(A[3].c_str());
            std::string s = b >= A[1].size() ? "" : (len < 0 ? A[1].substr(b) : A[1].substr(b, (size_t)len));
            setVar(A[4], s);
        } else if (sub == "FIND" && A.size() >= 4) {
            size_t f = (A.size() > 4 && A[4] == "REVERSE") ? A[1].rfind(A[2]) : A[1].find(A[2]);
            setVar(A[3], f == std::string::npos ? "-1" : std::to_string(f));
        } else if (sub == "JOIN" && A.size() >= 3) setVar(A[2], joinList(std::vector<std::string>(A.begin() + 3, A.end()), A[1]));
        else if (sub == "COMPARE" && A.size() >= 5) {
            bool eq = A[2] == A[3];
            if (A[1] == "EQUAL") setVar(A[4], eq ? "1" : "0");
            else if (A[1] == "NOTEQUAL") setVar(A[4], eq ? "0" : "1");
        } else if (sub == "REGEX" && A.size() >= 5) {
            try {
                if (A[1] == "REPLACE" && A.size() >= 6) {
                    std::string in = joinList(std::vector<std::string>(A.begin() + 5, A.end()), "");
                    std::string rep = std::regex_replace(A[3], std::regex(R"(\\(\d))"), "$$$1");
                    setVar(A[4], std::regex_replace(in, std::regex(A[2]), rep));
                } else if (A[1] == "MATCH") {
                    std::string in = joinList(std::vector<std::string>(A.begin() + 4, A.end()), "");
                    std::smatch m;
                    setVar(A[3], std::regex_search(in, m, std::regex(A[2])) ? m[0].str() : "");
                }
            } catch (...) { warn("string(REGEX): bad pattern"); }
        } else warn("string(" + sub + ") is not supported");
    }

    void fileCmd(const std::vector<std::string>& A) {
        if (A.size() < 2) return;
        const std::string& sub = A[0];
        if (sub == "GLOB" || sub == "GLOB_RECURSE") {
            std::vector<std::string> out;
            for (size_t i = 2; i < A.size(); ++i) {
                if (A[i] == "CONFIGURE_DEPENDS" || A[i] == "FOLLOW_SYMLINKS" || A[i] == "LIST_DIRECTORIES") continue;
                if (A[i] == "RELATIVE") { i++; continue; }
                globFiles(A[i], sub == "GLOB_RECURSE", out);
            }
            setVar(A[1], joinList(out));
        } else if (sub == "READ" && A.size() >= 3) {
            std::string t;
            readTextFile(absPath(A[1]), t);
            setVar(A[2], t);
        } else if (sub == "TO_CMAKE_PATH" && A.size() >= 3) {
            std::string s = A[1];
            std::replace(s.begin(), s.end(), '\\', '/');
            setVar(A[2], s);
        } else if (sub == "REAL_PATH" && A.size() >= 3) {
            setVar(A[2], gen(absPath(A[1])));
        } else if ((sub == "WRITE" || sub == "APPEND") && A.size() >= 2) {
            // Generated text files live in memory only (served to Clang through the VFS overlay).
            fs::path out(A[1]);
            if (!out.is_absolute()) out = curDir_ / out;
            out = normalize(out);
            std::string text;
            for (size_t i = 2; i < A.size(); ++i) text += A[i];
            bool found = false;
            for (auto& v : spec_.virtualFiles)
                if (v.first == out) { v.second = sub == "APPEND" ? v.second + text : text; found = true; break; }
            if (!found) spec_.virtualFiles.emplace_back(out, std::move(text));
        } else if (sub == "MAKE_DIRECTORY" || sub == "COPY" || sub == "REMOVE" ||
                   sub == "REMOVE_RECURSE" || sub == "RENAME" || sub == "DOWNLOAD" || sub == "GENERATE") {
            warn("file(" + sub + ") ignored (CRuntime never writes to disk)");
        } else warn("file(" + sub + ") is not supported");
    }

    // ---- final resolution of the chosen executable
    static bool looksLikeLibPath(const std::string& s) {
        std::string l = lower(s);
        auto ends = [&](const char* e) { return l.size() >= strlen(e) && l.compare(l.size() - strlen(e), strlen(e), e) == 0; };
        return ends(".lib") || ends(".dll") || ends(".a") || s.find('/') != std::string::npos || s.find('\\') != std::string::npos;
    }

    void resolve(const std::string& wanted) {
        Target* exe = nullptr;
        if (!wanted.empty()) {
            exe = findTarget(wanted);
            if (!exe || exe->kind != Kind::Exe) throw CMakeError("target '" + wanted + "' is not an executable defined by add_executable()");
        } else {
            for (auto& n : order_) if (targets_[n].kind == Kind::Exe) { exe = &targets_[n]; break; }
        }
        if (!exe) throw CMakeError("no add_executable() target found");
        spec_.name = exe->name;
        spec_.root = rootDir_;

        std::set<std::string> seen;
        auto addUnique = [](auto& dst, const auto& v) {
            if (std::find(dst.begin(), dst.end(), v) == dst.end()) dst.push_back(v);
        };
        std::function<void(Target&, bool)> visit = [&](Target& t, bool top) {
            if (!seen.insert(t.name).second) return;
            for (auto& s : t.sources) {
                Lang l;
                if (!sourceLang(s, l)) continue;
                bool dup = false;
                for (auto& x : spec_.sources) if (x.path == s) { dup = true; break; }
                if (!dup) spec_.sources.push_back({s, l});
            }
            for (auto& i : (top ? t.inc : std::vector<fs::path>())) addUnique(spec_.includeDirs, i);
            for (auto& i : t.incIface) addUnique(spec_.includeDirs, i);
            for (auto& d : (top ? t.defs : std::vector<std::string>())) addUnique(spec_.defines, d);
            for (auto& d : t.defsIface) addUnique(spec_.defines, d);
            for (auto& o : (top ? t.opts : std::vector<std::string>())) addUnique(spec_.compileOptions, o);
            for (auto& o : t.optsIface) addUnique(spec_.compileOptions, o);
            for (auto& d : t.libDirs) addUnique(spec_.libDirs, d);
            for (auto& l : t.links) {
                if (Target* dep = findTarget(l)) { visit(*dep, false); continue; }
                if (l.find("::") != std::string::npos) { warn("link target '" + l + "' not resolvable (ignored)"); continue; }
                if (l.empty() || l[0] == '$') continue;
                std::string v = looksLikeLibPath(l) ? gen(absPath(l)) : l;
                addUnique(spec_.libs, v);
            }
        };
        visit(*exe, true);
        for (auto& i : gInc_) addUnique(spec_.includeDirs, i);
        for (auto& d : gDefs_) addUnique(spec_.defines, d);
        for (auto& o : gOpts_) addUnique(spec_.compileOptions, o);
        for (auto& d : gLibDirs_) addUnique(spec_.libDirs, d);
        for (auto& l : gLibs_) addUnique(spec_.libs, l);
        addUnique(spec_.includeDirs, rootDir_);

        for (const char* v : {"CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS"})
            for (auto& tok : [&] { std::vector<std::string> r; std::stringstream ss(getVar(v)); std::string w; while (ss >> w) r.push_back(w); return r; }())
                addUnique(spec_.compileOptions, tok);

        auto stdOf = [&](const std::string& tv, const std::string& var, bool cxx) -> std::string {
            std::string n = !tv.empty() ? tv : getVar(var);
            if (n.empty()) return "";
            if (cxx) return n == "26" ? "c++2c" : "c++" + n;
            return "c" + n;
        };
        spec_.cxxStd = stdOf(exe->cxxStd, "CMAKE_CXX_STANDARD", true);
        spec_.cStd = stdOf(exe->cStd, "CMAKE_C_STANDARD", false);
        if (spec_.sources.empty()) throw CMakeError("target '" + exe->name + "' has no C/C++ source files");
    }
};

}  // namespace

bool interpretCMake(const fs::path& file, const std::string& wantedTarget, ProjectSpec& spec, std::string& err) {
    try {
        Interp in(spec);
        in.run(file, wantedTarget);
        return true;
    } catch (const CMakeError& e) {
        err = std::string("CMake: ") + e.what();
    } catch (const std::exception& e) {
        err = std::string("CMake interpreter failure: ") + e.what();
    }
    return false;
}

}  // namespace cr
