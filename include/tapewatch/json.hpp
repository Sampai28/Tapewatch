// A small JSON writer and reader.
//
// Tapewatch emits JSON Lines for alerts and reads a JSON detector config. That
// is the entire requirement, and it does not justify a dependency: the writer
// is a string builder with brace tracking, the reader is a recursive descent
// parser over a std::string. Both are ~200 lines and are covered by
// tests/test_json.cpp.
//
// The writer has explicit `const char*` overloads. Without them a string
// literal binds to the `bool` overload -- a standard conversion beats a
// user-defined one -- and every literal field silently serialises as `true`.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace tapewatch {

class JsonWriter {
public:
    explicit JsonWriter(std::string& out) : out_(out) {}

    void begin_object() {
        sep();
        out_ += '{';
        stack_.push_back(false);
    }
    void end_object() {
        out_ += '}';
        stack_.pop_back();
    }
    void begin_array() {
        sep();
        out_ += '[';
        stack_.push_back(false);
    }
    void end_array() {
        out_ += ']';
        stack_.pop_back();
    }

    void key(std::string_view k) {
        sep();
        escape(k);
        out_ += ':';
        pending_key_ = true;
    }

    void value(std::string_view v) {
        sep();
        escape(v);
    }
    void value(const char* v) { value(std::string_view(v)); }
    void value(bool v) {
        sep();
        out_ += v ? "true" : "false";
    }
    void value(std::int64_t v) {
        sep();
        char b[24];
        out_.append(b, std::snprintf(b, sizeof b, "%lld", (long long)v));
    }
    void value(std::uint64_t v) {
        sep();
        char b[24];
        out_.append(b, std::snprintf(b, sizeof b, "%llu", (unsigned long long)v));
    }
    void value(int v) { value(static_cast<std::int64_t>(v)); }
    void value(unsigned v) { value(static_cast<std::uint64_t>(v)); }
    // Six places: enough that a score round trips through the Python harness
    // unchanged, few enough that the files stay diffable.
    void value(double v) {
        sep();
        if (!std::isfinite(v)) {
            out_ += "null";
            return;
        }
        char b[40];
        out_.append(b, std::snprintf(b, sizeof b, "%.6g", v));
    }
    void null() {
        sep();
        out_ += "null";
    }

    template <typename T>
    void field(std::string_view k, T v) {
        key(k);
        value(v);
    }
    void field(std::string_view k, const char* v) {
        key(k);
        value(std::string_view(v));
    }
    void field(std::string_view k, const std::string& v) {
        key(k);
        value(std::string_view(v));
    }

private:
    void sep() {
        if (pending_key_) {
            pending_key_ = false;
            return;
        }
        if (!stack_.empty()) {
            if (stack_.back()) out_ += ',';
            stack_.back() = true;
        }
    }

    void escape(std::string_view s) {
        out_ += '"';
        for (char c : s) {
            switch (c) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof b, "\\u%04x", c);
                        out_ += b;
                    } else {
                        out_ += c;
                    }
            }
        }
        out_ += '"';
    }

    std::string& out_;
    std::vector<bool> stack_;
    bool pending_key_{false};
};

// --------------------------------------------------------------------------
// Reader
// --------------------------------------------------------------------------

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type{Type::Null};
    bool b{false};
    double num{0.0};
    std::string str;
    std::vector<JsonValue> arr;
    std::map<std::string, JsonValue> obj;

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_number() const { return type == Type::Number; }

    // Lookups return a default rather than throwing. A detector config with a
    // missing key should fall back to the built-in default and say so, not
    // take the process down mid-run.
    const JsonValue* get(const std::string& k) const {
        if (type != Type::Object) return nullptr;
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
    double num_or(const std::string& k, double d) const {
        const JsonValue* v = get(k);
        return (v && v->type == Type::Number) ? v->num : d;
    }
    std::int64_t int_or(const std::string& k, std::int64_t d) const {
        const JsonValue* v = get(k);
        return (v && v->type == Type::Number) ? static_cast<std::int64_t>(v->num) : d;
    }
    bool bool_or(const std::string& k, bool d) const {
        const JsonValue* v = get(k);
        return (v && v->type == Type::Bool) ? v->b : d;
    }
    std::string str_or(const std::string& k, const std::string& d) const {
        const JsonValue* v = get(k);
        return (v && v->type == Type::String) ? v->str : d;
    }
};

// Returns false and fills `error` on malformed input.
bool json_parse(std::string_view text, JsonValue& out, std::string& error);

}  // namespace tapewatch
