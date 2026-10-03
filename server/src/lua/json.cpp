#include "lua/json.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dnp::server::lua {

namespace {

constexpr int MAX_DEPTH = 64;

struct Parser {
    lua_State* L;
    const char* p;
    const char* end;
    std::string& err;
    int depth = 0;

    const char* begin = p;
    bool fail(const char* what) {
        if (err.empty()) err = std::string(what) + " at byte " + std::to_string(p - begin);
        return false;
    }
    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (end - p < 4) return fail("a \\u escape is cut short");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("a \\u escape is not hexadecimal");
        }
        return true;
    }
    bool string(std::string& out) {
        ++p;   // the opening quote
        while (p < end) {
            const unsigned char c = static_cast<unsigned char>(*p++);
            if (c == '"') return true;
            if (c < 0x20) return fail("a control character inside a string");
            if (c != '\\') { out += static_cast<char>(c); continue; }
            if (p >= end) break;
            const char e = *p++;
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        unsigned lo = 0;
                        if (end - p < 2 || p[0] != '\\' || p[1] != 'u') return fail("a lone high surrogate");
                        p += 2;
                        if (!hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("a high surrogate without its low half");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("a lone low surrogate");
                    }
                    utf8(out, cp);
                    break;
                }
                default: return fail("an unknown escape in a string");
            }
        }
        return fail("a string is not closed");
    }
    bool number() {
        const char* s = p;
        if (p < end && *p == '-') ++p;
        if (p >= end) return fail("a number is cut short");
        if (*p == '0') ++p;
        else if (*p >= '1' && *p <= '9') { while (p < end && *p >= '0' && *p <= '9') ++p; }
        else return fail("a number has no digits");
        if (p < end && *p == '.') {
            ++p;
            if (p >= end || *p < '0' || *p > '9') return fail("a number has nothing after its point");
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            if (p >= end || *p < '0' || *p > '9') return fail("a number's exponent has no digits");
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        const std::string text(s, static_cast<size_t>(p - s));
        lua_pushnumber(L, std::strtod(text.c_str(), nullptr));
        return true;
    }
    bool word(const char* w, size_t n) {
        if (static_cast<size_t>(end - p) < n || std::memcmp(p, w, n) != 0) return fail("an unknown word");
        p += n;
        return true;
    }
    bool value() {
        ws();
        if (p >= end) return fail("a value is missing");
        if (++depth > MAX_DEPTH) return fail("nested too deeply");
        lua_checkstack(L, 4);
        bool ok = false;
        const char c = *p;
        if (c == '{') {
            ++p;
            lua_newtable(L);
            ws();
            if (p < end && *p == '}') { ++p; ok = true; }
            else {
                for (;;) {
                    ws();
                    if (p >= end || *p != '"') { ok = fail("an object key is not a string"); break; }
                    std::string key;
                    if (!string(key)) { ok = false; break; }
                    ws();
                    if (p >= end || *p != ':') { ok = fail("an object key has no ':'"); break; }
                    ++p;
                    lua_pushlstring(L, key.data(), key.size());
                    if (!value()) { lua_pop(L, 1); ok = false; break; }
                    lua_rawset(L, -3);
                    ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == '}') { ++p; ok = true; break; }
                    ok = fail("an object is not closed");
                    break;
                }
            }
            if (!ok) lua_pop(L, 1);
        } else if (c == '[') {
            ++p;
            lua_newtable(L);
            ws();
            int i = 0;
            if (p < end && *p == ']') { ++p; ok = true; }
            else {
                for (;;) {
                    if (!value()) { ok = false; break; }
                    lua_rawseti(L, -2, ++i);
                    ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == ']') { ++p; ok = true; break; }
                    ok = fail("an array is not closed");
                    break;
                }
            }
            if (!ok) lua_pop(L, 1);
        } else if (c == '"') {
            std::string s;
            ok = string(s);
            if (ok) lua_pushlstring(L, s.data(), s.size());
        } else if (c == 't') {
            ok = word("true", 4);
            if (ok) lua_pushboolean(L, 1);
        } else if (c == 'f') {
            ok = word("false", 5);
            if (ok) lua_pushboolean(L, 0);
        } else if (c == 'n') {
            ok = fail("null, which a Lua table cannot hold in an array; leave the field out");
        } else {
            ok = number();
        }
        --depth;
        return ok;
    }
};

}  // namespace

bool push_json(lua_State* L, const char* text, size_t len, std::string& err) {
    err.clear();
    Parser ps{L, text, text + len, err};
    const int top = lua_gettop(L);
    if (!ps.value()) { lua_settop(L, top); if (err.empty()) err = "not JSON"; return false; }
    ps.ws();
    if (ps.p != ps.end) { lua_settop(L, top); err = "something after the value"; return false; }
    return true;
}

bool push_json_file(lua_State* L, const std::string& path, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) { err = "cannot open " + path; return false; }
    std::string text;
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
        if (text.size() > (4u << 20)) { std::fclose(f); err = path + " is over 4 MB"; return false; }
    }
    std::fclose(f);
    return push_json(L, text.data(), text.size(), err);
}

}  // namespace dnp::server::lua
