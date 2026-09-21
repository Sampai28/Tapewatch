// Spoofing.
//
// The pattern: post a large order on one side of the book with no intention
// of trading it, let the display of size move other participants, pull it, and
// take liquidity on the *other* side at the price your own order helped make.
//
// The hard part is not recognising a large order that gets cancelled quickly.
// That is what a market maker does several hundred times an hour, legitimately
// -- quotes move because the fair price moved. Every venue's surveillance team
// has a pile of false positives shaped exactly like that.
//
// What separates the two is what happens next. A market maker who pulls a bid
// does not then immediately sell. A spoofer does, because the sell was the
// point and the bid was the advertisement. So the opposite-side execution is a
// hard gate here, not a weighted signal: no trade the other way inside the
// window, no alert, no matter how large or how fleeting the order was. The
// weights then rank what survives.
//
// A second defence: participants quoting both sides through the window have
// their score reduced, because symmetric quoting is the signature of market
// making and the asymmetry of spoofing is the point. The engine is never told
// who the registered market makers are -- that would make the headline
// false-positive number a measurement of a lookup table.

#include <algorithm>
#include <memory>

#include "common.hpp"
#include "tapewatch/detector.hpp"

namespace tapewatch {
namespace {

class SpoofingDetector final : public IDetector {
public:
    SpoofingDetector(const SpoofingConfig& cfg, Price tick_size) : cfg_(cfg), tick_(tick_size) {}

    DetectorKind kind() const override { return DetectorKind::Spoofing; }
    bool enabled() const override { return cfg_.enabled; }

    void scan(const SymbolContext& ctx, Ts from, Ts to, AlertSink& sink) override {
        // A cancel is only judgeable once the window on *both* sides of it has
        // gone by, so the slice examined lags the scan point by one window.
        const Ts w = cfg_.opposite_window_ns;
        const Ts anchor_lo = from - w;
        const Ts anchor_hi = to - w;
        if (anchor_hi <= anchor_lo) return;

        const auto& cancels = ctx.cancels;
        for (std::size_t i = cancels.lower_bound_ts(anchor_lo + 1); i < cancels.size(); ++i) {
            const Ts cts = cancels.ts_at(i);
            if (cts > anchor_hi) break;
            evaluate(ctx, cancels[i], cts, to, sink);
        }
    }

private:
    void evaluate(const SymbolContext& ctx, const CancelRec& c, Ts cts, Ts detect_ts,
                  AlertSink& sink) {
        if (c.remaining < cfg_.min_qty) return;
        // Distance is measured against the touch as it stood when the order
        // landed. An order five ticks off the touch is pressure; one fifty
        // ticks off is a parked resting order nobody was looking at.
        if (c.ticks_from_touch < 0 || c.ticks_from_touch > cfg_.near_touch_ticks) return;

        const Ts life = cts - c.entry_ts;
        if (life < 0) return;
        // Three window-widths of slack: the scoring curve handles the ranking,
        // the gate only exists to keep long-lived inventory orders out.
        if (life > cfg_.max_lifetime_ns * 3) return;

        const Side spoof_side = c.side;
        const Side benefit_side = opposite(spoof_side);

        Ts first_trade = 0, last_trade = 0;
        const Ts lo = cts - cfg_.opposite_window_ns;
        const Ts hi = cts + cfg_.opposite_window_ns;
        // Aggressive only. Being lifted on the other side after pulling a bid
        // is what happens to a market maker all day; going out and hitting
        // the other side is what a spoofer does. Counting both here was worth
        // several hundred false positives a session against market makers on
        // the shipped config, and essentially nothing in recall.
        const Qty opposite_qty =
            detail::directional_qty(ctx.trades, c.participant, benefit_side, lo, hi,
                                    detail::Role::Aggressor, &first_trade, &last_trade);
        if (opposite_qty <= 0) return;  // hard gate -- see the header comment
        const Qty passive_qty = detail::directional_qty(ctx.trades, c.participant, benefit_side,
                                                        lo, hi, detail::Role::Maker);

        Alert a;
        a.detector = DetectorKind::Spoofing;
        a.symbol = ctx.symbol;
        a.participant = c.participant;
        a.start_ts = std::min(c.entry_ts, first_trade == 0 ? c.entry_ts : first_trade);
        a.end_ts = std::max(cts, last_trade);
        a.detect_ts = detect_ts;
        a.seq_first = c.seq;
        a.seq_last = c.seq;
        a.order_ids.push_back(c.order_id);

        const double mean_qty =
            c.side_orders_at_entry > 0
                ? static_cast<double>(c.side_qty_at_entry) / c.side_orders_at_entry
                : static_cast<double>(c.original);
        const double size_multiple =
            mean_qty > 0.0 ? static_cast<double>(c.original) / mean_qty : 1.0;

        a.signal("size", ratio01(size_multiple, cfg_.size_depth_multiple), cfg_.w_size);
        a.signal("life",
                 cfg_.max_lifetime_ns > 0
                     ? 1.0 - static_cast<double>(life) / static_cast<double>(cfg_.max_lifetime_ns)
                     : 0.0,
                 cfg_.w_life);
        a.signal("unfilled",
                 c.original > 0 ? 1.0 - static_cast<double>(c.filled) / c.original : 1.0,
                 cfg_.w_unfilled);
        a.signal("opposite",
                 ratio01(static_cast<double>(opposite_qty), static_cast<double>(c.remaining)),
                 cfg_.w_opposite);
        a.signal("press",
                 cfg_.near_touch_ticks > 0
                     ? 1.0 - static_cast<double>(c.ticks_from_touch) / cfg_.near_touch_ticks
                     : 1.0,
                 cfg_.w_press);

        const double two_sided = detail::two_sidedness(ctx.placements, c.participant,
                                                       cts - cfg_.opposite_window_ns, cts);
        const double penalty = cfg_.two_sided_penalty * two_sided;
        const double score = clamp01(a.weighted_score() - penalty);
        if (score < cfg_.min_score) return;

        a.score = score;
        a.penalty = penalty;
        a.note("cancelled_qty", c.remaining);
        a.note("order_qty", c.original);
        a.note("filled_qty", c.filled);
        a.note("lifetime_ms", life / kNsPerMs);
        a.note("opposite_qty", opposite_qty);
        a.note("opposite_passive_qty", passive_qty);
        a.note("ticks_from_touch", c.ticks_from_touch);
        a.note("side_depth_at_entry", c.side_qty_at_entry);
        a.note("two_sided_pct", static_cast<std::int64_t>(two_sided * 100.0 + 0.5));
        a.note("spoof_side_is_buy", spoof_side == Side::Buy ? 1 : 0);
        sink.emit(std::move(a));
    }

    SpoofingConfig cfg_;
    Price tick_;
};

}  // namespace

std::unique_ptr<IDetector> make_spoofing_detector(const SpoofingConfig& cfg, Price tick_size) {
    return std::make_unique<SpoofingDetector>(cfg, tick_size);
}

}  // namespace tapewatch
