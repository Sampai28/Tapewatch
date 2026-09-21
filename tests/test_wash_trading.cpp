#include "tape_builder.hpp"
#include "test_harness.hpp"

using namespace tapewatch;
using namespace twtest;

TW_TEST(wash_fires_on_a_direct_self_trade) {
    Tape t;
    t.at(1000);
    t.order(50, 500, Side::Sell, 101, 60);
    t.at(1100).trade(50, 501, 50, 500, Side::Buy, 101, 60);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);

    TW_CHECK(w.count(DetectorKind::WashTrading, 50) >= 1);
    const Alert* a = w.first(DetectorKind::WashTrading);
    TW_CHECK(a != nullptr);
    if (!a) return;
    TW_CHECK(a->has_counterparty);
    TW_CHECK_EQ(a->counterparty, 50u);
    for (const auto& [k, v] : a->evidence)
        if (k == "is_self_trade") TW_CHECK_EQ(v, static_cast<std::int64_t>(1));
}

TW_TEST(wash_fires_on_a_reciprocal_pair) {
    Tape t;
    t.at(1000);
    t.order(51, 510, Side::Sell, 101, 100);
    t.order(50, 500, Side::Sell, 101, 100);
    // 50 buys 100 from 51, then 51 buys the same 100 back from 50.
    t.at(1100).trade(50, 501, 51, 510, Side::Buy, 101, 100);
    t.at(1200).trade(51, 511, 50, 500, Side::Buy, 101, 100);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);

    TW_CHECK(w.count(DetectorKind::WashTrading) >= 1);
    const Alert* a = w.first(DetectorKind::WashTrading);
    TW_CHECK(a != nullptr);
    if (!a) return;
    TW_CHECK_EQ(a->participant, 50u);
    TW_CHECK_EQ(a->counterparty, 51u);
    for (const auto& [k, v] : a->evidence) {
        if (k == "qty_lo_to_hi") TW_CHECK_EQ(v, static_cast<std::int64_t>(100));
        if (k == "qty_hi_to_lo") TW_CHECK_EQ(v, static_cast<std::int64_t>(100));
        if (k == "counterparty_breadth") TW_CHECK_EQ(v, static_cast<std::int64_t>(1));
    }
}

TW_TEST(wash_ignores_one_directional_flow) {
    // 50 buys from 51 five times and never sells back. That is a position,
    // not a wash.
    Tape t;
    t.at(1000);
    for (int i = 0; i < 5; ++i) {
        t.order(51, 600 + i, Side::Sell, 101, 40);
        t.advance(50).trade(50, 700 + i, 51, 600 + i, Side::Buy, 101, 40);
        t.advance(50);
    }

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::WashTrading), static_cast<std::size_t>(0));
}

TW_TEST(wash_leaves_a_broad_two_way_market_maker_alone) {
    // Participant 60 trades both ways with six different counterparties and
    // ends the window flat. Reciprocity, flatness and repetition are all
    // maxed out; only breadth and concentration say this is a market maker.
    Tape t;
    t.at(1000);
    OrderId oid = 1000;
    for (ParticipantId cp = 70; cp < 76; ++cp) {
        const OrderId ask = oid++;
        const OrderId lift = oid++;
        const OrderId bid = oid++;
        const OrderId hit = oid++;
        t.order(60, ask, Side::Sell, 101, 40);
        t.advance(20).trade(cp, lift, 60, ask, Side::Buy, 101, 40);
        t.advance(20).order(60, bid, Side::Buy, 100, 40);
        t.advance(20).trade(cp, hit, 60, bid, Side::Sell, 100, 40);
        t.advance(20);
    }

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::WashTrading), static_cast<std::size_t>(0));
}

TW_TEST(wash_ignores_a_reciprocal_pair_below_the_size_floor) {
    Tape t;
    t.at(1000);
    t.order(51, 510, Side::Sell, 101, 5);
    t.order(50, 500, Side::Sell, 101, 5);
    t.at(1100).trade(50, 501, 51, 510, Side::Buy, 101, 5);
    t.at(1200).trade(51, 511, 50, 500, Side::Buy, 101, 5);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(w.count(DetectorKind::WashTrading), static_cast<std::size_t>(0));
}

TW_TEST(wash_self_trade_does_not_move_the_net_position) {
    // A participant on both sides of a print has bought and sold the same
    // lot. Counting it as a buy would make `nonet` look like directional
    // trading and suppress the clearest signal there is.
    Tape t;
    t.at(1000);
    t.order(50, 500, Side::Sell, 101, 80);
    t.at(1100).trade(50, 501, 50, 500, Side::Buy, 101, 80);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    const Alert* a = w.first(DetectorKind::WashTrading);
    TW_CHECK(a != nullptr);
    if (!a) return;
    for (const auto& s : a->signals) {
        if (s.name == "nonet") TW_CHECK_NEAR(s.value, 1.0, 1e-9);
        if (s.name == "self") TW_CHECK_NEAR(s.value, 1.0, 1e-9);
    }
}
