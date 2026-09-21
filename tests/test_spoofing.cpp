// Spoofing detector scenarios.
//
// Each test is a small hand-built tape. The positive case and the market-maker
// case are deliberately the same *shape* -- post size, pull it, trade the
// other way -- because if the detector could only tell them apart by something
// other than shape, it would be useless on a real venue.

#include "tape_builder.hpp"
#include "test_harness.hpp"

using namespace tapewatch;
using namespace twtest;

namespace {

// A thin resting book so that a 200-lot order is visibly outsized.
void background(Tape& t) {
    t.at(1000);
    t.order(1, 101, Side::Buy, 99, 20);
    t.order(1, 102, Side::Buy, 98, 20);
    t.order(2, 201, Side::Sell, 103, 20);
    t.order(2, 202, Side::Sell, 104, 20);
}

}  // namespace

TW_TEST(spoofing_fires_on_size_pulled_before_a_trade_the_other_way) {
    Tape t;
    background(t);

    // A 200-lot bid at the front of a book whose orders average 20.
    t.at(1100).order(50, 500, Side::Buy, 100, 200);
    t.at(1400).cancel(50, 500);
    // ...and immediately sells into the bid the display helped hold up.
    t.at(1500).trade(50, 501, 1, 101, Side::Sell, 99, 20);
    t.at(1520).trade(50, 502, 1, 102, Side::Sell, 98, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);

    TW_CHECK(w.count(DetectorKind::Spoofing, 50) >= 1);
    const Alert* a = w.first(DetectorKind::Spoofing);
    TW_CHECK(a != nullptr);
    if (!a) return;
    TW_CHECK_EQ(a->participant, 50u);
    TW_CHECK_EQ(a->order_ids.size(), static_cast<std::size_t>(1));
    TW_CHECK_EQ(a->order_ids[0], static_cast<OrderId>(500));
    TW_CHECK(a->score >= 0.5);
    // The evidence an analyst needs to dismiss or escalate in under a minute.
    bool saw_opposite = false, saw_life = false;
    for (const auto& [k, v] : a->evidence) {
        if (k == "opposite_qty") {
            saw_opposite = true;
            TW_CHECK_EQ(v, static_cast<std::int64_t>(40));
        }
        if (k == "lifetime_ms") {
            saw_life = true;
            TW_CHECK_EQ(v, static_cast<std::int64_t>(300));
        }
    }
    TW_CHECK(saw_opposite);
    TW_CHECK(saw_life);
}

TW_TEST(spoofing_needs_the_trade_on_the_other_side) {
    // Identical size and identical pull, but the participant never trades.
    // Cancelling a big order quickly is not manipulation; it is Tuesday.
    Tape t;
    background(t);
    t.at(1100).order(50, 500, Side::Buy, 100, 200);
    t.at(1400).cancel(50, 500);
    t.at(2000).order(50, 503, Side::Buy, 100, 200);
    t.at(2300).cancel(50, 503);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::Spoofing), static_cast<std::size_t>(0));
}

TW_TEST(spoofing_ignores_a_trade_on_the_same_side) {
    // Posting a bid, pulling it, and then buying is not spoofing -- the
    // direction is the whole pattern. Getting participant_side backwards
    // would light this up.
    Tape t;
    background(t);
    t.at(1100).order(50, 500, Side::Buy, 100, 200);
    t.at(1400).cancel(50, 500);
    t.at(1500).trade(50, 501, 2, 201, Side::Buy, 103, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::Spoofing), static_cast<std::size_t>(0));
}

TW_TEST(spoofing_leaves_an_ordinary_two_sided_quoter_alone) {
    // The market maker false positive, in its cleanest form: quotes both
    // sides at ordinary size, refreshes the bid, and is lifted on the offer
    // moments later. Same three ingredients as the spoof, none of the intent.
    Tape t;
    background(t);
    t.at(1100).order(60, 600, Side::Buy, 100, 20);
    t.at(1100).order(60, 601, Side::Sell, 102, 20);
    t.at(2600).cancel(60, 600);
    t.at(2700).trade(70, 700, 60, 601, Side::Buy, 102, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::Spoofing, 60), static_cast<std::size_t>(0));
}

TW_TEST(spoofing_penalises_two_sided_quoting_at_identical_shape) {
    // Two participants run the exact same pattern. The only difference is
    // that one of them was also quoting the other side throughout. The
    // penalty must show up as a lower score, not as a different gate.
    auto score_for = [](bool also_quote_other_side) {
        Tape t;
        background(t);
        t.at(1100).order(50, 500, Side::Buy, 100, 200);
        if (also_quote_other_side) {
            t.at(1100).order(50, 510, Side::Sell, 103, 200);
            t.at(1150).order(50, 511, Side::Sell, 104, 200);
            t.at(1160).order(50, 512, Side::Sell, 105, 200);
        }
        t.at(1400).cancel(50, 500);
        t.at(1500).trade(50, 501, 1, 101, Side::Sell, 99, 20);
        t.at(1520).trade(50, 502, 1, 102, Side::Sell, 98, 20);

        CollectingWriter w;
        Pipeline p(test_config(), w);
        t.run(p);
        const Alert* a = w.first(DetectorKind::Spoofing);
        return a ? a->score : -1.0;
    };

    const double one_sided = score_for(false);
    const double two_sided = score_for(true);
    TW_CHECK(one_sided > 0.0);
    TW_CHECK(two_sided < one_sided);
}

TW_TEST(spoofing_ignores_an_order_parked_far_from_the_touch) {
    // Fifty ticks away nobody was looking at it, so pulling it moved nothing.
    Tape t;
    background(t);
    t.at(1100).order(50, 500, Side::Buy, 50, 200);
    t.at(1400).cancel(50, 500);
    t.at(1500).trade(50, 501, 1, 101, Side::Sell, 99, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::Spoofing), static_cast<std::size_t>(0));
}

TW_TEST(spoofing_ignores_a_long_lived_order) {
    Tape t;
    background(t);
    t.at(1100).order(50, 500, Side::Buy, 100, 200);
    t.at(30000).cancel(50, 500);  // resting for 29 seconds
    t.at(30100).trade(50, 501, 1, 101, Side::Sell, 99, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::Spoofing), static_cast<std::size_t>(0));
}

TW_TEST(spoofing_ignores_a_small_order) {
    Tape t;
    background(t);
    t.at(1100).order(50, 500, Side::Buy, 100, 5);  // below min_qty
    t.at(1400).cancel(50, 500);
    t.at(1500).trade(50, 501, 1, 101, Side::Sell, 99, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::Spoofing), static_cast<std::size_t>(0));
}

TW_TEST(spoofing_sees_through_an_amend_to_zero) {
    // Amending a layer away instead of cancelling it must not be an escape
    // hatch from the cancel-based detectors.
    Tape t;
    background(t);
    t.at(1100).order(50, 500, Side::Buy, 100, 200);
    t.at(1400).modify(50, 500, Side::Buy, 100, 0);
    t.at(1500).trade(50, 501, 1, 101, Side::Sell, 99, 20);
    t.at(1520).trade(50, 502, 1, 102, Side::Sell, 98, 20);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK(w.count(DetectorKind::Spoofing, 50) >= 1);
}
