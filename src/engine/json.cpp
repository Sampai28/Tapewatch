#include "tapewatch/json.hpp"

#include <charconv>
#include <cstdlib>

namespace tapewatch {
namespace {

struct Parser {
    std::string_view s;
    std::size_t i{0};
    std::string err;

    void skip_ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool eof() const { return i >= s.size(); }
    char peek() const { return s[i]; }

    bool fail(const char* m) {
        if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i);
        return false;
    }

    bool literal(std::string_view lit) {
        if (s.compare(i, lit.size(), lit) != 0) return fail("bad literal");
        i += lit.size();
        return true;
    }

    bool parse_string(std::string& out) {
        if (eof() || peek() != '"') return fail("expected string");
        ++i;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return fail("truncated escape");
            char e = s[i++];
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
                    if (i + 4 > s.size()) return fail("truncated \\u");
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s[i + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                        else return fail("bad \\u digit");
                    }
                    i += 4;
                    // Config and alert text is ASCII. Anything above the BMP
                    // basic range is passed through as UTF-8 for the common
                    // cases and not surrogate-paired, which this codebase
                    // never emits.
                    if (cp < 0x80) {
                        out += char(cp);
                    } else if (cp < 0x800) {
                        out += char(0xC0 | (cp >> 6));
                        out += char(0x80 | (cp & 0x3F));
                    } else {
                        out += char(0xE0 | (cp >> 12));
                        out += char(0x80 | ((cp >> 6) & 0x3F));
                        out += char(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_value(JsonValue& v) {
        skip_ws();
        if (eof()) return fail("unexpected end");
        switch (peek()) {
            case '{': return parse_object(v);
            case '[': return parse_array(v);
            case '"':
                v.type = JsonValue::Type::String;
                return parse_string(v.str);
            case 't':
                if (!literal("true")) return false;
                v.type = JsonValue::Type::Bool;
                v.b = true;
                return true;
            case 'f':
                if (!literal("false")) return false;
                v.type = JsonValue::Type::Bool;
                v.b = false;
                return true;
            case 'n':
                if (!literal("null")) return false;
                v.type = JsonValue::Type::Null;
                return true;
            default: return parse_number(v);
        }
    }

    bool parse_number(JsonValue& v) {
        const std::size_t start = i;
        if (!eof() && (peek() == '-' || peek() == '+')) ++i;
        bool any = false;
        while (!eof() && ((peek() >= '0' && peek() <= '9') || peek() == '.' || peek() == 'e' ||
                          peek() == 'E' || peek() == '-' || peek() == '+')) {
            any = true;
            ++i;
        }
        if (!any) return fail("expected number");
        const std::string text(s.substr(start, i - start));
        char* end = nullptr;
        const double d = std::strtod(text.c_str(), &end);
        if (end == text.c_str()) return fail("malformed number");
        v.type = JsonValue::Type::Number;
        v.num = d;
        return true;
    }

    bool parse_array(JsonValue& v) {
        v.type = JsonValue::Type::Array;
        ++i;  // '['
        skip_ws();
        if (!eof() && peek() == ']') {
            ++i;
            return true;
        }
        while (true) {
            JsonValue child;
            if (!parse_value(child)) return false;
            v.arr.push_back(std::move(child));
            skip_ws();
            if (eof()) return fail("unterminated array");
            if (peek() == ',') {
                ++i;
                continue;
            }
            if (peek() == ']') {
                ++i;
                return true;
            }
            return fail("expected , or ]");
        }
    }

    bool parse_object(JsonValue& v) {
        v.type = JsonValue::Type::Object;
        ++i;  // '{'
        skip_ws();
        if (!eof() && peek() == '}') {
            ++i;
            return true;
        }
        while (true) {
            skip_ws();
            std::string k;
            if (!parse_string(k)) return false;
            skip_ws();
            if (eof() || peek() != ':') return fail("expected :");
            ++i;
            JsonValue child;
            if (!parse_value(child)) return false;
            v.obj[k] = std::move(child);
            skip_ws();
            if (eof()) return fail("unterminated object");
            if (peek() == ',') {
                ++i;
                continue;
            }
            if (peek() == '}') {
                ++i;
                return true;
            }
            return fail("expected , or }");
        }
    }
};

}  // namespace

bool json_parse(std::string_view text, JsonValue& out, std::string& error) {
    Parser p;
    p.s = text;
    if (!p.parse_value(out)) {
        error = p.err;
        return false;
    }
    p.skip_ws();
    if (!p.eof()) {
        error = "trailing content at offset " + std::to_string(p.i);
        return false;
    }
    return true;
}

}  // namespace tapewatch
