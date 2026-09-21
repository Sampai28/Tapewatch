#include "tapewatch/book_state.hpp"

#include <algorithm>

namespace tapewatch {

void BookState::add_level(Side s, Price px, Qty q, Ts ts) {
    auto& m = levels(s);
    auto& agg = m[px];
    agg.quantity += q;
    agg.orders += 1;
    agg.newest_ts = std::max(agg.newest_ts, ts);
    side_qty_[static_cast<int>(s)] += q;
    side_orders_[static_cast<int>(s)] += 1;
}

void BookState::remove_level(Side s, Price px, Qty q) {
    auto& m = levels(s);
    auto it = m.find(px);
    if (it == m.end()) return;
    it->second.quantity -= q;
    if (it->second.orders > 0) it->second.orders -= 1;
    side_qty_[static_cast<int>(s)] -= q;
    if (side_orders_[static_cast<int>(s)] > 0) side_orders_[static_cast<int>(s)] -= 1;
    if (it->second.quantity <= 0 || it->second.orders == 0) m.erase(it);
}

void BookState::reduce_level(Side s, Price px, Qty q) {
    auto& m = levels(s);
    auto it = m.find(px);
    if (it == m.end()) return;
    it->second.quantity -= q;
    side_qty_[static_cast<int>(s)] -= q;
    if (it->second.quantity <= 0) {
        side_orders_[static_cast<int>(s)] -=
            std::min<std::uint32_t>(side_orders_[static_cast<int>(s)], it->second.orders);
        m.erase(it);
    }
}

void BookState::note_crossed() {
    if (crossed()) ++stats_.crossed_observations;
}

bool BookState::on_order(const MarketEvent& e) {
    if (orders_.count(e.order_id)) {
        ++stats_.duplicate_order_id;
        return false;
    }
    if (orders_.size() >= max_orders_) {
        ++stats_.orders_dropped_cap;
        return false;
    }
    if (e.quantity <= 0 || e.price == kNoPrice) {
        // A zero-quantity or unpriced resting order is not a book state we can
        // represent. Marketable flow shows up as trades, which do not come
        // through here.
        return false;
    }

    RestingOrder r{};
    r.participant = e.participant;
    r.id = e.order_id;
    r.side = e.side;
    r.price = e.price;
    r.original_qty = e.quantity;
    r.remaining = e.quantity;
    r.filled = 0;
    r.entry_ts = e.ts;
    r.entry_seq = e.seq;
    r.side_qty_at_entry = side_qty(e.side);
    r.side_orders_at_entry = side_orders_[static_cast<int>(e.side)];
    {
        // An empty side has no touch to measure against, but the arriving
        // order becomes the touch, so the honest answer is zero rather than
        // "unknown" -- and "unknown" would make the pressure signal drop the
        // most aggressive placements there are.
        const int t = ticks_from_touch(e.side, e.price, tick_size_);
        r.ticks_from_touch_at_entry = t < 0 ? 0 : t;
    }
    orders_.emplace(e.order_id, r);

    add_level(e.side, e.price, e.quantity, e.ts);
    auto& fp = footprint_[e.participant];
    fp.qty[static_cast<int>(e.side)] += e.quantity;
    fp.orders[static_cast<int>(e.side)] += 1;

    ++stats_.orders_added;
    note_crossed();
    return true;
}

bool BookState::on_cancel(const MarketEvent& e, RestingOrder& removed) {
    auto it = orders_.find(e.order_id);
    if (it == orders_.end()) {
        ++stats_.unknown_cancel;
        return false;
    }
    removed = it->second;

    remove_level(removed.side, removed.price, removed.remaining);
    auto& fp = footprint_[removed.participant];
    const int si = static_cast<int>(removed.side);
    fp.qty[si] -= removed.remaining;
    if (fp.orders[si] > 0) fp.orders[si] -= 1;

    orders_.erase(it);
    return true;
}

bool BookState::on_modify(const MarketEvent& e, RestingOrder& before, RestingOrder& after) {
    auto it = orders_.find(e.order_id);
    if (it == orders_.end()) {
        ++stats_.unknown_modify;
        return false;
    }
    before = it->second;
    RestingOrder& r = it->second;

    const Qty new_qty = e.quantity;
    const Price new_px = e.price == kNoPrice ? r.price : e.price;

    remove_level(r.side, r.price, r.remaining);
    auto& fp = footprint_[r.participant];
    const int si = static_cast<int>(r.side);
    fp.qty[si] -= r.remaining;
    if (fp.orders[si] > 0) fp.orders[si] -= 1;

    if (new_qty <= 0) {
        // An amend to zero is a cancel in every venue that allows it.
        after = r;
        after.remaining = 0;
        orders_.erase(it);
        return true;
    }

    const bool price_changed = new_px != r.price;
    r.price = new_px;
    r.remaining = new_qty;
    r.original_qty = std::max(r.original_qty, new_qty + r.filled);
    r.modifies += 1;
    // Losing price priority restarts the clock. Keeping it does not.
    if (price_changed) {
        r.entry_ts = e.ts;
        r.entry_seq = e.seq;
    }

    add_level(r.side, r.price, r.remaining, e.ts);
    fp.qty[si] += r.remaining;
    fp.orders[si] += 1;

    after = r;
    note_crossed();
    return true;
}

bool BookState::on_trade(const MarketEvent& e, RestingOrder& maker_snapshot, bool& maker_gone) {
    maker_gone = false;
    auto it = orders_.find(e.maker_order_id);
    if (it == orders_.end()) {
        ++stats_.unknown_maker;
        return false;
    }
    RestingOrder& r = it->second;

    Qty fill = e.quantity;
    if (fill > r.remaining) {
        ++stats_.oversized_fill;
        fill = r.remaining;
    }

    auto& fp = footprint_[r.participant];
    const int si = static_cast<int>(r.side);
    fp.qty[si] -= fill;

    r.remaining -= fill;
    r.filled += fill;

    if (r.remaining <= 0) {
        remove_level(r.side, r.price, fill);
        if (fp.orders[si] > 0) fp.orders[si] -= 1;
        maker_snapshot = r;
        maker_gone = true;
        orders_.erase(it);
    } else {
        reduce_level(r.side, r.price, fill);
        maker_snapshot = r;
    }
    return true;
}

void BookState::erase_order(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    const RestingOrder r = it->second;
    orders_.erase(it);
    remove_level(r.side, r.price, r.remaining);
    auto& fp = footprint_[r.participant];
    const int si = static_cast<int>(r.side);
    fp.qty[si] -= r.remaining;
    if (fp.orders[si] > 0) fp.orders[si] -= 1;
}

std::size_t BookState::repair_cross() {
    std::size_t removed = 0;
    // A bound rather than a while(true): a repair that needs more than this
    // many levels is not a stale order, it is a broken reconstruction, and
    // spinning on it would turn a data problem into a hang.
    for (int guard = 0; guard < 64 && crossed(); ++guard) {
        const Price bpx = best(Side::Buy);
        const Price apx = best(Side::Sell);
        const Ts bnew = bids_.at(bpx).newest_ts;
        const Ts anew = asks_.at(apx).newest_ts;
        const Side stale = bnew <= anew ? Side::Buy : Side::Sell;
        const Price px = stale == Side::Buy ? bpx : apx;

        std::vector<OrderId> ids;
        for (const auto& [id, o] : orders_)
            if (o.side == stale && o.price == px) ids.push_back(id);

        if (ids.empty()) {
            // An aggregate with no orders behind it. Nothing to attribute, so
            // drop the level directly and keep the totals consistent.
            auto& m = levels(stale);
            auto it = m.find(px);
            if (it != m.end()) {
                side_qty_[static_cast<int>(stale)] -= it->second.quantity;
                m.erase(it);
            }
            ++stats_.stale_levels_removed;
            continue;
        }
        for (OrderId id : ids) {
            erase_order(id);
            ++removed;
        }
        ++stats_.stale_levels_removed;
    }
    stats_.stale_orders_removed += removed;
    return removed;
}

Price BookState::best(Side s) const {
    if (s == Side::Buy) {
        if (bids_.empty()) return kNoPrice;
        return bids_.rbegin()->first;
    }
    if (asks_.empty()) return kNoPrice;
    return asks_.begin()->first;
}

bool BookState::crossed() const {
    const Price bb = best(Side::Buy);
    const Price ba = best(Side::Sell);
    return bb != kNoPrice && ba != kNoPrice && bb >= ba;
}

Price BookState::mid2() const {
    const Price bb = best(Side::Buy);
    const Price ba = best(Side::Sell);
    if (bb == kNoPrice || ba == kNoPrice) return kNoPrice;
    return bb + ba;
}

Qty BookState::qty_within(Side s, int ticks, Price tick_size) const {
    const Price b = best(s);
    if (b == kNoPrice || tick_size <= 0 || ticks < 0) return 0;
    const Price span = static_cast<Price>(ticks) * tick_size;
    Qty total = 0;
    if (s == Side::Buy) {
        const Price floor_px = b - span;
        for (auto it = bids_.rbegin(); it != bids_.rend() && it->first >= floor_px; ++it)
            total += it->second.quantity;
    } else {
        const Price ceil_px = b + span;
        for (auto it = asks_.begin(); it != asks_.end() && it->first <= ceil_px; ++it)
            total += it->second.quantity;
    }
    return total;
}

double BookState::mean_order_qty(Side s) const {
    const std::uint32_t orders = side_orders_[static_cast<int>(s)];
    if (orders == 0) return 0.0;
    return static_cast<double>(side_qty(s)) / static_cast<double>(orders);
}

Qty BookState::participant_qty(ParticipantId p, Side s) const {
    auto it = footprint_.find(p);
    if (it == footprint_.end()) return 0;
    return it->second.qty[static_cast<int>(s)];
}

std::uint32_t BookState::participant_orders(ParticipantId p, Side s) const {
    auto it = footprint_.find(p);
    if (it == footprint_.end()) return 0;
    return it->second.orders[static_cast<int>(s)];
}

int BookState::ticks_from_touch(Side s, Price px, Price tick_size) const {
    const Price b = best(s);
    if (b == kNoPrice || tick_size <= 0) return -1;
    const Price diff = s == Side::Buy ? (b - px) : (px - b);
    if (diff <= 0) return 0;
    return static_cast<int>(diff / tick_size);
}

std::vector<BookState::Level> BookState::top(Side s, std::size_t depth) const {
    std::vector<Level> out;
    out.reserve(depth);
    if (s == Side::Buy) {
        for (auto it = bids_.rbegin(); it != bids_.rend() && out.size() < depth; ++it)
            out.push_back({it->first, it->second.quantity, it->second.orders});
    } else {
        for (auto it = asks_.begin(); it != asks_.end() && out.size() < depth; ++it)
            out.push_back({it->first, it->second.quantity, it->second.orders});
    }
    return out;
}

}  // namespace tapewatch
