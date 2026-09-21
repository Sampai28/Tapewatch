// A fixed-capacity, time-bounded sliding window.
//
// Every rolling statistic in the engine lives in one of these. The capacity is
// allocated once at construction and never grows, which is the whole point:
// surveillance runs against a tape of unknown length, and a detector whose
// memory is a function of how long the market has been open is a detector that
// dies at 15:59. Memory here is a function of the *window*, which is bounded by
// configuration.
//
// Two eviction rules, both cheap:
//   - time:     anything older than (now - window_ns) leaves the front
//   - capacity: if the ring is full, the oldest element leaves to make room
//
// The second one is a lie the caller needs to know about, because it silently
// narrows the window during a burst. `overflowed()` counts how often it has
// happened so the run can report it instead of quietly under-detecting.

#pragma once

#include <cstddef>
#include <vector>

#include "tapewatch/types.hpp"

namespace tapewatch {

template <typename T>
class TimeWindow {
public:
    TimeWindow(std::size_t capacity, Ts window_ns)
        : buf_(capacity ? capacity : 1), window_ns_(window_ns) {}

    // Appends and then evicts. `now` is the timestamp of the arriving element.
    void push(Ts now, const T& item) {
        if (size_ == buf_.size()) {
            head_ = next(head_);
            --size_;
            ++overflowed_;
        }
        const std::size_t slot = (head_ + size_) % buf_.size();
        buf_[slot] = Entry{now, item};
        ++size_;
        expire(now);
    }

    // Drops everything older than the window. Safe to call without pushing --
    // detectors call it before reading statistics so a quiet period does not
    // leave stale elements visible.
    void expire(Ts now) {
        const Ts cutoff = now - window_ns_;
        while (size_ > 0 && buf_[head_].ts < cutoff) {
            head_ = next(head_);
            --size_;
        }
    }

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    std::size_t capacity() const { return buf_.size(); }
    std::uint64_t overflowed() const { return overflowed_; }
    Ts window_ns() const { return window_ns_; }

    // Indexed oldest-first. Windows are small (bounded by config) and every
    // consumer is a linear scan, so an index operator beats an iterator here.
    const T& operator[](std::size_t i) const { return buf_[(head_ + i) % buf_.size()].item; }
    Ts ts_at(std::size_t i) const { return buf_[(head_ + i) % buf_.size()].ts; }

    // Index of the first element with ts >= t, or size() if none. Elements go
    // in in timestamp order (the reorder buffer upstream guarantees it), so a
    // binary search is valid and lets a detector scan a three-second slice of
    // a thirty-second window instead of the whole thing.
    std::size_t lower_bound_ts(Ts t) const {
        std::size_t lo = 0, hi = size_;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (ts_at(mid) < t) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    const T& front() const { return buf_[head_].item; }
    const T& back() const { return buf_[(head_ + size_ - 1) % buf_.size()].item; }
    Ts front_ts() const { return buf_[head_].ts; }
    Ts back_ts() const { return buf_[(head_ + size_ - 1) % buf_.size()].ts; }

    void clear() {
        head_ = 0;
        size_ = 0;
    }

private:
    struct Entry {
        Ts ts{0};
        T item{};
    };

    std::size_t next(std::size_t i) const { return (i + 1) % buf_.size(); }

    std::vector<Entry> buf_;
    Ts window_ns_;
    std::size_t head_{0};
    std::size_t size_{0};
    std::uint64_t overflowed_{0};
};

}  // namespace tapewatch
