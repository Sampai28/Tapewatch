#include "tapewatch/pipeline.hpp"

#include <algorithm>

namespace tapewatch {
namespace {

Ts required_history(const EngineConfig& c) {
    const Ts spoof = c.spoofing.opposite_window_ns * 2 + c.spoofing.max_lifetime_ns * 3;
    const Ts layer = c.layering.opposite_window_ns * 2 + c.layering.cancel_span_ns +
                     c.layering.placement_window_ns;
    const Ts wash = c.wash.window_ns;
    const Ts mom = c.momentum.revert_ns + c.momentum.burst_ns + c.momentum.reversal_ns;
    return std::max({spoof, layer, wash, mom}) + 2 * c.scan_interval_ns;
}

}  // namespace

// The sink exists so a detector can emit without knowing about cooldowns,
// alert ids, feed damage or output formats.
class Pipeline::Sink final : public AlertSink {
public:
    Sink(Pipeline& p) : p_(p) {}
    void emit(Alert&& a) override {
        const int d = static_cast<int>(a.detector);
        if (p_.cooldown_blocks(a)) {
            ++p_.stats_.alerts_suppressed_cooldown;
            ++p_.stats_.suppressed_by_detector[d];
            return;
        }
        a.alert_id = p_.next_alert_id_++;
        a.degraded = p_.overlaps_damage(a);
        a.confidence = a.degraded ? a.score * p_.cfg_.degrade_factor : a.score;
        if (a.degraded) ++p_.stats_.alerts_degraded;
        ++p_.stats_.alerts_emitted;
        ++p_.stats_.alerts_by_detector[d];
        p_.out_.write(a);
    }

private:
    Pipeline& p_;
};

Pipeline::Pipeline(const EngineConfig& cfg, AlertWriter& out)
    : cfg_(cfg),
      out_(out),
      history_ns_(required_history(cfg)),
      reorder_(cfg.reorder_tolerance_ns, cfg.window_capacity),
      guard_(cfg.feed_degrade_ns, cfg.window_capacity * 4) {
    if (cfg_.spoofing.enabled)
        detectors_.push_back(make_spoofing_detector(cfg_.spoofing, cfg_.tick_size));
    if (cfg_.layering.enabled)
        detectors_.push_back(make_layering_detector(cfg_.layering, cfg_.tick_size));
    if (cfg_.wash.enabled) detectors_.push_back(make_wash_detector(cfg_.wash, cfg_.tick_size));
    if (cfg_.momentum.enabled)
        detectors_.push_back(make_momentum_detector(cfg_.momentum, cfg_.tick_size));
}

Pipeline::~Pipeline() = default;

SymbolContext& Pipeline::context_for(const std::string& symbol) {
    auto it = symbols_.find(symbol);
    if (it != symbols_.end()) return *it->second;
    auto ctx = std::make_unique<SymbolContext>(symbol, cfg_.max_resting_orders, cfg_.tick_size,
                                               cfg_.window_capacity, history_ns_);
    SymbolContext& ref = *ctx;
    symbols_.emplace(symbol, std::move(ctx));
    ++stats_.symbols;
    return ref;
}

void Pipeline::feed_line(std::string_view line) {
    ++stats_.feed.lines_read;
    MarketEvent e;
    const ParseError err = parse_event(line, e);
    if (err != ParseError::None) {
        if (err != ParseError::Empty) {
            ++stats_.feed.parse_failures;
            ++stats_.feed.parse_error_kind[static_cast<int>(err)];
        }
        return;
    }
    if (!reorder_.push(e)) {
        ++stats_.feed.reordered_dropped;
        return;
    }
    ready_.clear();
    reorder_.drain_ready(ready_);
    for (const MarketEvent& r : ready_) apply(r);
}

void Pipeline::finish() {
    ready_.clear();
    reorder_.flush(ready_);
    for (const MarketEvent& r : ready_) apply(r);
    stats_.feed.reordered_recovered = reorder_.recovered();
    // One last pass far enough ahead that every deferred evaluation window
    // closes. Without it, anything in the final revert_ms of the tape is never
    // judged, which silently costs recall at the end of every run.
    advance_scans(now_ + history_ns_, true);

    for (const auto& [sym, ctx] : symbols_) {
        const BookStats& b = ctx->book.stats();
        stats_.book.orders_added += b.orders_added;
        stats_.book.orders_dropped_cap += b.orders_dropped_cap;
        stats_.book.unknown_cancel += b.unknown_cancel;
        stats_.book.unknown_modify += b.unknown_modify;
        stats_.book.unknown_maker += b.unknown_maker;
        stats_.book.duplicate_order_id += b.duplicate_order_id;
        stats_.book.crossed_observations += b.crossed_observations;
        stats_.book.oversized_fill += b.oversized_fill;
        stats_.book.stale_levels_removed += b.stale_levels_removed;
        stats_.book.stale_orders_removed += b.stale_orders_removed;
        stats_.window_overflows += ctx->trades.overflowed() + ctx->placements.overflowed() +
                                   ctx->cancels.overflowed() + ctx->mids.overflowed();
    }
}

void Pipeline::apply(const MarketEvent& e) {
    const Ts before_damage = guard_.degraded_until();
    if (guard_.admit(e, stats_.feed) != FeedGuard::Verdict::Accept) return;
    if (guard_.degraded_until() != before_damage) {
        damage_.push_back(e.ts);
        while (!damage_.empty() && damage_.front() < e.ts - history_ns_ - cfg_.feed_degrade_ns)
            damage_.pop_front();
    }

    if (!started_) {
        started_ = true;
        stats_.first_ts = e.ts;
        next_scan_ = e.ts + cfg_.scan_interval_ns;
    }
    stats_.last_ts = e.ts;

    // Scans happen before the event is applied, so a scan at time T sees the
    // book exactly as it was at T rather than one event into the future.
    if (e.ts >= next_scan_) advance_scans(e.ts, false);
    now_ = e.ts;

    SymbolContext& ctx = context_for(e.symbol);
    ctx.now = e.ts;
    ctx.seq = e.seq;
    ctx.degraded = guard_.degraded(e.ts);

    switch (e.kind) {
        case EventKind::Order: {
            if (!ctx.book.on_order(e)) break;
            const RestingOrder* r = ctx.book.find(e.order_id);
            PlacementRec p;
            p.seq = e.seq;
            p.participant = e.participant;
            p.order_id = e.order_id;
            p.side = e.side;
            p.price = e.price;
            p.qty = e.quantity;
            p.ticks_from_touch = r ? r->ticks_from_touch_at_entry : -1;
            ctx.placements.push(e.ts, p);
            break;
        }
        case EventKind::Modify: {
            RestingOrder before{}, after{};
            if (!ctx.book.on_modify(e, before, after)) break;
            // A modify that moves price is a fresh placement for surveillance
            // purposes: it re-advertises size at a new level.
            if (after.remaining > 0 && after.price != before.price) {
                PlacementRec p;
                p.seq = e.seq;
                p.participant = after.participant;
                p.order_id = after.id;
                p.side = after.side;
                p.price = after.price;
                p.qty = after.remaining;
                p.ticks_from_touch = ctx.book.ticks_from_touch(after.side, after.price,
                                                               cfg_.tick_size);
                ctx.placements.push(e.ts, p);
            }
            // An amend to zero is a withdrawal and is recorded as one, so a
            // participant cannot dodge the cancel-based detectors by amending
            // their layer away instead of cancelling it.
            if (after.remaining <= 0) push_cancel(ctx, e, before);
            break;
        }
        case EventKind::Cancel: {
            RestingOrder removed{};
            if (!ctx.book.on_cancel(e, removed)) break;
            push_cancel(ctx, e, removed);
            break;
        }
        case EventKind::Trade: {
            TradeRec t;
            t.seq = e.seq;
            t.taker = e.participant;
            t.maker = e.maker_participant;
            t.taker_order = e.order_id;
            t.maker_order = e.maker_order_id;
            t.aggressor = e.side;
            t.price = e.price;
            t.qty = e.quantity;
            t.mid2_before = ctx.book.mid2();
            ctx.trades.push(e.ts, t);

            RestingOrder maker{};
            bool gone = false;
            ctx.book.on_trade(e, maker, gone);
            break;
        }
    }

    // A crossed reconstruction is always our problem, never the venue's.
    // Repairing here rather than tolerating it keeps every depth and
    // distance-from-touch measurement downstream meaningful; the repair
    // counts are reported so a run over a badly damaged feed is visible.
    if (ctx.book.crossed()) ctx.book.repair_cross();

    ctx.mids.push(e.ts, MidRec{ctx.book.mid2()});
}

void Pipeline::push_cancel(SymbolContext& ctx, const MarketEvent& e, const RestingOrder& r) {
    CancelRec c;
    c.seq = e.seq;
    c.participant = r.participant;
    c.order_id = r.id;
    c.side = r.side;
    c.price = r.price;
    c.remaining = r.remaining;
    c.original = r.original_qty;
    c.filled = r.filled;
    c.entry_ts = r.entry_ts;
    c.ticks_from_touch = r.ticks_from_touch_at_entry;
    c.side_qty_at_entry = r.side_qty_at_entry;
    c.side_orders_at_entry = r.side_orders_at_entry;
    ctx.cancels.push(e.ts, c);
}

void Pipeline::advance_scans(Ts now, bool final_pass) {
    if (!started_) return;
    while (next_scan_ <= now) {
        const Ts from = next_scan_ - cfg_.scan_interval_ns;
        for (auto& [sym, ctx] : symbols_) run_scan(*ctx, from, next_scan_);
        ++stats_.scans;
        next_scan_ += cfg_.scan_interval_ns;
    }
    if (final_pass) {
        // Nothing left to do: the loop above already walked past the end of
        // every deferred window because `now` was pushed out by history_ns.
    }
}

void Pipeline::run_scan(SymbolContext& ctx, Ts from, Ts to) {
    // Windows evict lazily, on push. A quiet symbol would otherwise present a
    // detector with data from well outside its window.
    ctx.trades.expire(to);
    ctx.placements.expire(to);
    ctx.cancels.expire(to);
    ctx.mids.expire(to);

    Sink sink(*this);
    for (auto& d : detectors_) {
        if (!d->enabled()) continue;
        d->scan(ctx, from, to, sink);
    }
}

bool Pipeline::cooldown_blocks(const Alert& a) {
    Ts cooldown = 0;
    switch (a.detector) {
        case DetectorKind::Spoofing: cooldown = cfg_.spoofing.cooldown_ns; break;
        case DetectorKind::Layering: cooldown = cfg_.layering.cooldown_ns; break;
        case DetectorKind::WashTrading: cooldown = cfg_.wash.cooldown_ns; break;
        case DetectorKind::MomentumIgnition: cooldown = cfg_.momentum.cooldown_ns; break;
        default: break;
    }
    if (cooldown <= 0) return false;

    CooldownKey k{static_cast<int>(a.detector), a.participant,
                  a.has_counterparty ? a.counterparty : 0, a.symbol};
    auto it = cooldown_.find(k);
    if (it != cooldown_.end() && a.detect_ts - it->second < cooldown) return true;
    cooldown_[k] = a.detect_ts;
    return false;
}

bool Pipeline::overlaps_damage(const Alert& a) const {
    const Ts lo = a.start_ts - cfg_.feed_degrade_ns;
    const Ts hi = a.end_ts + cfg_.feed_degrade_ns;
    // damage_ is timestamp-ordered and short; a scan beats an index here.
    for (Ts d : damage_)
        if (d >= lo && d <= hi) return true;
    return false;
}

}  // namespace tapewatch
