#include "tapewatch/book_state.hpp"
#include "test_harness.hpp"

using namespace tapewatch;

namespace {

MarketEvent order(Ts ts, SeqNum seq, ParticipantId pid, OrderId oid, Side s, Price px, Qty q) {
    MarketEvent e;
    e.kind = EventKind::Order;
    e.ts = ts;
    e.seq = seq;
    e.eid = seq;
    e.symbol = "TWX";
    e.participant = pid;
    e.order_id = oid;
    e.side = s;
    e.price = px;
    e.quantity = q;
    return e;
}

MarketEvent cancel(Ts ts, SeqNum seq, ParticipantId pid, OrderId oid) {
    MarketEvent e = order(ts, seq, pid, oid, Side::Buy, 0, 0);
    e.kind = EventKind::Cancel;
    return e;
}

MarketEvent trade(Ts ts, SeqNum seq, ParticipantId taker, ParticipantId maker, OrderId maker_oid,
                  Side aggressor, Price px, Qty q) {
    MarketEvent e;
    e.kind = EventKind::Trade;
    e.ts = ts;
    e.seq = seq;
    e.eid = seq;
    e.symbol = "TWX";
    e.participant = taker;
    e.order_id = 9000 + seq;
    e.maker_participant = maker;
    e.maker_order_id = maker_oid;
    e.side = aggressor;
    e.price = px;
    e.quantity = q;
    return e;
}

}  // namespace

TW_TEST(book_tracks_touch_and_depth) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Buy, 100, 50));
    b.on_order(order(2, 2, 10, 2, Side::Buy, 99, 30));
    b.on_order(order(3, 3, 11, 3, Side::Sell, 102, 20));

    TW_CHECK_EQ(b.best(Side::Buy), static_cast<Price>(100));
    TW_CHECK_EQ(b.best(Side::Sell), static_cast<Price>(102));
    TW_CHECK_EQ(b.mid2(), static_cast<Price>(202));
    TW_CHECK_EQ(b.side_qty(Side::Buy), static_cast<Qty>(80));
    TW_CHECK_EQ(b.level_count(Side::Buy), static_cast<std::size_t>(2));
    TW_CHECK_EQ(b.qty_within(Side::Buy, 0, 1), static_cast<Qty>(50));
    TW_CHECK_EQ(b.qty_within(Side::Buy, 1, 1), static_cast<Qty>(80));
    TW_CHECK(!b.crossed());
}

TW_TEST(book_records_state_before_the_order_joined) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Buy, 100, 50));
    b.on_order(order(2, 2, 10, 2, Side::Buy, 100, 40));
    // The second order must see a depth of 50, not 90. Measuring after the
    // order lands lets a large enough order normalise itself.
    const RestingOrder* r = b.find(2);
    TW_CHECK(r != nullptr);
    TW_CHECK_EQ(r->side_qty_at_entry, static_cast<Qty>(50));
    TW_CHECK_EQ(r->side_orders_at_entry, 1u);
    TW_CHECK_EQ(r->ticks_from_touch_at_entry, 0);
}

TW_TEST(book_first_order_on_an_empty_side_is_at_the_touch) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Buy, 100, 50));
    const RestingOrder* r = b.find(1);
    TW_CHECK(r != nullptr);
    // -1 would mean "no touch", and would make the pressure signal discard the
    // most aggressive placement there is.
    TW_CHECK_EQ(r->ticks_from_touch_at_entry, 0);
}

TW_TEST(book_measures_distance_from_touch_in_ticks) {
    BookState b(1000, 5);
    b.on_order(order(1, 1, 10, 1, Side::Buy, 1000, 10));
    b.on_order(order(2, 2, 10, 2, Side::Buy, 985, 10));
    TW_CHECK_EQ(b.find(2)->ticks_from_touch_at_entry, 3);
}

TW_TEST(book_cancel_returns_the_removed_order) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Buy, 100, 50));
    RestingOrder removed;
    TW_CHECK(b.on_cancel(cancel(500, 2, 10, 1), removed));
    TW_CHECK_EQ(removed.remaining, static_cast<Qty>(50));
    TW_CHECK_EQ(removed.entry_ts, static_cast<Ts>(1));
    TW_CHECK_EQ(removed.lifetime(500), static_cast<Ts>(499));
    TW_CHECK_EQ(b.side_qty(Side::Buy), static_cast<Qty>(0));
    TW_CHECK_EQ(b.resting_count(), static_cast<std::size_t>(0));
}

TW_TEST(book_unknown_cancel_is_counted_not_fatal) {
    BookState b(1000, 1);
    RestingOrder removed;
    TW_CHECK(!b.on_cancel(cancel(1, 1, 10, 777), removed));
    TW_CHECK_EQ(b.stats().unknown_cancel, static_cast<std::uint64_t>(1));
}

TW_TEST(book_partial_fill_keeps_the_order_and_its_level_count) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Sell, 102, 50));
    b.on_order(order(2, 2, 11, 2, Side::Sell, 102, 10));
    TW_CHECK_EQ(b.side_orders(Side::Sell), 2u);

    RestingOrder maker;
    bool gone = true;
    TW_CHECK(b.on_trade(trade(3, 3, 12, 10, 1, Side::Buy, 102, 20), maker, gone));
    TW_CHECK(!gone);
    TW_CHECK_EQ(maker.remaining, static_cast<Qty>(30));
    TW_CHECK_EQ(maker.filled, static_cast<Qty>(20));
    TW_CHECK_EQ(b.side_qty(Side::Sell), static_cast<Qty>(40));
    // Two orders still rest at 102; a partial fill must not decrement the
    // count, or mean_order_qty drifts upward with every print.
    TW_CHECK_EQ(b.side_orders(Side::Sell), 2u);
    TW_CHECK_EQ(b.asks().at(102).orders, 2u);
}

TW_TEST(book_full_fill_removes_the_order) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Sell, 102, 20));
    RestingOrder maker;
    bool gone = false;
    TW_CHECK(b.on_trade(trade(2, 2, 12, 10, 1, Side::Buy, 102, 20), maker, gone));
    TW_CHECK(gone);
    TW_CHECK_EQ(b.resting_count(), static_cast<std::size_t>(0));
    TW_CHECK_EQ(b.side_orders(Side::Sell), 0u);
    TW_CHECK_EQ(b.best(Side::Sell), kNoPrice);
}

TW_TEST(book_oversized_fill_is_clamped_and_counted) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Sell, 102, 5));
    RestingOrder maker;
    bool gone = false;
    b.on_trade(trade(2, 2, 12, 10, 1, Side::Buy, 102, 50), maker, gone);
    TW_CHECK(gone);
    TW_CHECK_EQ(maker.filled, static_cast<Qty>(5));
    TW_CHECK_EQ(b.stats().oversized_fill, static_cast<std::uint64_t>(1));
    TW_CHECK_EQ(b.side_qty(Side::Sell), static_cast<Qty>(0));
}

TW_TEST(book_modify_at_the_same_price_keeps_the_clock_running) {
    BookState b(1000, 1);
    b.on_order(order(100, 1, 10, 1, Side::Buy, 100, 50));
    MarketEvent m = order(900, 2, 10, 1, Side::Buy, 100, 30);
    m.kind = EventKind::Modify;
    RestingOrder before, after;
    TW_CHECK(b.on_modify(m, before, after));
    // A size-down at the same price does not lose priority, so surveillance
    // must not treat the order as newly placed either.
    TW_CHECK_EQ(after.entry_ts, static_cast<Ts>(100));
    TW_CHECK_EQ(after.remaining, static_cast<Qty>(30));
    TW_CHECK_EQ(b.side_qty(Side::Buy), static_cast<Qty>(30));
}

TW_TEST(book_modify_that_moves_price_restarts_the_clock) {
    BookState b(1000, 1);
    b.on_order(order(100, 1, 10, 1, Side::Buy, 100, 50));
    MarketEvent m = order(900, 2, 10, 1, Side::Buy, 99, 50);
    m.kind = EventKind::Modify;
    RestingOrder before, after;
    TW_CHECK(b.on_modify(m, before, after));
    TW_CHECK_EQ(after.entry_ts, static_cast<Ts>(900));
    TW_CHECK_EQ(after.price, static_cast<Price>(99));
    TW_CHECK_EQ(b.best(Side::Buy), static_cast<Price>(99));
}

TW_TEST(book_modify_to_zero_is_a_withdrawal) {
    BookState b(1000, 1);
    b.on_order(order(100, 1, 10, 1, Side::Buy, 100, 50));
    MarketEvent m = order(900, 2, 10, 1, Side::Buy, 100, 0);
    m.kind = EventKind::Modify;
    RestingOrder before, after;
    TW_CHECK(b.on_modify(m, before, after));
    TW_CHECK_EQ(b.resting_count(), static_cast<std::size_t>(0));
    TW_CHECK_EQ(before.remaining, static_cast<Qty>(50));
}

TW_TEST(book_respects_its_order_cap) {
    BookState b(2, 1);
    TW_CHECK(b.on_order(order(1, 1, 10, 1, Side::Buy, 100, 1)));
    TW_CHECK(b.on_order(order(2, 2, 10, 2, Side::Buy, 100, 1)));
    TW_CHECK(!b.on_order(order(3, 3, 10, 3, Side::Buy, 100, 1)));
    TW_CHECK_EQ(b.stats().orders_dropped_cap, static_cast<std::uint64_t>(1));
    TW_CHECK_EQ(b.resting_count(), static_cast<std::size_t>(2));
}

TW_TEST(book_duplicate_order_id_is_rejected) {
    BookState b(1000, 1);
    TW_CHECK(b.on_order(order(1, 1, 10, 1, Side::Buy, 100, 1)));
    TW_CHECK(!b.on_order(order(2, 2, 10, 1, Side::Buy, 100, 1)));
    TW_CHECK_EQ(b.stats().duplicate_order_id, static_cast<std::uint64_t>(1));
}

TW_TEST(book_notices_a_crossed_market) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Sell, 100, 10));
    b.on_order(order(2, 2, 11, 2, Side::Buy, 101, 10));
    TW_CHECK(b.crossed());
    TW_CHECK_EQ(b.stats().crossed_observations, static_cast<std::uint64_t>(1));
}

TW_TEST(book_repair_discards_the_stale_side_of_a_cross) {
    BookState b(1000, 1);
    // An offer at 100 that we never saw cancelled, then the market trades up
    // around it. The stale offer is the older one, and it is the one to go.
    b.on_order(order(1, 1, 10, 1, Side::Sell, 100, 10));
    b.on_order(order(500, 2, 11, 2, Side::Sell, 105, 10));
    b.on_order(order(600, 3, 12, 3, Side::Buy, 101, 10));
    TW_CHECK(b.crossed());

    const std::size_t removed = b.repair_cross();
    TW_CHECK_EQ(removed, static_cast<std::size_t>(1));
    TW_CHECK(!b.crossed());
    TW_CHECK_EQ(b.best(Side::Sell), static_cast<Price>(105));
    TW_CHECK_EQ(b.best(Side::Buy), static_cast<Price>(101));
    TW_CHECK_EQ(b.stats().stale_orders_removed, static_cast<std::uint64_t>(1));
    TW_CHECK_EQ(b.stats().stale_levels_removed, static_cast<std::uint64_t>(1));
    // Depth and the participant footprint have to follow the repair, or the
    // next size comparison is against a book that no longer exists.
    TW_CHECK_EQ(b.side_qty(Side::Sell), static_cast<Qty>(10));
    TW_CHECK_EQ(b.side_orders(Side::Sell), 1u);
    TW_CHECK_EQ(b.participant_qty(10, Side::Sell), static_cast<Qty>(0));
}

TW_TEST(book_repair_does_nothing_to_an_uncrossed_book) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Buy, 99, 10));
    b.on_order(order(2, 2, 11, 2, Side::Sell, 101, 10));
    TW_CHECK_EQ(b.repair_cross(), static_cast<std::size_t>(0));
    TW_CHECK_EQ(b.stats().stale_levels_removed, static_cast<std::uint64_t>(0));
    TW_CHECK_EQ(b.resting_count(), static_cast<std::size_t>(2));
}

TW_TEST(book_repair_clears_several_stale_levels) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Sell, 100, 10));
    b.on_order(order(2, 2, 10, 2, Side::Sell, 101, 10));
    b.on_order(order(3, 3, 10, 3, Side::Sell, 102, 10));
    b.on_order(order(500, 4, 11, 4, Side::Sell, 110, 10));
    b.on_order(order(600, 5, 12, 5, Side::Buy, 105, 10));
    TW_CHECK(b.crossed());
    TW_CHECK_EQ(b.repair_cross(), static_cast<std::size_t>(3));
    TW_CHECK(!b.crossed());
    TW_CHECK_EQ(b.best(Side::Sell), static_cast<Price>(110));
}

TW_TEST(book_tracks_per_participant_footprint) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Buy, 100, 50));
    b.on_order(order(2, 2, 10, 2, Side::Sell, 102, 20));
    b.on_order(order(3, 3, 11, 3, Side::Buy, 99, 70));
    TW_CHECK_EQ(b.participant_qty(10, Side::Buy), static_cast<Qty>(50));
    TW_CHECK_EQ(b.participant_qty(10, Side::Sell), static_cast<Qty>(20));
    TW_CHECK_EQ(b.participant_qty(11, Side::Buy), static_cast<Qty>(70));
    TW_CHECK_EQ(b.participant_orders(10, Side::Buy), 1u);

    RestingOrder removed;
    b.on_cancel(cancel(4, 4, 10, 1), removed);
    TW_CHECK_EQ(b.participant_qty(10, Side::Buy), static_cast<Qty>(0));
    TW_CHECK_EQ(b.participant_orders(10, Side::Buy), 0u);
}

TW_TEST(book_top_is_best_first_on_both_sides) {
    BookState b(1000, 1);
    b.on_order(order(1, 1, 10, 1, Side::Buy, 98, 1));
    b.on_order(order(2, 2, 10, 2, Side::Buy, 100, 2));
    b.on_order(order(3, 3, 10, 3, Side::Buy, 99, 3));
    b.on_order(order(4, 4, 11, 4, Side::Sell, 104, 4));
    b.on_order(order(5, 5, 11, 5, Side::Sell, 102, 5));

    const auto bids = b.top(Side::Buy, 3);
    TW_CHECK_EQ(bids.size(), static_cast<std::size_t>(3));
    TW_CHECK_EQ(bids[0].price, static_cast<Price>(100));
    TW_CHECK_EQ(bids[2].price, static_cast<Price>(98));

    const auto asks = b.top(Side::Sell, 3);
    TW_CHECK_EQ(asks.size(), static_cast<std::size_t>(2));
    TW_CHECK_EQ(asks[0].price, static_cast<Price>(102));
}

TW_TEST(book_mean_order_qty_is_zero_on_an_empty_side) {
    BookState b(1000, 1);
    TW_CHECK_NEAR(b.mean_order_qty(Side::Buy), 0.0, 1e-9);
    b.on_order(order(1, 1, 10, 1, Side::Buy, 100, 60));
    b.on_order(order(2, 2, 10, 2, Side::Buy, 99, 40));
    TW_CHECK_NEAR(b.mean_order_qty(Side::Buy), 50.0, 1e-9);
}
