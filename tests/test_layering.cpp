#include "tape_builder.hpp"
#include "test_harness.hpp"

using namespace tapewatch;
using namespace twtest;

namespace {

void background(Tape& t) {
    t.at(1000);
    t.order(1, 101, Side::Buy, 96, 20);
    t.order(1, 102, Side::Buy, 95, 20);
    t.order(2, 201, Side::Sell, 103, 20);
    t.order(2, 202, Side::Sell, 104, 20);
}

}  // namespace

TW_TEST(layering_fires_on_a_coordinated_stack_pulled_together) {
    Tape t;
    background(t);

    // Four bids across four levels, in and out as one decision.
    t.at(1100).order(50, 500, Side::Buy, 100, 50);
    t.at(1120).order(50, 501, Side::Buy, 99, 50);
    t.at(1140).order(50, 502, Side::Buy, 98, 50);
    t.at(1160).order(50, 503, Side::Buy, 97, 50);

    t.at(2000).cancel(50, 500);
    t.at(2020).cancel(50, 501);
    t.at(2040).cancel(50, 502);
    t.at(2060).cancel(50, 503);

    t.at(2100).trade(50, 510, 1, 101, Side::Sell, 96, 20);
    t.at(2120).trade(50, 511, 1, 102, Side::Sell, 95, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);

    TW_CHECK(w.count(DetectorKind::Layering, 50) >= 1);
    const Alert* a = w.first(DetectorKind::Layering);
    TW_CHECK(a != nullptr);
    if (!a) return;
    TW_CHECK_EQ(a->order_ids.size(), static_cast<std::size_t>(4));
    for (const auto& [k, v] : a->evidence) {
        if (k == "distinct_levels") TW_CHECK_EQ(v, static_cast<std::int64_t>(4));
        if (k == "layer_qty") TW_CHECK_EQ(v, static_cast<std::int64_t>(200));
        if (k == "cancel_span_ms") TW_CHECK_EQ(v, static_cast<std::int64_t>(60));
    }
}

TW_TEST(layering_needs_more_than_two_levels) {
    Tape t;
    background(t);
    t.at(1100).order(50, 500, Side::Buy, 100, 50);
    t.at(1120).order(50, 501, Side::Buy, 99, 50);
    t.at(2000).cancel(50, 500);
    t.at(2020).cancel(50, 501);
    t.at(2100).trade(50, 510, 1, 101, Side::Sell, 96, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::Layering), static_cast<std::size_t>(0));
}

TW_TEST(layering_needs_the_cancels_to_cluster) {
    // Same four orders at the same four levels, withdrawn over half a minute
    // as independent decisions. That is inventory management.
    Tape t;
    background(t);
    t.at(1100).order(50, 500, Side::Buy, 100, 50);
    t.at(1120).order(50, 501, Side::Buy, 99, 50);
    t.at(1140).order(50, 502, Side::Buy, 98, 50);
    t.at(1160).order(50, 503, Side::Buy, 97, 50);

    t.at(2000).cancel(50, 500);
    t.at(9000).cancel(50, 501);
    t.at(16000).cancel(50, 502);
    t.at(23000).cancel(50, 503);
    t.at(23100).trade(50, 510, 1, 101, Side::Sell, 96, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::Layering), static_cast<std::size_t>(0));
}

TW_TEST(layering_needs_the_trade_on_the_other_side) {
    Tape t;
    background(t);
    t.at(1100).order(50, 500, Side::Buy, 100, 50);
    t.at(1120).order(50, 501, Side::Buy, 99, 50);
    t.at(1140).order(50, 502, Side::Buy, 98, 50);
    t.at(1160).order(50, 503, Side::Buy, 97, 50);
    t.at(2000).cancel(50, 500);
    t.at(2020).cancel(50, 501);
    t.at(2040).cancel(50, 502);
    t.at(2060).cancel(50, 503);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::Layering), static_cast<std::size_t>(0));
}

TW_TEST(layering_does_not_mix_sides_into_one_cluster) {
    // Two bids and two offers withdrawn together is a quote refresh, not a
    // four-level wall. Clustering has to be per side.
    Tape t;
    background(t);
    t.at(1100).order(50, 500, Side::Buy, 100, 50);
    t.at(1120).order(50, 501, Side::Buy, 99, 50);
    t.at(1140).order(50, 502, Side::Sell, 102, 50);
    t.at(1160).order(50, 503, Side::Sell, 101, 50);
    t.at(2000).cancel(50, 500);
    t.at(2020).cancel(50, 501);
    t.at(2040).cancel(50, 502);
    t.at(2060).cancel(50, 503);
    t.at(2100).trade(50, 510, 1, 101, Side::Sell, 96, 20);
    t.at(2120).trade(50, 511, 2, 201, Side::Buy, 103, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::Layering), static_cast<std::size_t>(0));
}

TW_TEST(layering_catches_a_stack_taken_away_by_amendment) {
    Tape t;
    background(t);
    t.at(1100).order(50, 500, Side::Buy, 100, 50);
    t.at(1120).order(50, 501, Side::Buy, 99, 50);
    t.at(1140).order(50, 502, Side::Buy, 98, 50);
    t.at(1160).order(50, 503, Side::Buy, 97, 50);

    t.at(2000).modify(50, 500, Side::Buy, 100, 0);
    t.at(2020).modify(50, 501, Side::Buy, 99, 0);
    t.at(2040).modify(50, 502, Side::Buy, 98, 0);
    t.at(2060).modify(50, 503, Side::Buy, 97, 0);

    t.at(2100).trade(50, 510, 1, 101, Side::Sell, 96, 20);
    t.at(2120).trade(50, 511, 1, 102, Side::Sell, 95, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK(w.count(DetectorKind::Layering, 50) >= 1);
}
