#include "tapewatch/feed_integrity.hpp"

#include <algorithm>

#include "tapewatch/json.hpp"

namespace tapewatch {

bool ReorderBuffer::push(const MarketEvent& e) {
    const bool late = e.ts < last_released_ts_;
    if (late) return false;

    const bool was_out_of_order = e.ts < max_ts_seen_;
    max_ts_seen_ = std::max(max_ts_seen_, e.ts);
    heap_.push_back(Node{e.ts, e.seq, e});
    std::push_heap(heap_.begin(), heap_.end(), Later{});
    if (was_out_of_order) ++recovered_;
    return true;
}

void ReorderBuffer::pop_top(std::vector<MarketEvent>& out) {
    std::pop_heap(heap_.begin(), heap_.end(), Later{});
    last_released_ts_ = heap_.back().ts;
    out.push_back(std::move(heap_.back().event));
    heap_.pop_back();
}

void ReorderBuffer::drain_ready(std::vector<MarketEvent>& out) {
    const Ts watermark = max_ts_seen_ - tolerance_;
    while (!heap_.empty() && heap_.front().ts <= watermark) pop_top(out);
    // Capacity is a hard wall, not a hint: a tape whose timestamps stall while
    // messages keep coming must not be allowed to buffer without limit. When
    // it happens the buffer releases early, which can let an out-of-order
    // event through, so it is counted rather than absorbed.
    while (heap_.size() > capacity_) {
        ++overflow_releases_;
        pop_top(out);
    }
}

void ReorderBuffer::flush(std::vector<MarketEvent>& out) {
    while (!heap_.empty()) pop_top(out);
}

FeedGuard::Verdict FeedGuard::admit(const MarketEvent& e, FeedStats& stats) {
    if (seen_.count(e.eid)) {
        ++stats.duplicate_event_ids;
        taint(e.ts);
        return Verdict::DropDuplicate;
    }
    seen_.insert(e.eid);
    seen_order_.push_back(e.eid);
    if (seen_order_.size() > dedup_capacity_) {
        seen_.erase(seen_order_.front());
        seen_order_.pop_front();
    }

    if (have_last_) {
        if (e.seq > last_seq_ + 1) {
            ++stats.sequence_gaps;
            stats.sequence_missing += e.seq - last_seq_ - 1;
            taint(e.ts);
        } else if (e.seq <= last_seq_) {
            // Timestamp order put this event here, sequence order disagrees.
            // We cannot tell which clock is wrong, so we keep the event, keep
            // the timestamp ordering, and mark the window.
            ++stats.sequence_backward;
            taint(e.ts);
        }
    }
    have_last_ = true;
    last_seq_ = std::max(last_seq_, e.seq);

    ++stats.events_admitted;
    if (degraded(e.ts)) ++stats.degraded_events;
    return Verdict::Accept;
}

std::string feed_stats_to_json(const FeedStats& s) {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field("lines_read", s.lines_read);
    w.field("parse_failures", s.parse_failures);
    w.field("events_admitted", s.events_admitted);
    w.field("duplicate_event_ids", s.duplicate_event_ids);
    w.field("sequence_gaps", s.sequence_gaps);
    w.field("sequence_missing", s.sequence_missing);
    w.field("sequence_backward", s.sequence_backward);
    w.field("reordered_recovered", s.reordered_recovered);
    w.field("reordered_dropped", s.reordered_dropped);
    w.field("degraded_events", s.degraded_events);
    w.key("parse_errors");
    w.begin_object();
    for (int i = 0; i <= static_cast<int>(ParseError::BadSide); ++i)
        w.field(parse_error_str(static_cast<ParseError>(i)), s.parse_error_kind[i]);
    w.end_object();
    w.end_object();
    return out;
}

}  // namespace tapewatch
