// The detector interface and the rolling state every detector reads.
//
// All four detectors are *scanners*, not event handlers. The pipeline advances
// the book event by event and then, every `scan_interval_ms` of tape time,
// asks each detector to look at the slice of history that just became
// available. Three reasons:
//
//   1. Cost. An event handler that walks a thirty-second trade window on every
//      trade is quadratic in the message rate. A scanner walks it once per
//      interval regardless of rate, so the engine's cost is bounded by the
//      window size and the interval, not by how busy the market is.
//   2. Some of these patterns are only visible after the fact. Momentum
//      ignition is defined partly by what the price does *after* the burst;
//      there is no event at which the answer is known. The scanner evaluates
//      bursts that are old enough to have an answer.
//   3. It is what real surveillance does. Nobody runs a wash-trade test on
//      every print.
//
// The cost of the choice is that detection latency has a floor of one scan
// interval. That floor is real, it is in the measured latency numbers, and it
// is not hidden.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tapewatch/alert.hpp"
#include "tapewatch/book_state.hpp"
#include "tapewatch/config.hpp"
#include "tapewatch/event.hpp"
#include "tapewatch/ring.hpp"
#include "tapewatch/types.hpp"

namespace tapewatch {

struct TradeRec {
    SeqNum seq{0};
    ParticipantId taker{0};
    ParticipantId maker{0};
    OrderId taker_order{0};
    OrderId maker_order{0};
    Side aggressor{Side::Buy};
    Price price{0};
    Qty qty{0};
    Price mid2_before{kNoPrice};
};

struct PlacementRec {
    SeqNum seq{0};
    ParticipantId participant{0};
    OrderId order_id{0};
    Side side{Side::Buy};
    Price price{0};
    Qty qty{0};
    int ticks_from_touch{-1};
};

struct CancelRec {
    SeqNum seq{0};
    ParticipantId participant{0};
    OrderId order_id{0};
    Side side{Side::Buy};
    Price price{0};
    Qty remaining{0};
    Qty original{0};
    Qty filled{0};
    Ts entry_ts{0};
    // Distance from the touch measured when the order was *placed*, not when
    // it was pulled -- an order that was aggressive on arrival is what matters,
    // and by cancel time the touch has usually moved.
    int ticks_from_touch{-1};
    Qty side_qty_at_entry{0};
    std::uint32_t side_orders_at_entry{0};
};

struct MidRec {
    Price mid2{kNoPrice};
};

// Everything a detector may read. One instance per symbol, owned by the
// pipeline, handed to every detector by const reference during a scan.
struct SymbolContext {
    std::string symbol;
    BookState book;
    TimeWindow<TradeRec> trades;
    TimeWindow<PlacementRec> placements;
    TimeWindow<CancelRec> cancels;
    TimeWindow<MidRec> mids;

    Ts now{0};
    SeqNum seq{0};
    // Set while the feed is within `feed_degrade_ns` of a gap, a duplicate
    // storm or a clock anomaly. Alerts raised in this state are marked and
    // their confidence is cut.
    bool degraded{false};
    Ts degraded_until{0};

    SymbolContext(std::string sym, std::size_t max_orders, Price tick_size, std::size_t cap,
                  Ts history_ns)
        : symbol(std::move(sym)),
          book(max_orders, tick_size),
          trades(cap, history_ns),
          placements(cap, history_ns),
          cancels(cap, history_ns),
          mids(cap, history_ns) {}

    // Mid at or immediately before `t`, in mid2 units. kNoPrice when the
    // window holds nothing that old.
    Price mid2_at(Ts t) const {
        if (mids.empty()) return kNoPrice;
        std::size_t i = mids.lower_bound_ts(t + 1);
        if (i == 0) return kNoPrice;
        return mids[i - 1].mid2;
    }
};

class AlertSink {
public:
    virtual ~AlertSink() = default;
    virtual void emit(Alert&& a) = 0;
};

class IDetector {
public:
    virtual ~IDetector() = default;
    virtual DetectorKind kind() const = 0;
    virtual bool enabled() const = 0;
    // Examine the tape. `from` is exclusive, `to` inclusive; a detector is
    // responsible for not re-alerting on behaviour it already reported, which
    // in practice means anchoring each candidate on an event whose timestamp
    // falls in (from, to].
    virtual void scan(const SymbolContext& ctx, Ts from, Ts to, AlertSink& sink) = 0;
};

// Factories. Each detector owns a copy of its config slice.
std::unique_ptr<IDetector> make_spoofing_detector(const SpoofingConfig& cfg, Price tick_size);
std::unique_ptr<IDetector> make_layering_detector(const LayeringConfig& cfg, Price tick_size);
std::unique_ptr<IDetector> make_wash_detector(const WashConfig& cfg, Price tick_size);
std::unique_ptr<IDetector> make_momentum_detector(const MomentumConfig& cfg, Price tick_size);

}  // namespace tapewatch
