// Order book reconstruction from the normalised tape.
//
// This is not a matching engine. It never decides anything; it replays what a
// venue already decided, so that a detector can ask questions the raw event
// stream cannot answer -- how deep was the book when that order landed, how
// long had it been resting, how much of it ever traded.
//
// Ordering matches Matchbook's BookSnapshot: bids descending, asks ascending.
//
// Memory is capped at construction. A tape that resting-orders its way past
// the cap gets the new order *rejected and counted*, not silently accepted at
// the cost of unbounded growth. Under-detecting loudly beats dying quietly.

#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <unordered_map>
#include <vector>

#include "tapewatch/event.hpp"
#include "tapewatch/types.hpp"

namespace tapewatch {

struct RestingOrder {
    ParticipantId participant{0};
    OrderId id{0};
    Side side{Side::Buy};
    Price price{kNoPrice};
    Qty original_qty{0};
    Qty remaining{0};
    Qty filled{0};
    Ts entry_ts{0};
    SeqNum entry_seq{0};
    // The state of the order's own side of the book immediately *before* this
    // order joined it. Captured here because it is the only honest denominator
    // for "was that order unusually large": comparing against the book as it
    // looks after the order lands lets a big enough order make itself look
    // normal.
    Qty side_qty_at_entry{0};
    std::uint32_t side_orders_at_entry{0};
    int ticks_from_touch_at_entry{-1};
    // A modify that changes price is, for surveillance purposes, a cancel and a
    // replace: it resets entry_ts. Counting them separately means a participant
    // cannot dodge a lifetime test by amending an order instead of pulling it.
    std::uint32_t modifies{0};

    Ts lifetime(Ts now) const { return now - entry_ts; }
    bool ever_filled() const { return filled > 0; }
};

struct LevelAgg {
    Qty quantity{0};
    std::uint32_t orders{0};
    // When this level last had an order added to it. The crossed-book repair
    // uses it to decide which side of a cross is the stale one.
    Ts newest_ts{0};
};

struct BookStats {
    std::uint64_t orders_added{0};
    std::uint64_t orders_dropped_cap{0};   // rejected because the book was full
    std::uint64_t unknown_cancel{0};       // cancel for an order we never saw
    std::uint64_t unknown_modify{0};
    std::uint64_t unknown_maker{0};        // trade against an unseen resting order
    std::uint64_t duplicate_order_id{0};
    std::uint64_t crossed_observations{0}; // best bid >= best ask after apply
    std::uint64_t oversized_fill{0};       // fill larger than the resting remainder
    std::uint64_t stale_levels_removed{0}; // price levels discarded by repair
    std::uint64_t stale_orders_removed{0};
};

class BookState {
public:
    BookState(std::size_t max_resting_orders, Price tick_size = 1)
        : max_orders_(max_resting_orders ? max_resting_orders : 1),
          tick_size_(tick_size > 0 ? tick_size : 1) {}

    Price tick_size() const { return tick_size_; }

    // --- mutation -------------------------------------------------------
    // Each returns whether the event referred to state we actually had. The
    // pipeline turns a `false` into a feed-integrity counter, not an abort.

    bool on_order(const MarketEvent& e);
    bool on_cancel(const MarketEvent& e, RestingOrder& removed);
    bool on_modify(const MarketEvent& e, RestingOrder& before, RestingOrder& after);
    // `maker_gone` is set when the fill consumed the resting order entirely.
    bool on_trade(const MarketEvent& e, RestingOrder& maker_snapshot, bool& maker_gone);

    // Uncross the book by discarding stale levels.
    //
    // A real venue's book is never crossed. If the reconstruction is, we are
    // holding an order the venue already removed and whose removal message we
    // never saw -- a dropped cancel, or a dropped fill. Left alone, one stale
    // order sitting inside the spread makes every depth, imbalance and
    // distance-from-touch measurement wrong for the rest of the session, so a
    // single lost message quietly destroys the whole run.
    //
    // The repair drops the crossing level whose most recent order is older,
    // on the reasoning that the side nobody has refreshed is the side we are
    // out of date on. It is a heuristic and it can be wrong; what it cannot
    // do is be silently wrong, because every level it discards is counted and
    // the count is reported. On an undamaged feed it never fires, and the
    // test suite asserts that.
    std::size_t repair_cross();

    // --- queries --------------------------------------------------------

    const RestingOrder* find(OrderId id) const {
        auto it = orders_.find(id);
        return it == orders_.end() ? nullptr : &it->second;
    }

    Price best(Side s) const;
    bool crossed() const;
    // Twice the midpoint, so an odd spread does not round away. Detectors that
    // measure price movement work in mid2 units and halve only for display.
    Price mid2() const;

    Qty side_qty(Side s) const { return side_qty_[static_cast<int>(s)]; }
    std::uint32_t side_orders(Side s) const { return side_orders_[static_cast<int>(s)]; }
    std::size_t level_count(Side s) const { return levels(s).size(); }
    std::size_t resting_count() const { return orders_.size(); }

    // Total quantity resting within `ticks` price levels of the touch, on the
    // given side. This is the denominator for "how big was that order really".
    Qty qty_within(Side s, int ticks, Price tick_size) const;

    // Mean quantity per order on a side; 0 when the side is empty. Used to
    // scale an order's size against what the book normally looks like.
    double mean_order_qty(Side s) const;

    Qty participant_qty(ParticipantId p, Side s) const;
    std::uint32_t participant_orders(ParticipantId p, Side s) const;

    // Distance from the touch in ticks, >= 0. Returns -1 when the side is empty
    // (no touch to measure against) so callers can tell "at the touch" from
    // "no touch at all".
    int ticks_from_touch(Side s, Price px, Price tick_size) const;

    const std::map<Price, LevelAgg>& bids() const { return bids_; }
    const std::map<Price, LevelAgg>& asks() const { return asks_; }

    const BookStats& stats() const { return stats_; }

    // Top `depth` levels, best first, matching Matchbook's snapshot ordering.
    struct Level {
        Price price;
        Qty quantity;
        std::uint32_t orders;
    };
    std::vector<Level> top(Side s, std::size_t depth) const;

private:
    std::map<Price, LevelAgg>& levels(Side s) { return s == Side::Buy ? bids_ : asks_; }
    const std::map<Price, LevelAgg>& levels(Side s) const { return s == Side::Buy ? bids_ : asks_; }

    void add_level(Side s, Price px, Qty q, Ts ts);
    void erase_order(OrderId id);
    void remove_level(Side s, Price px, Qty q);
    // A fill against a resting order that survives it: quantity leaves the
    // level but the order does not, so the order count must not move.
    void reduce_level(Side s, Price px, Qty q);
    void note_crossed();

    std::unordered_map<OrderId, RestingOrder> orders_;
    std::map<Price, LevelAgg> bids_;
    std::map<Price, LevelAgg> asks_;
    std::array<Qty, 2> side_qty_{0, 0};
    std::array<std::uint32_t, 2> side_orders_{0, 0};

    struct Footprint {
        std::array<Qty, 2> qty{0, 0};
        std::array<std::uint32_t, 2> orders{0, 0};
    };
    std::unordered_map<ParticipantId, Footprint> footprint_;

    std::size_t max_orders_;
    Price tick_size_{1};
    BookStats stats_{};
};

}  // namespace tapewatch
