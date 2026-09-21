// Core scalar types for the Tapewatch detection engine.
//
// Money and quantity are integers, everywhere, always. Prices are minor units
// (cents for a dollar-quoted instrument); quantities are whole lots. There is
// no floating point anywhere in the hot path or in anything that is compared
// for equality, because two surveillance runs over the same tape must produce
// the same alerts and 0.1 + 0.2 does not.
//
// Detector *scores* are the one exception: they are doubles, they are never
// compared for equality, and they are rounded to 6 places on the way out.

#pragma once

#include <cstdint>
#include <string>

namespace tapewatch {

using Price = std::int64_t;   // minor units (e.g. cents). Never a double.
using Qty = std::int64_t;     // whole lots.
using OrderId = std::uint64_t;
using ParticipantId = std::uint32_t;
using SeqNum = std::uint64_t;
using Ts = std::int64_t;      // nanoseconds since epoch of the tape.
using EventId = std::uint64_t;

// Signed so that "no price" and arithmetic on price differences stay in one
// type. kNoPrice is deliberately not 0 -- 0 is a legal price in some markets.
inline constexpr Price kNoPrice = INT64_MIN;

inline constexpr Ts kNsPerMs = 1'000'000;
inline constexpr Ts kNsPerSec = 1'000'000'000;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

inline const char* side_str(Side s) { return s == Side::Buy ? "B" : "S"; }

inline bool parse_side(char c, Side& out) {
    if (c == 'B') { out = Side::Buy; return true; }
    if (c == 'S') { out = Side::Sell; return true; }
    return false;
}

// The four record kinds in a normalised Tapewatch tape. See event.hpp for the
// wire format and docs/notes.md for why it is a superset of Matchbook's.
enum class EventKind : std::uint8_t {
    Order = 0,   // 'O' -- a new order became visible
    Cancel = 1,  // 'X' -- an order was withdrawn
    Modify = 2,  // 'M' -- price and/or quantity replaced (absolute values)
    Trade = 3,   // 'T' -- an execution between a taker and a maker
};

inline const char* kind_str(EventKind k) {
    switch (k) {
        case EventKind::Order: return "O";
        case EventKind::Cancel: return "X";
        case EventKind::Modify: return "M";
        case EventKind::Trade: return "T";
    }
    return "?";
}

// Detector identity. Kept as an enum rather than a string in the hot path;
// stringified once, at alert emission.
enum class DetectorKind : std::uint8_t {
    Spoofing = 0,
    Layering = 1,
    WashTrading = 2,
    MomentumIgnition = 3,
    Count = 4,
};

inline const char* detector_str(DetectorKind d) {
    switch (d) {
        case DetectorKind::Spoofing: return "spoofing";
        case DetectorKind::Layering: return "layering";
        case DetectorKind::WashTrading: return "wash_trading";
        case DetectorKind::MomentumIgnition: return "momentum_ignition";
        default: return "unknown";
    }
}

inline bool parse_detector(const std::string& s, DetectorKind& out) {
    if (s == "spoofing") { out = DetectorKind::Spoofing; return true; }
    if (s == "layering") { out = DetectorKind::Layering; return true; }
    if (s == "wash_trading") { out = DetectorKind::WashTrading; return true; }
    if (s == "momentum_ignition") { out = DetectorKind::MomentumIgnition; return true; }
    return false;
}

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

// Saturating ratio used all over the signal code: how far x has travelled
// towards a threshold, capped at 1. A zero or negative threshold means the
// signal is switched off rather than infinite.
inline double ratio01(double x, double threshold) {
    if (threshold <= 0.0) return 0.0;
    return clamp01(x / threshold);
}

}  // namespace tapewatch
