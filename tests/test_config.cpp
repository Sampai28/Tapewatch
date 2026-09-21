#include "tapewatch/alert.hpp"
#include "tapewatch/config.hpp"
#include "test_harness.hpp"

using namespace tapewatch;

TW_TEST(config_empty_object_gives_defaults) {
    const ConfigLoad c = parse_config("{}");
    TW_CHECK(c.ok);
    TW_CHECK_EQ(c.config.tick_size, static_cast<Price>(1));
    TW_CHECK(c.config.spoofing.enabled);
    TW_CHECK_EQ(c.unknown_keys.size(), static_cast<std::size_t>(0));
}

TW_TEST(config_durations_are_milliseconds_on_the_wire) {
    const ConfigLoad c = parse_config(R"({"spoofing":{"max_lifetime_ms":250}})");
    TW_CHECK(c.ok);
    TW_CHECK_EQ(c.config.spoofing.max_lifetime_ns, static_cast<Ts>(250 * kNsPerMs));
}

TW_TEST(config_reports_keys_nothing_reads) {
    // A typo in a threshold is a run that measured something other than what
    // was asked for. Silence here is the expensive failure mode.
    const ConfigLoad c = parse_config(R"({
        "tick_sise": 5,
        "spoofing": {"max_lifetime_mss": 1, "weights": {"sizee": 0.5}}
    })");
    TW_CHECK(c.ok);
    TW_CHECK_EQ(c.unknown_keys.size(), static_cast<std::size_t>(3));
    bool saw_root = false, saw_nested = false, saw_weight = false;
    for (const auto& k : c.unknown_keys) {
        if (k == "tick_sise") saw_root = true;
        if (k == "spoofing.max_lifetime_mss") saw_nested = true;
        if (k == "spoofing.weights.sizee") saw_weight = true;
    }
    TW_CHECK(saw_root);
    TW_CHECK(saw_nested);
    TW_CHECK(saw_weight);
    // ...and the real default survives the typo rather than being clobbered.
    TW_CHECK_EQ(c.config.spoofing.max_lifetime_ns, static_cast<Ts>(2000 * kNsPerMs));
}

TW_TEST(config_rejects_a_nonsense_tick_size) {
    const ConfigLoad c = parse_config(R"({"tick_size":0})");
    TW_CHECK(!c.ok);
    TW_CHECK(!c.error.empty());
}

TW_TEST(config_rejects_malformed_json) {
    const ConfigLoad c = parse_config("{nope}");
    TW_CHECK(!c.ok);
}

TW_TEST(config_round_trips_through_its_own_serialiser) {
    ConfigLoad a = parse_config(R"({
        "tick_size": 5,
        "scan_interval_ms": 123,
        "wash_trading": {"window_ms": 7000, "weights": {"self": 0.9}},
        "momentum_ignition": {"enabled": false}
    })");
    TW_CHECK(a.ok);
    const ConfigLoad b = parse_config(config_to_json(a.config));
    TW_CHECK(b.ok);
    TW_CHECK_EQ(b.config.tick_size, static_cast<Price>(5));
    TW_CHECK_EQ(b.config.scan_interval_ns, static_cast<Ts>(123 * kNsPerMs));
    TW_CHECK_EQ(b.config.wash.window_ns, static_cast<Ts>(7000 * kNsPerMs));
    TW_CHECK_NEAR(b.config.wash.w_self, 0.9, 1e-9);
    TW_CHECK(!b.config.momentum.enabled);
    TW_CHECK_EQ(b.unknown_keys.size(), static_cast<std::size_t>(0));
}

TW_TEST(config_weights_need_not_sum_to_one) {
    // weighted_score normalises, so editing one weight changes the ranking
    // without silently moving every operating point.
    Alert a;
    a.signal("x", 1.0, 2.0);
    a.signal("y", 0.0, 2.0);
    TW_CHECK_NEAR(a.weighted_score(), 0.5, 1e-9);

    Alert b;
    b.signal("x", 1.0, 0.2);
    b.signal("y", 0.0, 0.2);
    TW_CHECK_NEAR(b.weighted_score(), 0.5, 1e-9);
}

TW_TEST(config_signal_values_are_clamped) {
    Alert a;
    a.signal("over", 4.0, 1.0);
    a.signal("under", -2.0, 1.0);
    TW_CHECK_NEAR(a.signals[0].value, 1.0, 1e-9);
    TW_CHECK_NEAR(a.signals[1].value, 0.0, 1e-9);
}
