// Helpers shared by the four detectors.
//
// The one that matters is `participant_side`. A participant's *direction* in a
// trade is not the trade's side field: the tape records the aggressor's side,
// so a participant who is the resting maker traded the other way. Getting this
// backwards inverts every opposite-side test in the file, which is the whole
// discriminator between manipulation and ordinary two-way flow, and it would
// do so silently -- the detector would still fire, just on the wrong people.

#pragma once

#include <algorithm>
#include <cstdint>

#include "tapewatch/detector.hpp"

namespace tapewatch {
namespace detail {

// Did `pid` buy or sell in this trade? Returns false when they were not
// involved at all.
inline bool participant_side(const TradeRec& t, ParticipantId pid, Side& out) {
    if (t.taker == pid) {
        out = t.aggressor;
        return true;
    }
    if (t.maker == pid) {
        out = opposite(t.aggressor);
        return true;
    }
    return false;
}

// Whether the participant chose the moment of a trade or merely supplied the
// liquidity for it.
//
// This distinction does most of the work of separating spoofing from market
// making, and it is available directly on the tape. A spoofer pulls a bid and
// then *sells aggressively* -- taking the offer was the entire point of the
// display. A market maker pulls a bid and is then *lifted on its own offer* --
// passive, chosen by somebody else, and no more evidence of intent than the
// weather. Counting both as "traded the other way" makes every market maker
// on the venue look like a spoofer, which is exactly what it did before this
// was split out.
enum class Role { Any, Aggressor, Maker };

inline bool role_matches(const TradeRec& t, ParticipantId pid, Role role) {
    switch (role) {
        case Role::Aggressor: return t.taker == pid;
        case Role::Maker: return t.maker == pid;
        case Role::Any: return t.taker == pid || t.maker == pid;
    }
    return false;
}

// Executed quantity for `pid` in direction `want`, over trades with timestamps
// in [lo, hi]. The slice is found by binary search, so the cost is the size of
// the slice and not the size of the window.
inline Qty directional_qty(const TimeWindow<TradeRec>& trades, ParticipantId pid, Side want, Ts lo,
                           Ts hi, Role role = Role::Any, Ts* first_ts = nullptr,
                           Ts* last_ts = nullptr) {
    Qty total = 0;
    const std::size_t begin = trades.lower_bound_ts(lo);
    for (std::size_t i = begin; i < trades.size(); ++i) {
        const Ts ts = trades.ts_at(i);
        if (ts > hi) break;
        Side s;
        if (!participant_side(trades[i], pid, s) || s != want) continue;
        if (!role_matches(trades[i], pid, role)) continue;
        total += trades[i].qty;
        if (first_ts && (*first_ts == 0 || ts < *first_ts)) *first_ts = ts;
        if (last_ts && ts > *last_ts) *last_ts = ts;
    }
    return total;
}

// How two-sided was this participant's quoting over [lo, hi]? 0 means they
// only ever posted on one side; 1 means a perfect balance of placements on
// both. Market makers sit near 1 by definition, which is exactly why this is
// subtracted from the spoofing and layering scores rather than added.
inline double two_sidedness(const TimeWindow<PlacementRec>& placements, ParticipantId pid, Ts lo,
                            Ts hi) {
    std::uint32_t n[2] = {0, 0};
    const std::size_t begin = placements.lower_bound_ts(lo);
    for (std::size_t i = begin; i < placements.size(); ++i) {
        if (placements.ts_at(i) > hi) break;
        if (placements[i].participant != pid) continue;
        n[static_cast<int>(placements[i].side)] += 1;
    }
    const std::uint32_t lo_n = std::min(n[0], n[1]);
    const std::uint32_t hi_n = std::max(n[0], n[1]);
    if (hi_n == 0 || lo_n == 0) return 0.0;
    return 2.0 * static_cast<double>(lo_n) / static_cast<double>(lo_n + hi_n);
}

// Absolute distance from `px` to the midpoint, in ticks. -1 when there was no
// two-sided market to measure against.
inline int ticks_from_mid(Price mid2, Price px, Price tick_size) {
    if (mid2 == kNoPrice || tick_size <= 0) return -1;
    const Price diff2 = std::abs(2 * px - mid2);
    return static_cast<int>(diff2 / (2 * tick_size));
}

}  // namespace detail
}  // namespace tapewatch
