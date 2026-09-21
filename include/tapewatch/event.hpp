// The normalised Tapewatch tape: one market event per line, comma separated,
// every field an integer or a single character.
//
//   O,<ts>,<seq>,<eid>,<sym>,<pid>,<oid>,<side>,<px>,<qty>
//   X,<ts>,<seq>,<eid>,<sym>,<pid>,<oid>
//   M,<ts>,<seq>,<eid>,<sym>,<pid>,<oid>,<px>,<qty>
//   T,<ts>,<seq>,<eid>,<sym>,<taker_pid>,<taker_oid>,<maker_pid>,<maker_oid>,<side>,<px>,<qty>
//
//   ts    nanoseconds, monotonically non-decreasing in a clean feed
//   seq   venue sequence number, contiguous in a clean feed
//   eid   globally unique event id, used to spot duplicates
//   sym   symbol, bare ASCII, no commas
//   side  B or S; on a trade it is the *aggressor's* side
//   px    minor units; qty whole lots
//   M     carries absolute new price and quantity, not deltas
//
// Why not just replay Matchbook's event log? Because it has no timestamps (by
// design -- they would break byte-identical replay), no symbol, and no
// participant on output records. Surveillance needs all three. The adapter in
// tools/matchbook_adapter.py reconstructs them; this format is what it emits.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tapewatch/types.hpp"

namespace tapewatch {

struct MarketEvent {
    EventKind kind{EventKind::Order};
    Ts ts{0};
    SeqNum seq{0};
    EventId eid{0};
    std::string symbol;

    // Order / Cancel / Modify: the acting participant and their order.
    // Trade: the aggressor (taker).
    ParticipantId participant{0};
    OrderId order_id{0};

    // Trade only.
    ParticipantId maker_participant{0};
    OrderId maker_order_id{0};

    Side side{Side::Buy};
    Price price{kNoPrice};
    Qty quantity{0};

    bool is_trade() const { return kind == EventKind::Trade; }
};

// Parse failure detail, so the reader can report *why* a line was dropped
// rather than silently skipping it.
enum class ParseError : std::uint8_t {
    None = 0,
    Empty,
    UnknownKind,
    FieldCount,
    BadInteger,
    BadSide,
};

const char* parse_error_str(ParseError e);

// Parses one line into `out`. Returns ParseError::None on success. The line
// must not contain a trailing newline; the reader strips it.
ParseError parse_event(std::string_view line, MarketEvent& out);

// Renders `e` back to the canonical line form. parse(format(e)) == e for every
// event this codebase produces; tests/test_event_parse.cpp holds that round
// trip.
std::string format_event(const MarketEvent& e);

}  // namespace tapewatch
