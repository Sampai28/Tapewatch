#include "tapewatch/json.hpp"
#include "test_harness.hpp"

using namespace tapewatch;

TW_TEST(json_writer_emits_string_literals_as_strings) {
    // Without an explicit const char* overload this produces {"kind":true},
    // because const char* -> bool is a standard conversion and beats the
    // user-defined conversion to string_view. It is silent, and it corrupts
    // every field written with a literal.
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field("kind", "spoofing");
    w.field("flag", true);
    w.end_object();
    TW_CHECK_EQ(out, std::string("{\"kind\":\"spoofing\",\"flag\":true}"));
}

TW_TEST(json_writer_nests_and_separates) {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field("a", static_cast<std::int64_t>(1));
    w.key("b");
    w.begin_array();
    w.value(static_cast<std::int64_t>(2));
    w.value(static_cast<std::int64_t>(3));
    w.end_array();
    w.key("c");
    w.begin_object();
    w.field("d", 0.5);
    w.end_object();
    w.end_object();
    TW_CHECK_EQ(out, std::string("{\"a\":1,\"b\":[2,3],\"c\":{\"d\":0.5}}"));
}

TW_TEST(json_writer_escapes) {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field("s", "a\"b\\c\nd");
    w.end_object();
    TW_CHECK_EQ(out, std::string("{\"s\":\"a\\\"b\\\\c\\nd\"}"));
}

TW_TEST(json_reader_parses_nested_config) {
    JsonValue v;
    std::string err;
    const bool ok = json_parse(
        R"({"tick_size":5,"on":true,"off":false,"nil":null,
            "spoofing":{"weights":{"size":0.25},"name":"sp"},
            "list":[1,2,3]})",
        v, err);
    TW_CHECK(ok);
    TW_CHECK_EQ(err, std::string(""));
    TW_CHECK_EQ(v.int_or("tick_size", 0), static_cast<std::int64_t>(5));
    TW_CHECK(v.bool_or("on", false));
    TW_CHECK(!v.bool_or("off", true));
    const JsonValue* sp = v.get("spoofing");
    TW_CHECK(sp != nullptr);
    TW_CHECK_EQ(sp->str_or("name", ""), std::string("sp"));
    TW_CHECK_NEAR(sp->get("weights")->num_or("size", 0.0), 0.25, 1e-9);
    TW_CHECK_EQ(v.get("list")->arr.size(), static_cast<std::size_t>(3));
}

TW_TEST(json_reader_rejects_trailing_content) {
    JsonValue v;
    std::string err;
    TW_CHECK(!json_parse("{\"a\":1} junk", v, err));
    TW_CHECK(!err.empty());
}

TW_TEST(json_reader_rejects_truncated_object) {
    JsonValue v;
    std::string err;
    TW_CHECK(!json_parse("{\"a\":", v, err));
}

TW_TEST(json_reader_missing_key_returns_default) {
    JsonValue v;
    std::string err;
    TW_CHECK(json_parse("{}", v, err));
    TW_CHECK_NEAR(v.num_or("absent", 4.5), 4.5, 1e-9);
    TW_CHECK(v.get("absent") == nullptr);
}
