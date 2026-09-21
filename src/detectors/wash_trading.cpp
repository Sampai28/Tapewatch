// Wash trading.
//
// Trading with yourself. The point is the print: volume that looks like
// interest, a price that looks like a market, with no change in who owns what.
// It comes in three shapes, and this detector handles all three because a
// system that only catches the first one catches nothing real:
//
//   direct     the same participant on both sides of the print. Trivial to
//              spot, trivial to avoid, and therefore rare in practice.
//   matched    two accounts trading back and forth, A selling to B and B
//              selling to A in near-equal size. The reciprocity is the tell.
//   circular   the same through a chain. Not detected here -- see the
//              limitation in docs/detectors.md rather than a hint of support
//              that does not exist.
//
// The confounder is a market maker, and it is a serious one. Over any window
// a market maker has large gross volume, near-zero net position, and trades
// both ways with the same active counterparties. Reciprocity, flatness and
// repetition -- three of the five signals here -- describe a market maker
// perfectly, and no amount of tuning those three will separate the two.
//
// What does separate them is *concentration*, and it takes two forms, so this
// detector uses both:
//
//   share    how much of the busier party's volume went through this single
//            counterparty. A wash pair is near all of it. A market maker's
//            largest single relationship is a small slice of its book.
//   breadth  how many distinct counterparties the busier party traded with at
//            all. This is subtracted from the score rather than added to it,
//            the same shape as the two-sided quoting defence in the spoofing
//            detector, and for the same reason: market making is inferred
//            from behaviour, never from a roster the engine was handed.
//
// This detector runs on a timer rather than per-print. Pair statistics over a
// thirty-second window recomputed on every trade would be quadratic in the
// message rate; recomputed once per scan interval they are linear.

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "common.hpp"
#include "tapewatch/detector.hpp"

namespace tapewatch {
namespace {

struct PairAgg {
    Qty a_to_b{0};  // quantity where the lower-numbered id was the buyer
    Qty b_to_a{0};
    Qty gross{0};
    std::uint32_t prints{0};
    std::uint32_t offmarket_prints{0};
    Ts first_ts{0};
    Ts last_ts{0};
    SeqNum first_seq{0};
    SeqNum last_seq{0};
};

struct PidAgg {
    Qty gross{0};
    Qty net{0};  // signed: buys positive
    Qty self{0};
    std::uint32_t self_prints{0};
    std::set<ParticipantId> counterparties;
    Ts first_ts{0};
    Ts last_ts{0};
    SeqNum first_seq{0};
    SeqNum last_seq{0};
};

class WashDetector final : public IDetector {
public:
    WashDetector(const WashConfig& cfg, Price tick_size) : cfg_(cfg), tick_(tick_size) {}

    DetectorKind kind() const override { return DetectorKind::WashTrading; }
    bool enabled() const override { return cfg_.enabled; }

    void scan(const SymbolContext& ctx, Ts from, Ts to, AlertSink& sink) override {
        const auto& trades = ctx.trades;
        if (trades.empty()) return;
        // Nothing new printed, nothing to say.
        if (trades.back_ts() <= from) return;

        const Ts lo = to - cfg_.window_ns;
        std::map<std::pair<ParticipantId, ParticipantId>, PairAgg> pairs;
        std::unordered_map<ParticipantId, PidAgg> pids;
        Qty symbol_volume = 0;

        for (std::size_t i = trades.lower_bound_ts(lo); i < trades.size(); ++i) {
            const Ts ts = trades.ts_at(i);
            if (ts > to) break;
            const TradeRec& t = trades[i];
            symbol_volume += t.qty;

            const bool offmarket = is_offmarket(t);

            if (t.taker == t.maker) {
                PidAgg& p = pids[t.taker];
                p.self += t.qty;
                p.self_prints += 1;
                p.gross += t.qty;
                // A self-trade moves no position, so `net` is untouched.
                stamp(p, ts, t.seq);
                continue;
            }

            for (int which = 0; which < 2; ++which) {
                const ParticipantId pid = which == 0 ? t.taker : t.maker;
                const ParticipantId other = which == 0 ? t.maker : t.taker;
                const Side s = which == 0 ? t.aggressor : opposite(t.aggressor);
                PidAgg& p = pids[pid];
                p.gross += t.qty;
                p.net += (s == Side::Buy ? t.qty : -t.qty);
                p.counterparties.insert(other);
                stamp(p, ts, t.seq);
            }

            const ParticipantId lo_id = std::min(t.taker, t.maker);
            const ParticipantId hi_id = std::max(t.taker, t.maker);
            PairAgg& pa = pairs[{lo_id, hi_id}];
            const Side taker_side = t.aggressor;
            const ParticipantId buyer = taker_side == Side::Buy ? t.taker : t.maker;
            if (buyer == lo_id) pa.a_to_b += t.qty;
            else pa.b_to_a += t.qty;
            pa.gross += t.qty;
            pa.prints += 1;
            if (offmarket) pa.offmarket_prints += 1;
            if (pa.first_ts == 0) {
                pa.first_ts = ts;
                pa.first_seq = t.seq;
            }
            pa.last_ts = ts;
            pa.last_seq = t.seq;
        }

        if (symbol_volume <= 0) return;

        for (const auto& [pid, agg] : pids) {
            if (agg.self <= 0) continue;
            if (agg.gross < cfg_.min_gross_qty) continue;
            emit_self(ctx, pid, agg, symbol_volume, to, sink);
        }

        for (const auto& [key, agg] : pairs) {
            const Qty lo_q = std::min(agg.a_to_b, agg.b_to_a);
            if (lo_q <= 0) continue;  // one-directional flow is not a wash
            if (agg.gross < cfg_.min_gross_qty) continue;
            emit_pair(ctx, key.first, key.second, agg, pids, symbol_volume, to, sink);
        }
    }

private:
    static void stamp(PidAgg& p, Ts ts, SeqNum seq) {
        if (p.first_ts == 0) {
            p.first_ts = ts;
            p.first_seq = seq;
        }
        p.last_ts = ts;
        p.last_seq = seq;
    }

    bool is_offmarket(const TradeRec& t) const {
        const int d = detail::ticks_from_mid(t.mid2_before, t.price, tick_);
        return d >= 0 && d > cfg_.offmarket_ticks;
    }

    void emit_self(const SymbolContext& ctx, ParticipantId pid, const PidAgg& agg,
                   Qty symbol_volume, Ts detect_ts, AlertSink& sink) {
        Alert a;
        a.detector = DetectorKind::WashTrading;
        a.symbol = ctx.symbol;
        a.participant = pid;
        a.counterparty = pid;
        a.has_counterparty = true;
        a.start_ts = agg.first_ts;
        a.end_ts = agg.last_ts;
        a.detect_ts = detect_ts;
        a.seq_first = agg.first_seq;
        a.seq_last = agg.last_seq;

        const double self_frac =
            agg.gross > 0 ? static_cast<double>(agg.self) / agg.gross : 0.0;
        a.signal("self", self_frac, cfg_.w_self);
        // A participant on both sides of a print is by construction perfectly
        // reciprocal with itself; stating it rather than leaving the signal at
        // zero keeps the weight vector comparable across the two shapes.
        a.signal("recip", 1.0, cfg_.w_recip);
        a.signal("nonet",
                 agg.gross > 0
                     ? 1.0 - static_cast<double>(std::llabs(agg.net)) / agg.gross
                     : 0.0,
                 cfg_.w_nonet);
        a.signal("share",
                 ratio01(static_cast<double>(agg.self) / symbol_volume, cfg_.share_target),
                 cfg_.w_share);
        a.signal("price", 0.0, cfg_.w_price);

        const double score = a.weighted_score();
        if (score < cfg_.min_score) return;
        a.score = score;
        a.note("self_qty", agg.self);
        a.note("self_prints", agg.self_prints);
        a.note("gross_qty", agg.gross);
        a.note("net_qty", agg.net);
        a.note("symbol_volume", symbol_volume);
        a.note("is_self_trade", 1);
        sink.emit(std::move(a));
    }

    void emit_pair(const SymbolContext& ctx, ParticipantId lo_id, ParticipantId hi_id,
                   const PairAgg& agg, const std::unordered_map<ParticipantId, PidAgg>& pids,
                   Qty symbol_volume, Ts detect_ts, AlertSink& sink) {
        const Qty lo_q = std::min(agg.a_to_b, agg.b_to_a);
        const Qty hi_q = std::max(agg.a_to_b, agg.b_to_a);
        const double recip = hi_q > 0 ? static_cast<double>(lo_q) / hi_q : 0.0;

        Alert a;
        a.detector = DetectorKind::WashTrading;
        a.symbol = ctx.symbol;
        a.participant = lo_id;
        a.counterparty = hi_id;
        a.has_counterparty = true;
        a.start_ts = agg.first_ts;
        a.end_ts = agg.last_ts;
        a.detect_ts = detect_ts;
        a.seq_first = agg.first_seq;
        a.seq_last = agg.last_seq;

        // The pair's combined net exposure change. Two accounts passing the
        // same position back and forth net to nothing between them even when
        // each one's individual net is large.
        Qty pair_net = 0, pair_gross = 0, busier_gross = 0;
        std::size_t breadth = 0;
        for (ParticipantId p : {lo_id, hi_id}) {
            auto it = pids.find(p);
            if (it == pids.end()) continue;
            pair_net += it->second.net;
            pair_gross += it->second.gross;
            busier_gross = std::max(busier_gross, it->second.gross);
            breadth = std::max(breadth, it->second.counterparties.size());
        }

        a.signal("self", 0.0, cfg_.w_self);
        a.signal("recip", ratio01(recip, cfg_.recip_target), cfg_.w_recip);
        a.signal("nonet",
                 pair_gross > 0
                     ? 1.0 - static_cast<double>(std::llabs(pair_net)) / pair_gross
                     : 0.0,
                 cfg_.w_nonet);
        a.signal("share",
                 busier_gross > 0
                     ? ratio01(static_cast<double>(agg.gross) / busier_gross, cfg_.share_target)
                     : 0.0,
                 cfg_.w_share);
        a.signal("price",
                 agg.prints > 0
                     ? static_cast<double>(agg.offmarket_prints) / agg.prints
                     : 0.0,
                 cfg_.w_price);

        const double spread_out =
            breadth > 1 ? ratio01(static_cast<double>(breadth - 1), cfg_.breadth_target) : 0.0;
        const double penalty = cfg_.breadth_penalty * spread_out;
        const double score = clamp01(a.weighted_score() - penalty);
        if (score < cfg_.min_score) return;
        a.score = score;
        a.penalty = penalty;
        a.note("pair_gross_qty", agg.gross);
        a.note("qty_lo_to_hi", agg.a_to_b);
        a.note("qty_hi_to_lo", agg.b_to_a);
        a.note("prints", agg.prints);
        a.note("offmarket_prints", agg.offmarket_prints);
        a.note("pair_net_qty", pair_net);
        a.note("busier_gross_qty", busier_gross);
        a.note("counterparty_breadth", static_cast<std::int64_t>(breadth));
        a.note("symbol_volume", symbol_volume);
        a.note("is_self_trade", 0);
        sink.emit(std::move(a));
    }

    WashConfig cfg_;
    Price tick_;
};

}  // namespace

std::unique_ptr<IDetector> make_wash_detector(const WashConfig& cfg, Price tick_size) {
    return std::make_unique<WashDetector>(cfg, tick_size);
}

}  // namespace tapewatch
