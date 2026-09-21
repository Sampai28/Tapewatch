// Layering.
//
// Spoofing's plural. Instead of one conspicuous order, a stack of them across
// several price levels on one side, which is harder to spot per-order --
// each individual order looks ordinary -- and produces a more convincing wall
// than a single large order at one level, because depth spread over levels
// reads as genuine interest rather than as one participant's bluff.
//
// The tell is coordination. Real interest at five price levels arrives and
// leaves independently, because the reasons for it are independent. A layer
// arrives together and, crucially, *leaves together*: one decision cancels
// all of it, within a fraction of a second, right about when the participant
// trades the other way.
//
// So this detector clusters a participant's cancels by side and by time, and
// asks four questions of each cluster: how many distinct levels, how much of
// that side of the book was theirs, how tightly did the cancels group, and did
// they trade the other way. As with spoofing, the last one is a hard gate.
//
// Layering and spoofing will both fire on the same behaviour when a layer
// contains one dominant order. That is left alone rather than suppressed: the
// evaluation harness reports typed matches, so a layering episode caught only
// by the spoofing detector shows up as a miss for layering, which is
// information, and the console groups alerts by participant and time so an
// analyst sees one case, not two.

#include <algorithm>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "common.hpp"
#include "tapewatch/detector.hpp"

namespace tapewatch {
namespace {

class LayeringDetector final : public IDetector {
public:
    LayeringDetector(const LayeringConfig& cfg, Price tick_size) : cfg_(cfg), tick_(tick_size) {}

    DetectorKind kind() const override { return DetectorKind::Layering; }
    bool enabled() const override { return cfg_.enabled; }

    void scan(const SymbolContext& ctx, Ts from, Ts to, AlertSink& sink) override {
        const Ts w = cfg_.opposite_window_ns;
        const Ts anchor_hi = to - w;
        const Ts anchor_lo = from - w;
        if (anchor_hi <= anchor_lo) return;

        // Gather every cancel that could belong to a cluster ending in the
        // slice, keyed by participant and side. The extra cancel_span of
        // lookback is what lets a cluster that began before the slice still be
        // evaluated whole.
        const Ts gather_lo = anchor_lo - cfg_.cancel_span_ns;
        const auto& cancels = ctx.cancels;

        std::map<std::pair<ParticipantId, int>, std::vector<Entry>> groups;
        for (std::size_t i = cancels.lower_bound_ts(gather_lo); i < cancels.size(); ++i) {
            const Ts cts = cancels.ts_at(i);
            if (cts > anchor_hi) break;
            const CancelRec& c = cancels[i];
            if (c.ticks_from_touch < 0 || c.ticks_from_touch > cfg_.near_touch_ticks) continue;
            groups[{c.participant, static_cast<int>(c.side)}].push_back(Entry{cts, &c});
        }

        for (auto& [key, entries] : groups) {
            if (entries.size() < static_cast<std::size_t>(cfg_.min_orders)) continue;
            evaluate_group(ctx, key.first, static_cast<Side>(key.second), entries, anchor_lo,
                           anchor_hi, to, sink);
        }
    }

private:
    struct Entry {
        Ts ts;
        const CancelRec* rec;
    };

    void evaluate_group(const SymbolContext& ctx, ParticipantId pid, Side side,
                        std::vector<Entry>& entries, Ts anchor_lo, Ts anchor_hi, Ts detect_ts,
                        AlertSink& sink) {
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return a.ts < b.ts; });

        // Widest run of cancels fitting inside cancel_span, ending in the
        // slice we are responsible for. A two-pointer sweep; the cooldown in
        // the pipeline stops overlapping runs from each producing an alert.
        std::size_t best_lo = 0, best_hi = 0;
        std::size_t lo = 0;
        for (std::size_t hi = 0; hi < entries.size(); ++hi) {
            while (entries[hi].ts - entries[lo].ts > cfg_.cancel_span_ns) ++lo;
            if (entries[hi].ts <= anchor_lo || entries[hi].ts > anchor_hi) continue;
            if (hi - lo >= best_hi - best_lo) {
                best_lo = lo;
                best_hi = hi;
            }
        }
        const std::size_t count = best_hi - best_lo + 1;
        if (best_hi == best_lo && entries[best_lo].ts <= anchor_lo) return;
        if (count < static_cast<std::size_t>(cfg_.min_orders)) return;

        std::map<Price, Qty> by_level;
        Qty total_remaining = 0, total_original = 0, total_filled = 0;
        Qty base_depth = -1;
        Ts entry_first = entries[best_lo].rec->entry_ts;
        std::vector<OrderId> ids;
        for (std::size_t i = best_lo; i <= best_hi; ++i) {
            const CancelRec& c = *entries[i].rec;
            by_level[c.price] += c.remaining;
            total_remaining += c.remaining;
            total_original += c.original;
            total_filled += c.filled;
            entry_first = std::min(entry_first, c.entry_ts);
            if (base_depth < 0 || c.side_qty_at_entry < base_depth)
                base_depth = c.side_qty_at_entry;
            ids.push_back(c.order_id);
        }
        if (static_cast<int>(by_level.size()) < cfg_.min_levels) return;
        if (base_depth < 0) base_depth = 0;

        const Ts cluster_start = entries[best_lo].ts;
        const Ts cluster_end = entries[best_hi].ts;
        const Side benefit_side = opposite(side);

        Ts first_trade = 0, last_trade = 0;
        const Ts win_lo = cluster_start - cfg_.opposite_window_ns;
        const Ts win_hi = cluster_end + cfg_.opposite_window_ns;
        // Aggressive only, for the reason set out in spoofing.cpp: a market
        // maker withdrawing a multi-level quote and then being hit on the
        // other side is the single most common shape on any venue.
        const Qty opposite_qty = detail::directional_qty(
            ctx.trades, pid, benefit_side, win_lo, win_hi, detail::Role::Aggressor, &first_trade,
            &last_trade);
        if (opposite_qty <= 0) return;  // hard gate, same reasoning as spoofing
        const Qty passive_qty = detail::directional_qty(ctx.trades, pid, benefit_side, win_lo,
                                                        win_hi, detail::Role::Maker);

        Alert a;
        a.detector = DetectorKind::Layering;
        a.symbol = ctx.symbol;
        a.participant = pid;
        a.start_ts = std::min(entry_first, first_trade == 0 ? entry_first : first_trade);
        a.end_ts = std::max(cluster_end, last_trade);
        a.detect_ts = detect_ts;
        a.seq_first = entries[best_lo].rec->seq;
        a.seq_last = entries[best_hi].rec->seq;
        a.order_ids = std::move(ids);

        const double share =
            total_remaining + base_depth > 0
                ? static_cast<double>(total_remaining) / (total_remaining + base_depth)
                : 0.0;
        const Ts span = cluster_end - cluster_start;

        a.signal("levels", ratio01(static_cast<double>(by_level.size()), 2.0 * cfg_.min_levels),
                 cfg_.w_levels);
        a.signal("imbalance", ratio01(share, cfg_.imbalance_target), cfg_.w_imbalance);
        a.signal("coord",
                 cfg_.cancel_span_ns > 0
                     ? 1.0 - static_cast<double>(span) / static_cast<double>(cfg_.cancel_span_ns)
                     : 1.0,
                 cfg_.w_coord);
        a.signal("opposite",
                 ratio01(static_cast<double>(opposite_qty), static_cast<double>(total_remaining)),
                 cfg_.w_opposite);
        a.signal("unfilled",
                 total_original > 0
                     ? 1.0 - static_cast<double>(total_filled) / total_original
                     : 1.0,
                 cfg_.w_unfilled);

        const double two_sided = detail::two_sidedness(
            ctx.placements, pid, cluster_start - cfg_.placement_window_ns, cluster_end);
        const double penalty = cfg_.two_sided_penalty * two_sided;
        const double score = clamp01(a.weighted_score() - penalty);
        if (score < cfg_.min_score) return;

        a.score = score;
        a.penalty = penalty;
        a.note("orders_in_layer", static_cast<std::int64_t>(count));
        a.note("distinct_levels", static_cast<std::int64_t>(by_level.size()));
        a.note("layer_qty", total_remaining);
        a.note("filled_qty", total_filled);
        a.note("base_depth", base_depth);
        a.note("cancel_span_ms", span / kNsPerMs);
        a.note("opposite_qty", opposite_qty);
        a.note("opposite_passive_qty", passive_qty);
        a.note("two_sided_pct", static_cast<std::int64_t>(two_sided * 100.0 + 0.5));
        a.note("layer_side_is_buy", side == Side::Buy ? 1 : 0);
        sink.emit(std::move(a));
    }

    LayeringConfig cfg_;
    Price tick_;
};

}  // namespace

std::unique_ptr<IDetector> make_layering_detector(const LayeringConfig& cfg, Price tick_size) {
    return std::make_unique<LayeringDetector>(cfg, tick_size);
}

}  // namespace tapewatch
