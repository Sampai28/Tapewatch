#include "tapewatch/ring.hpp"
#include "test_harness.hpp"

using namespace tapewatch;

TW_TEST(window_evicts_by_time) {
    TimeWindow<int> w(64, 1000);
    for (int i = 0; i < 5; ++i) w.push(i * 100, i);
    TW_CHECK_EQ(w.size(), static_cast<std::size_t>(5));

    w.push(1500, 99);
    // cutoff is 1500 - 1000 = 500, so ts 0..400 leave and 500.. would stay
    TW_CHECK_EQ(w.size(), static_cast<std::size_t>(1));
    TW_CHECK_EQ(w.front(), 99);
}

TW_TEST(window_expire_without_push) {
    TimeWindow<int> w(64, 1000);
    w.push(0, 1);
    w.push(100, 2);
    w.expire(2000);
    TW_CHECK(w.empty());
}

TW_TEST(window_capacity_is_a_hard_wall) {
    TimeWindow<int> w(4, 1'000'000);
    for (int i = 0; i < 10; ++i) w.push(i, i);
    TW_CHECK_EQ(w.size(), static_cast<std::size_t>(4));
    TW_CHECK_EQ(w.overflowed(), static_cast<std::uint64_t>(6));
    // The survivors are the newest four, not the oldest.
    TW_CHECK_EQ(w[0], 6);
    TW_CHECK_EQ(w[3], 9);
}

TW_TEST(window_indexes_oldest_first) {
    TimeWindow<int> w(8, 1'000'000);
    w.push(10, 1);
    w.push(20, 2);
    w.push(30, 3);
    TW_CHECK_EQ(w[0], 1);
    TW_CHECK_EQ(w[2], 3);
    TW_CHECK_EQ(w.ts_at(1), static_cast<Ts>(20));
    TW_CHECK_EQ(w.front_ts(), static_cast<Ts>(10));
    TW_CHECK_EQ(w.back_ts(), static_cast<Ts>(30));
}

TW_TEST(window_lower_bound_finds_slice_start) {
    TimeWindow<int> w(16, 1'000'000);
    for (int i = 0; i < 8; ++i) w.push(i * 10, i);
    TW_CHECK_EQ(w.lower_bound_ts(0), static_cast<std::size_t>(0));
    TW_CHECK_EQ(w.lower_bound_ts(35), static_cast<std::size_t>(4));
    TW_CHECK_EQ(w.lower_bound_ts(40), static_cast<std::size_t>(4));
    TW_CHECK_EQ(w.lower_bound_ts(1000), w.size());
}

TW_TEST(window_lower_bound_survives_wraparound) {
    // The ring has wrapped, so head_ is not 0 and a naive binary search over
    // raw slots would compare the wrong elements.
    TimeWindow<int> w(4, 1'000'000);
    for (int i = 0; i < 6; ++i) w.push(i * 10, i);
    TW_CHECK_EQ(w.size(), static_cast<std::size_t>(4));
    TW_CHECK_EQ(w.ts_at(0), static_cast<Ts>(20));
    TW_CHECK_EQ(w.lower_bound_ts(40), static_cast<std::size_t>(2));
    TW_CHECK_EQ(w[2], 4);
}
