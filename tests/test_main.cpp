#include <cstdio>
#include <cstring>
#include <string>

#include "test_harness.hpp"

namespace twtest {
namespace {
int g_failures_in_case = 0;
int g_total_failures = 0;
int g_total_checks = 0;
}  // namespace

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

void record_check() { ++g_total_checks; }

void record_failure(const char* file, int line, const std::string& message) {
    ++g_failures_in_case;
    ++g_total_failures;
    std::printf("    %s:%d: %s\n", file, line, message.c_str());
}

}  // namespace twtest

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failed = 0;

    for (const auto& c : twtest::registry()) {
        if (filter && std::strstr(c.name, filter) == nullptr) continue;
        ++run;
        twtest::g_failures_in_case = 0;
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        if (twtest::g_failures_in_case == 0) {
            std::printf("[  OK  ] %s\n", c.name);
        } else {
            std::printf("[ FAIL ] %s (%d)\n", c.name, twtest::g_failures_in_case);
            ++failed;
        }
    }

    std::printf("\n%d cases, %d checks, %d failed cases, %d failed checks\n", run,
                twtest::g_total_checks, failed, twtest::g_total_failures);
    return failed == 0 ? 0 : 1;
}
