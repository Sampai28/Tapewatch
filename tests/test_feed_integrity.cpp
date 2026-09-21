#include "tapewatch/feed_integrity.hpp"
#include "test_harness.hpp"

using namespace tapewatch;

namespace {

MarketEvent ev(Ts ts, SeqNum seq, EventId eid) {
    MarketEvent e;
    e.kind = EventKind::Order;
    e.ts = ts;
    e.seq = seq;
    e.eid = eid;
    e.symbol = "TWX";
    e.participant = 1;
    e.order_id = seq;
    e.side = Side::Buy;
    e.price = 100;
    e.quantity = 1;
    return e;
}

}  // namespace

TW_TEST(reorder_buffer_holds_until_the_watermark_passes) {
    ReorderBuffer rb(100, 64);
    std::vector<MarketEvent> out;
    rb.push(ev(0, 1, 1));
    rb.drain_ready(out);
    TW_CHECK(out.empty());  // nothing is safe yet

    rb.push(ev(150, 2, 2));
    rb.drain_ready(out);
    // watermark = 150 - 100 = 50, so ts 0 is releasable and ts 150 is not
    TW_CHECK_EQ(out.size(), static_cast<std::size_t>(1));
    TW_CHECK_EQ(out[0].ts, static_cast<Ts>(0));
}

TW_TEST(reorder_buffer_sorts_a_late_arrival_into_place) {
    ReorderBuffer rb(100, 64);
    std::vector<MarketEvent> out;
    rb.push(ev(10, 1, 1));
    rb.push(ev(40, 3, 3));
    rb.push(ev(20, 2, 2));  // out of order, inside tolerance
    TW_CHECK_EQ(rb.recovered(), static_cast<std::uint64_t>(1));

    rb.push(ev(500, 4, 4));
    rb.drain_ready(out);
    TW_CHECK_EQ(out.size(), static_cast<std::size_t>(3));
    TW_CHECK_EQ(out[0].ts, static_cast<Ts>(10));
    TW_CHECK_EQ(out[1].ts, static_cast<Ts>(20));
    TW_CHECK_EQ(out[2].ts, static_cast<Ts>(40));
}

TW_TEST(reorder_buffer_refuses_an_event_that_is_too_late) {
    ReorderBuffer rb(100, 64);
    std::vector<MarketEvent> out;
    rb.push(ev(0, 1, 1));
    rb.push(ev(1000, 2, 2));
    rb.drain_ready(out);  // releases ts 0, last_released_ts becomes 0
    TW_CHECK_EQ(out.size(), static_cast<std::size_t>(1));
    // Applying a cancel before the order it cancels corrupts the book for
    // everything downstream, so there is nothing useful to do but drop it.
    TW_CHECK(!rb.push(ev(-5, 3, 3)));
}

TW_TEST(reorder_buffer_breaks_timestamp_ties_by_sequence) {
    ReorderBuffer rb(0, 64);
    std::vector<MarketEvent> out;
    rb.push(ev(10, 7, 7));
    rb.push(ev(10, 5, 5));
    rb.push(ev(10, 6, 6));
    rb.flush(out);
    TW_CHECK_EQ(out.size(), static_cast<std::size_t>(3));
    TW_CHECK_EQ(out[0].seq, static_cast<SeqNum>(5));
    TW_CHECK_EQ(out[1].seq, static_cast<SeqNum>(6));
    TW_CHECK_EQ(out[2].seq, static_cast<SeqNum>(7));
}

TW_TEST(reorder_buffer_releases_early_rather_than_growing) {
    ReorderBuffer rb(1'000'000, 4);
    std::vector<MarketEvent> out;
    for (int i = 0; i < 10; ++i) rb.push(ev(i, i + 1, i + 1));
    rb.drain_ready(out);
    TW_CHECK_EQ(rb.size(), static_cast<std::size_t>(4));
    TW_CHECK_EQ(out.size(), static_cast<std::size_t>(6));
    TW_CHECK_EQ(rb.overflow_releases(), static_cast<std::uint64_t>(6));
}

TW_TEST(guard_drops_duplicate_event_ids) {
    FeedGuard g(1000, 16);
    FeedStats s;
    TW_CHECK(g.admit(ev(0, 1, 100), s) == FeedGuard::Verdict::Accept);
    TW_CHECK(g.admit(ev(1, 2, 100), s) == FeedGuard::Verdict::DropDuplicate);
    TW_CHECK_EQ(s.duplicate_event_ids, static_cast<std::uint64_t>(1));
    TW_CHECK_EQ(s.events_admitted, static_cast<std::uint64_t>(1));
    // A duplicate taints the feed: double-counting a print is exactly the
    // input a wash-trade test is most sensitive to.
    TW_CHECK(g.degraded(500));
}

TW_TEST(guard_counts_a_sequence_gap_and_what_it_cost) {
    FeedGuard g(1000, 16);
    FeedStats s;
    g.admit(ev(0, 10, 1), s);
    g.admit(ev(1, 15, 2), s);
    TW_CHECK_EQ(s.sequence_gaps, static_cast<std::uint64_t>(1));
    TW_CHECK_EQ(s.sequence_missing, static_cast<std::uint64_t>(4));
    TW_CHECK(g.degraded(1));
    TW_CHECK(!g.degraded(5000));
}

TW_TEST(guard_flags_timestamp_and_sequence_disagreement) {
    FeedGuard g(1000, 16);
    FeedStats s;
    g.admit(ev(0, 10, 1), s);
    // Arrives later in time but earlier in sequence: one of the two orderings
    // is lying and we cannot tell which.
    g.admit(ev(5, 9, 2), s);
    TW_CHECK_EQ(s.sequence_backward, static_cast<std::uint64_t>(1));
    TW_CHECK_EQ(s.events_admitted, static_cast<std::uint64_t>(2));
    TW_CHECK(g.degraded(5));
}

TW_TEST(guard_dedup_memory_is_bounded) {
    FeedGuard g(0, 4);
    FeedStats s;
    for (EventId i = 1; i <= 10; ++i) g.admit(ev(static_cast<Ts>(i), i, i), s);
    // eid 1 has aged out of the bounded set, so it is no longer recognised.
    // That is the price of not growing without limit, and it is deliberate.
    TW_CHECK(g.admit(ev(11, 11, 1), s) == FeedGuard::Verdict::Accept);
    TW_CHECK(g.admit(ev(12, 12, 10), s) == FeedGuard::Verdict::DropDuplicate);
}

TW_TEST(guard_counts_events_processed_while_degraded) {
    FeedGuard g(1000, 16);
    FeedStats s;
    g.admit(ev(0, 1, 1), s);
    g.admit(ev(10, 5, 2), s);  // gap, taints until 1010
    g.admit(ev(20, 6, 3), s);
    TW_CHECK_EQ(s.degraded_events, static_cast<std::uint64_t>(2));
}
