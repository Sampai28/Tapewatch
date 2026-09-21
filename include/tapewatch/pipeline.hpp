// The pipeline: tape in, alerts out.
//
//   line -> parse -> reorder buffer -> feed guard -> book + windows -> scan
//
// Everything above this layer is stateless about the market and everything
// below it is stateless about the run. The pipeline is where the two meet, and
// it owns the three things neither of them should: per-symbol state, alert
// de-duplication, and the decision that an alert was raised over damaged data.

#pragma once

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "tapewatch/config.hpp"
#include "tapewatch/detector.hpp"
#include "tapewatch/feed_integrity.hpp"

namespace tapewatch {

struct RunStats {
    FeedStats feed;
    BookStats book;

    std::uint64_t symbols{0};
    std::uint64_t scans{0};
    std::uint64_t alerts_emitted{0};
    std::uint64_t alerts_suppressed_cooldown{0};
    std::uint64_t alerts_degraded{0};
    std::uint64_t alerts_by_detector[static_cast<int>(DetectorKind::Count)]{};
    std::uint64_t suppressed_by_detector[static_cast<int>(DetectorKind::Count)]{};
    std::uint64_t window_overflows{0};

    Ts first_ts{0};
    Ts last_ts{0};
    double wall_seconds{0.0};

    Ts tape_span() const { return last_ts > first_ts ? last_ts - first_ts : 0; }
};

// Where alerts go. The CLI writes JSON Lines; tests collect into a vector.
class AlertWriter {
public:
    virtual ~AlertWriter() = default;
    virtual void write(const Alert& a) = 0;
};

class Pipeline {
public:
    Pipeline(const EngineConfig& cfg, AlertWriter& out);
    ~Pipeline();

    // Feeds one raw tape line. Safe to call with blank lines and comments.
    void feed_line(std::string_view line);
    // Releases the reorder buffer and runs a final scan. Call exactly once.
    void finish();

    const RunStats& stats() const { return stats_; }
    // Total lookback the windows are sized for; reported so a config whose
    // detector windows exceed their history is visible rather than silently
    // truncated.
    Ts history_ns() const { return history_ns_; }

private:
    class Sink;

    SymbolContext& context_for(const std::string& symbol);
    void apply(const MarketEvent& e);
    void push_cancel(SymbolContext& ctx, const MarketEvent& e, const RestingOrder& r);
    void advance_scans(Ts now, bool final_pass);
    void run_scan(SymbolContext& ctx, Ts from, Ts to);
    bool cooldown_blocks(const Alert& a);
    bool overlaps_damage(const Alert& a) const;

    EngineConfig cfg_;
    AlertWriter& out_;
    Ts history_ns_{0};

    ReorderBuffer reorder_;
    FeedGuard guard_;
    std::vector<MarketEvent> ready_;

    std::map<std::string, std::unique_ptr<SymbolContext>> symbols_;
    std::vector<std::unique_ptr<IDetector>> detectors_;

    struct CooldownKey {
        int detector;
        ParticipantId participant;
        ParticipantId counterparty;
        std::string symbol;
        bool operator<(const CooldownKey& o) const {
            if (detector != o.detector) return detector < o.detector;
            if (participant != o.participant) return participant < o.participant;
            if (counterparty != o.counterparty) return counterparty < o.counterparty;
            return symbol < o.symbol;
        }
    };
    std::map<CooldownKey, Ts> cooldown_;
    std::deque<Ts> damage_;

    RunStats stats_{};
    Ts next_scan_{0};
    Ts now_{0};
    bool started_{false};
    std::uint64_t next_alert_id_{1};
};

}  // namespace tapewatch
