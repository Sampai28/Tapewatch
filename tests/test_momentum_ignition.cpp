#include "tape_builder.hpp"
#include "test_harness.hpp"

using namespace tapewatch;
using namespace twtest;

namespace {

// This detector evaluates a burst window that sits `revert_ms` in the past,
// and it needs a midpoint from *before* the window opens to measure the move
// against. So the book is established at 25 seconds and the burst happens at
// 30, leaving five seconds of quiet tape for the pre-burst midpoint to come
// from. A tape that starts at the burst has no baseline and the detector
// correctly refuses to guess one.
void ladder(Tape& t) {
    t.at(25000);
    t.order(1, 100, Side::Buy, 100, 200);
    for (int i = 0; i < 10; ++i) t.order(2, 200 + i, Side::Sell, 101 + i, 10);
}

// Participant 50 lifts eight offers in four hundred milliseconds; the resting
// bid is refreshed higher behind it, so the midpoint moves eight ticks.
void burst(Tape& t) {
    t.at(30100);
    for (int i = 0; i < 8; ++i) t.advance(50).trade(50, 300 + i, 2, 200 + i, Side::Buy, 101 + i, 10);
    t.at(30520).cancel(1, 100);
    t.at(30530).order(1, 150, Side::Buy, 108, 200);
}

// The price goes back to where it started.
void revert(Tape& t) {
    t.at(34000).order(1, 160, Side::Buy, 100, 200);
    t.at(34010).order(2, 300, Side::Sell, 101, 50);
}

}  // namespace

TW_TEST(momentum_fires_when_the_igniter_reverses_and_the_price_comes_back) {
    Tape t;
    ladder(t);
    burst(t);

    // Sells back into the bid it created...
    t.at(33000).trade(50, 400, 1, 150, Side::Sell, 108, 100);
    t.at(33100).trade(50, 401, 1, 150, Side::Sell, 108, 100);
    // ...and the market goes back to where it was.
    revert(t);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);

    TW_CHECK(w.count(DetectorKind::MomentumIgnition, 50) >= 1);
    const Alert* a = w.first(DetectorKind::MomentumIgnition);
    TW_CHECK(a != nullptr);
    if (!a) return;
    TW_CHECK_EQ(a->participant, 50u);
    for (const auto& [k, v] : a->evidence) {
        if (k == "burst_trades") TW_CHECK(v >= 4);
        if (k == "move_ticks") TW_CHECK(v >= 4);
        if (k == "reverse_qty") TW_CHECK(v > 0);
        if (k == "burst_dir_is_buy") TW_CHECK_EQ(v, static_cast<std::int64_t>(1));
    }
    // Detection cannot happen before the reversal window has closed, and the
    // alert must say so rather than backdating itself.
    TW_CHECK(a->detect_ts > a->start_ts);
}

TW_TEST(momentum_leaves_an_informed_trader_alone) {
    // Same burst, same price move, same share of the aggressive flow. The
    // participant simply keeps the position, and the price stays where they
    // pushed it. There is nothing here to distinguish from being right.
    Tape t;
    ladder(t);
    burst(t);
    // The book stays where the burst left it and 50 keeps the position.
    t.at(34000).order(2, 300, Side::Sell, 109, 50);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::MomentumIgnition), static_cast<std::size_t>(0));
}

TW_TEST(momentum_needs_the_price_to_actually_move) {
    Tape t;
    t.at(25000);
    t.order(1, 100, Side::Buy, 100, 500);
    t.order(2, 200, Side::Sell, 101, 500);
    // Plenty of aggressive prints, all at the same price, no move.
    t.at(30100);
    for (int i = 0; i < 8; ++i) t.advance(50).trade(50, 300 + i, 2, 200, Side::Buy, 101, 10);
    t.at(33000).trade(50, 400, 1, 100, Side::Sell, 100, 80);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::MomentumIgnition), static_cast<std::size_t>(0));
}

TW_TEST(momentum_needs_enough_prints_to_be_a_burst) {
    Tape t;
    ladder(t);
    // Two prints move the price the same distance, but two prints are not an
    // ignition campaign -- that is somebody filling an order.
    t.at(30100).trade(50, 300, 2, 200, Side::Buy, 101, 10);
    t.at(30150).trade(50, 301, 2, 201, Side::Buy, 102, 10);
    t.at(30520).cancel(1, 100);
    t.at(30530).order(1, 150, Side::Buy, 108, 200);
    t.at(33000).trade(50, 400, 1, 150, Side::Sell, 108, 20);
    revert(t);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::MomentumIgnition), static_cast<std::size_t>(0));
}

TW_TEST(momentum_does_not_blame_the_resting_side) {
    // Participant 2 was the maker on every print in the burst. They supplied
    // the liquidity; they did not choose the moment.
    Tape t;
    ladder(t);
    burst(t);
    t.at(33000).trade(50, 400, 1, 150, Side::Sell, 108, 100);
    revert(t);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK(w.count(DetectorKind::MomentumIgnition, 50) >= 1);
    TW_CHECK_EQ(w.count(DetectorKind::MomentumIgnition, 2), static_cast<std::size_t>(0));
}
