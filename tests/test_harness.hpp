// A test harness in one header and thirty lines of runner.
//
// There is no Catch2 here on purpose. The suite needs registration, an
// assertion that prints both sides, and a non-zero exit code; fetching a
// framework at build time to get those would add a network dependency to the
// container build and about forty seconds of compile time per translation
// unit, in exchange for macros this project does not use.

#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace twtest {

using Fn = void (*)();

struct Case {
    const char* name;
    Fn fn;
};

std::vector<Case>& registry();
void record_failure(const char* file, int line, const std::string& message);
void record_check();

struct Reg {
    Reg(const char* name, Fn fn) { registry().push_back({name, fn}); }
};

template <typename T>
std::string show(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}
inline std::string show(bool v) { return v ? "true" : "false"; }

}  // namespace twtest

#define TW_TEST(name)                                     \
    static void name();                                   \
    static ::twtest::Reg tw_reg_##name(#name, name);      \
    static void name()

#define TW_CHECK(cond)                                                            \
    do {                                                                          \
        ::twtest::record_check();                                                 \
        if (!(cond)) ::twtest::record_failure(__FILE__, __LINE__, #cond);         \
    } while (0)

#define TW_CHECK_EQ(a, b)                                                                 \
    do {                                                                                  \
        ::twtest::record_check();                                                         \
        const auto tw_a = (a);                                                            \
        const auto tw_b = (b);                                                            \
        if (!(tw_a == tw_b))                                                              \
            ::twtest::record_failure(__FILE__, __LINE__,                                  \
                                     std::string(#a " == " #b "  (") +                    \
                                         ::twtest::show(tw_a) + " vs " +                  \
                                         ::twtest::show(tw_b) + ")");                     \
    } while (0)

#define TW_CHECK_NEAR(a, b, tol)                                                          \
    do {                                                                                  \
        ::twtest::record_check();                                                         \
        const double tw_a = (a);                                                          \
        const double tw_b = (b);                                                          \
        if (!((tw_a - tw_b) < (tol) && (tw_b - tw_a) < (tol)))                            \
            ::twtest::record_failure(__FILE__, __LINE__,                                  \
                                     std::string(#a " ~= " #b "  (") +                    \
                                         ::twtest::show(tw_a) + " vs " +                  \
                                         ::twtest::show(tw_b) + ")");                     \
    } while (0)
