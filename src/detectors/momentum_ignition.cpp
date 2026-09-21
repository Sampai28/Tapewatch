// Momentum ignition.
//
// Buy aggressively enough, fast enough, that momentum traders and stop orders
// follow you; then sell into the move you started. The manipulation is not the
// buying -- that is just trading -- it is the buying *in order to* sell.
//
// Which means intent has to be inferred from what happens afterwards, and that
// has a consequence this detector cannot design around: at the moment of the
// burst, it is indistinguishable from an informed trader who has decided the
// price is wrong and is taking the offer until it is right. The two look
// identical for several seconds. They differ in the two things that follow:
//
//   the informed trader keeps the position, and the price stays moved
//   the igniter reverses out, and the price comes back
//
// So the detector deliberately waits. It evaluates a burst only once
// `revert_ms` of tape has passed since the burst ended, because before then
// the answer does not exist yet. The cost is detection latency of roughly
// burst_ms + revert_ms, which for the shipped config is about ten seconds,
// and that latency is in the measured numbers rather than argued away.
// Compare with spoofing, where the discriminating evidence arrives within a
// window of the cancel and detection is correspondingly quicker.
//
// The reversal is a hard gate. No reverse trade, no alert -- an informed
// trader who was simply right does not generate one.

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <set>
#include <unordered_map>

#include "common.hpp"
#include "tapewatch/detector.hpp"

namespace tapewatch {
namespace {

struct BurstAgg {
    Qty qty[2]{0, 0};  // aggressive quantity by direction
    std::uint32_t fills[2]{0, 0};
    // Distinct marketable orders, which is what "a burst" means. One order
    // sweeping five levels is five fills and one decision.
    std::set<OrderId> orders[2];
    Ts first_ts{0};
    Ts last_ts{0};
    SeqNum first_seq{0};
    SeqNum last_seq{0};
};

class MomentumDetector final : public IDetector {
public:
    MomentumDetector(const MomentumConfig& cfg, Price tick_size) : cfg_(cfg), tick_(tick_size) {}

    DetectorKind kind() const override { return DetectorKind::MomentumIgnition; }
    bool enabled() const override { return cfg_.enabled; }

    void scan(const SymbolContext& ctx, Ts from, Ts to, AlertSink& sink) override {
        (void)from;
        const Ts burst_end = to - cfg_.revert_ns;
        const Ts burst_start = burst_end - cfg_.burst_ns;
        if (burst_start <= 0) return;

        const auto& trades = ctx.trades;
        if (trades.empty() || trades.front_ts() > burst_start) return;

        std::unordered_map<ParticipantId, BurstAgg> aggr;
        Qty total_aggressive = 0;
        for (std::size_t i = trades.lower_bound_ts(burst_start); i < trades.size(); ++i) {
            const Ts ts = trades.ts_at(i);
            if (ts > burst_end) break;
            const TradeRec& t = trades[i];
            // Only the aggressor is igniting anything. The resting side got
            // hit; they did not choose the moment.
            BurstAgg& b = aggr[t.taker];
            const int d = static_cast<int>(t.aggressor);
            b.qty[d] += t.qty;
            b.fills[d] += 1;
            b.orders[d].insert(t.taker_order);
            if (b.first_ts == 0) {
                b.first_ts = ts;
                b.first_seq = t.seq;
            }
            b.last_ts = ts;
            b.last_seq = t.seq;
            total_aggressive += t.qty;
        }
        if (total_aggressive <= 0) return;

        const Price mid_pre = ctx.mid2_at(burst_start);
        const Price mid_post = ctx.mid2_at(burst_end);
        const Price mid_now = ctx.mid2_at(to);
        if (mid_pre == kNoPrice || mid_post == kNoPrice) return;

        const Price move2 = mid_post - mid_pre;
        const int move_ticks = static_cast<int>(std::llabs(move2) / (2 * tick_));
        if (move_ticks < cfg_.min_move_ticks) return;
        const Side move_dir = move2 > 0 ? Side::Buy : Side::Sell;

        for (const auto& [pid, b] : aggr) {
            const int d = static_cast<int>(move_dir);
            if (static_cast<int>(b.orders[d].size()) < cfg_.min_orders) continue;
            // The burst has to be in the direction the price went. A
            // participant who sold into a rally did not cause it.
            if (b.qty[d] <= b.qty[1 - d]) continue;
            // And it has to be their move. Being one of thirty people buying
            // is not igniting anything.
            if (static_cast<double>(b.qty[d]) / total_aggressive < cfg_.min_share) continue;
            evaluate(ctx, pid, b, move_dir, move_ticks, move2, mid_post, mid_now, total_aggressive,
                     burst_start, burst_end, to, sink);
        }
    }

private:
    void evaluate(const SymbolContext& ctx, ParticipantId pid, const BurstAgg& b, Side move_dir,
                  int move_ticks, Price move2, Price mid_post, Price mid_now, Qty total_aggressive,
                  Ts burst_start, Ts burst_end, Ts detect_ts, AlertSink& sink) {
        const int d = static_cast<int>(move_dir);
        const Qty burst_qty = b.qty[d];

        Ts rev_first = 0, rev_last = 0;
        const Qty reverse_qty = detail::directional_qty(
            ctx.trades, pid, opposite(move_dir), burst_end, burst_end + cfg_.reversal_ns,
            detail::Role::Any, &rev_first, &rev_last);
        // A fraction, not a non-zero. Anybody who trades in both directions
        // occasionally has some opposite-side volume after any burst; what
        // distinguishes an igniter is getting most of the position back off.
        if (burst_qty <= 0) return;
        if (static_cast<double>(reverse_qty) / burst_qty < cfg_.min_reverse_fraction) return;

        // How much of the move gave itself back. Unknown when the book went
        // one-sided after the burst, in which case the signal is zero rather
        // than a guess.
        double revert = 0.0;
        if (mid_now != kNoPrice) {
            const Price given_back = move2 > 0 ? (mid_post - mid_now) : (mid_now - mid_post);
            if (given_back > 0)
                revert = ratio01(static_cast<double>(given_back),
                                 static_cast<double>(std::llabs(move2)));
        }

        Alert a;
        a.detector = DetectorKind::MomentumIgnition;
        a.symbol = ctx.symbol;
        a.participant = pid;
        a.start_ts = b.first_ts;
        a.end_ts = std::max(rev_last, burst_end);
        a.detect_ts = detect_ts;
        a.seq_first = b.first_seq;
        a.seq_last = b.last_seq;

        a.signal("burst",
                 ratio01(static_cast<double>(b.orders[d].size()), cfg_.burst_target_orders),
                 cfg_.w_burst);
        a.signal("move", ratio01(move_ticks, cfg_.move_target_ticks), cfg_.w_move);
        a.signal("share",
                 ratio01(static_cast<double>(burst_qty) / total_aggressive, cfg_.share_target),
                 cfg_.w_share);
        a.signal("reverse",
                 ratio01(static_cast<double>(reverse_qty), static_cast<double>(burst_qty)),
                 cfg_.w_reverse);
        a.signal("revert", revert, cfg_.w_revert);

        const double score = a.weighted_score();
        if (score < cfg_.min_score) return;
        a.score = score;
        a.note("burst_qty", burst_qty);
        a.note("burst_orders", static_cast<std::int64_t>(b.orders[d].size()));
        a.note("burst_fills", b.fills[d]);
        a.note("total_aggressive_qty", total_aggressive);
        a.note("move_ticks", move_ticks);
        a.note("reverse_qty", reverse_qty);
        a.note("revert_pct", static_cast<std::int64_t>(revert * 100.0 + 0.5));
        a.note("burst_start_ms", burst_start / kNsPerMs);
        a.note("burst_dir_is_buy", move_dir == Side::Buy ? 1 : 0);
        sink.emit(std::move(a));
    }

    MomentumConfig cfg_;
    Price tick_;
};

}  // namespace

std::unique_ptr<IDetector> make_momentum_detector(const MomentumConfig& cfg, Price tick_size) {
    return std::make_unique<MomentumDetector>(cfg, tick_size);
}

}  // namespace tapewatch
