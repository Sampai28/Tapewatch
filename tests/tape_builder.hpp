// Builds normalised tapes by hand so a detector test states the scenario it
// is testing in market terms rather than in CSV.

#pragma once

#include <string>
#include <vector>

#include "tapewatch/event.hpp"
#include "tapewatch/pipeline.hpp"

namespace twtest {

using namespace tapewatch;

class Tape {
public:
    explicit Tape(std::string symbol = "TWX") : symbol_(std::move(symbol)) {}

    // Two tapes merged into one stream must not collide on sequence numbers
    // or event ids -- the feed guard would correctly call the second one a
    // duplicate storm.
    Tape& start_ids(SeqNum seq, EventId eid) {
        seq_ = seq;
        eid_ = eid;
        return *this;
    }

    Tape& at(Ts ms) {
        ts_ = ms * kNsPerMs;
        return *this;
    }
    Tape& advance(Ts ms) {
        ts_ += ms * kNsPerMs;
        return *this;
    }
    Ts now_ms() const { return ts_ / kNsPerMs; }

    Tape& order(ParticipantId pid, OrderId oid, Side s, Price px, Qty q) {
        MarketEvent e = base(EventKind::Order);
        e.participant = pid;
        e.order_id = oid;
        e.side = s;
        e.price = px;
        e.quantity = q;
        return push(e);
    }
    Tape& cancel(ParticipantId pid, OrderId oid) {
        MarketEvent e = base(EventKind::Cancel);
        e.participant = pid;
        e.order_id = oid;
        return push(e);
    }
    Tape& modify(ParticipantId pid, OrderId oid, Side s, Price px, Qty q) {
        MarketEvent e = base(EventKind::Modify);
        e.participant = pid;
        e.order_id = oid;
        e.side = s;
        e.price = px;
        e.quantity = q;
        return push(e);
    }
    // `aggressor` is the taker's side.
    Tape& trade(ParticipantId taker, OrderId taker_oid, ParticipantId maker, OrderId maker_oid,
                Side aggressor, Price px, Qty q) {
        MarketEvent e = base(EventKind::Trade);
        e.participant = taker;
        e.order_id = taker_oid;
        e.maker_participant = maker;
        e.maker_order_id = maker_oid;
        e.side = aggressor;
        e.price = px;
        e.quantity = q;
        return push(e);
    }

    // Raw line, for feed-integrity tests that need to break the rules.
    Tape& raw(const std::string& line) {
        lines_.push_back(line);
        return *this;
    }
    Tape& skip_seq(SeqNum n) {
        seq_ += n;
        return *this;
    }
    Tape& repeat_last_eid() {
        if (eid_ > 1) --eid_;
        return *this;
    }

    const std::vector<std::string>& lines() const { return lines_; }

    void run(Pipeline& p) const {
        for (const std::string& l : lines_) p.feed_line(l);
        p.finish();
    }

private:
    MarketEvent base(EventKind k) {
        MarketEvent e;
        e.kind = k;
        e.ts = ts_;
        e.seq = seq_++;
        e.eid = eid_++;
        e.symbol = symbol_;
        return e;
    }
    Tape& push(const MarketEvent& e) {
        lines_.push_back(format_event(e));
        return *this;
    }

    std::string symbol_;
    std::vector<std::string> lines_;
    Ts ts_{1'000'000'000};
    SeqNum seq_{1};
    EventId eid_{1};
};

class CollectingWriter final : public AlertWriter {
public:
    void write(const Alert& a) override { alerts.push_back(a); }

    std::size_t count(DetectorKind k) const {
        std::size_t n = 0;
        for (const auto& a : alerts)
            if (a.detector == k) ++n;
        return n;
    }
    std::size_t count(DetectorKind k, ParticipantId pid) const {
        std::size_t n = 0;
        for (const auto& a : alerts)
            if (a.detector == k && a.participant == pid) ++n;
        return n;
    }
    const Alert* first(DetectorKind k) const {
        for (const auto& a : alerts)
            if (a.detector == k) return &a;
        return nullptr;
    }

    std::vector<Alert> alerts;
};

// A config with the shipped defaults except that every detector's cooldown is
// short, so a two-second test tape is not silently deduplicated.
inline EngineConfig test_config() {
    EngineConfig c;
    c.scan_interval_ns = 250 * kNsPerMs;
    c.spoofing.cooldown_ns = 100 * kNsPerMs;
    c.layering.cooldown_ns = 100 * kNsPerMs;
    c.wash.cooldown_ns = 100 * kNsPerMs;
    c.momentum.cooldown_ns = 100 * kNsPerMs;
    return c;
}

}  // namespace twtest
