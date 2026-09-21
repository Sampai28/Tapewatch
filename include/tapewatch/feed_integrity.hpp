// Feed integrity.
//
// A surveillance system that assumes a clean feed is a surveillance system
// that reports clean results on a damaged one, which is worse than reporting
// nothing. Real market data arrives with sequence gaps, duplicated messages,
// timestamps that disagree with sequence order, and events that show up after
// the ones that logically follow them.
//
// Tapewatch handles four defects explicitly:
//
//   sequence gap        seq jumps. Events are missing and the book is now
//                       wrong in a way we cannot repair. Alerts raised near a
//                       gap are marked `degraded` and their confidence is cut.
//   duplicate event id  the same eid twice. Dropped, counted. Applying a
//                       duplicate trade would double-count volume, which is
//                       exactly the input a wash-trade test is most sensitive
//                       to.
//   out of order        an event arriving after a later-timestamped one. A
//                       small reorder buffer fixes anything inside tolerance;
//                       anything later is dropped as unusable, because
//                       applying a cancel before the order it cancels
//                       corrupts the book for everything that follows.
//   clock disagreement  timestamp order and sequence order contradict each
//                       other. Nothing can be repaired here -- we do not know
//                       which of the two is lying -- so the event is applied
//                       in timestamp order and the window is marked degraded.
//
// None of these abort the run. All of them are counted, all of the counts go
// into the run manifest and the report, and a run over a damaged feed says so.

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

#include "tapewatch/event.hpp"
#include "tapewatch/types.hpp"

namespace tapewatch {

struct FeedStats {
    std::uint64_t lines_read{0};
    std::uint64_t parse_failures{0};
    std::uint64_t events_admitted{0};

    std::uint64_t duplicate_event_ids{0};
    std::uint64_t sequence_gaps{0};       // number of discontinuities
    std::uint64_t sequence_missing{0};    // total events implied missing
    std::uint64_t sequence_backward{0};   // ts order and seq order disagree
    std::uint64_t reordered_recovered{0}; // fixed by the reorder buffer
    std::uint64_t reordered_dropped{0};   // arrived beyond tolerance
    std::uint64_t degraded_events{0};     // processed while the feed was tainted

    std::uint64_t parse_error_kind[8]{};
};

// Releases events in timestamp order, holding each for `tolerance` of tape
// time so that a late arrival inside the tolerance can slot into place.
class ReorderBuffer {
public:
    ReorderBuffer(Ts tolerance, std::size_t capacity)
        : tolerance_(tolerance), capacity_(capacity ? capacity : 1) {}

    // Returns false when the event arrived too late to be placed correctly;
    // the caller drops and counts it.
    bool push(const MarketEvent& e);

    // Moves every event now safe to release into `out`, in timestamp order.
    void drain_ready(std::vector<MarketEvent>& out);
    // Releases everything, in order. Called once at end of tape.
    void flush(std::vector<MarketEvent>& out);

    std::size_t size() const { return heap_.size(); }
    std::uint64_t recovered() const { return recovered_; }
    std::uint64_t overflow_releases() const { return overflow_releases_; }

private:
    // (ts, seq) ordering: a stable total order even when two events share a
    // nanosecond, which synthetic tapes do constantly.
    struct Node {
        Ts ts;
        SeqNum seq;
        MarketEvent event;
    };
    struct Later {
        bool operator()(const Node& a, const Node& b) const {
            if (a.ts != b.ts) return a.ts > b.ts;
            return a.seq > b.seq;
        }
    };

    void pop_top(std::vector<MarketEvent>& out);

    std::vector<Node> heap_;
    Ts tolerance_;
    std::size_t capacity_;
    Ts max_ts_seen_{INT64_MIN};
    Ts last_released_ts_{INT64_MIN};
    std::uint64_t recovered_{0};
    std::uint64_t overflow_releases_{0};
};

// Runs after the reorder buffer, on a timestamp-ordered stream.
class FeedGuard {
public:
    FeedGuard(Ts degrade_ns, std::size_t dedup_capacity)
        : degrade_ns_(degrade_ns), dedup_capacity_(dedup_capacity ? dedup_capacity : 1) {}

    enum class Verdict { Accept, DropDuplicate };

    Verdict admit(const MarketEvent& e, FeedStats& stats);

    bool degraded(Ts now) const { return now < degraded_until_; }
    Ts degraded_until() const { return degraded_until_; }

private:
    void taint(Ts now) { degraded_until_ = std::max(degraded_until_, now + degrade_ns_); }

    Ts degrade_ns_;
    std::size_t dedup_capacity_;
    // A bounded LRU of recently seen ids. Unbounded would mean a set that
    // grows with the tape, which is the thing this engine promises not to do.
    std::unordered_set<EventId> seen_;
    std::deque<EventId> seen_order_;

    bool have_last_{false};
    SeqNum last_seq_{0};
    Ts degraded_until_{INT64_MIN};
};

std::string feed_stats_to_json(const FeedStats& s);

}  // namespace tapewatch
