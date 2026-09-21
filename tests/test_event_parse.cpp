#include "tapewatch/event.hpp"
#include "test_harness.hpp"

using namespace tapewatch;

namespace {

MarketEvent roundtrip(const MarketEvent& in, ParseError& err) {
    MarketEvent out;
    err = parse_event(format_event(in), out);
    return out;
}

}  // namespace

TW_TEST(event_order_round_trips) {
    MarketEvent e;
    e.kind = EventKind::Order;
    e.ts = 1'234'567'890;
    e.seq = 42;
    e.eid = 99;
    e.symbol = "TWX";
    e.participant = 7;
    e.order_id = 1001;
    e.side = Side::Sell;
    e.price = -250;  // negative prices are legal in some markets and must survive
    e.quantity = 500;

    ParseError err;
    const MarketEvent back = roundtrip(e, err);
    TW_CHECK_EQ(static_cast<int>(err), static_cast<int>(ParseError::None));
    TW_CHECK_EQ(static_cast<int>(back.kind), static_cast<int>(EventKind::Order));
    TW_CHECK_EQ(back.ts, e.ts);
    TW_CHECK_EQ(back.seq, e.seq);
    TW_CHECK_EQ(back.eid, e.eid);
    TW_CHECK_EQ(back.symbol, e.symbol);
    TW_CHECK_EQ(back.participant, e.participant);
    TW_CHECK_EQ(back.order_id, e.order_id);
    TW_CHECK_EQ(static_cast<int>(back.side), static_cast<int>(Side::Sell));
    TW_CHECK_EQ(back.price, e.price);
    TW_CHECK_EQ(back.quantity, e.quantity);
}

TW_TEST(event_trade_round_trips_with_both_participants) {
    MarketEvent e;
    e.kind = EventKind::Trade;
    e.ts = 5;
    e.seq = 6;
    e.eid = 7;
    e.symbol = "AAA";
    e.participant = 11;       // taker
    e.order_id = 21;
    e.maker_participant = 12;
    e.maker_order_id = 22;
    e.side = Side::Buy;
    e.price = 10050;
    e.quantity = 3;

    ParseError err;
    const MarketEvent back = roundtrip(e, err);
    TW_CHECK_EQ(static_cast<int>(err), static_cast<int>(ParseError::None));
    TW_CHECK_EQ(back.participant, 11u);
    TW_CHECK_EQ(back.maker_participant, 12u);
    TW_CHECK_EQ(back.order_id, 21u);
    TW_CHECK_EQ(back.maker_order_id, 22u);
    TW_CHECK_EQ(static_cast<int>(back.side), static_cast<int>(Side::Buy));
}

TW_TEST(event_cancel_round_trips) {
    MarketEvent e;
    e.kind = EventKind::Cancel;
    e.ts = 1;
    e.seq = 2;
    e.eid = 3;
    e.symbol = "Z";
    e.participant = 4;
    e.order_id = 5;

    ParseError err;
    const MarketEvent back = roundtrip(e, err);
    TW_CHECK_EQ(static_cast<int>(err), static_cast<int>(ParseError::None));
    TW_CHECK_EQ(static_cast<int>(back.kind), static_cast<int>(EventKind::Cancel));
    TW_CHECK_EQ(back.order_id, 5u);
}

TW_TEST(event_rejects_malformed_lines) {
    MarketEvent e;
    TW_CHECK_EQ(static_cast<int>(parse_event("", e)), static_cast<int>(ParseError::Empty));
    TW_CHECK_EQ(static_cast<int>(parse_event("# a comment", e)),
                static_cast<int>(ParseError::Empty));
    TW_CHECK_EQ(static_cast<int>(parse_event("Q,1,2,3,TWX,4,5", e)),
                static_cast<int>(ParseError::UnknownKind));
    // An order line with a cancel's field count.
    TW_CHECK_EQ(static_cast<int>(parse_event("O,1,2,3,TWX,4,5", e)),
                static_cast<int>(ParseError::FieldCount));
    TW_CHECK_EQ(static_cast<int>(parse_event("O,1,2,3,TWX,4,5,X,100,10", e)),
                static_cast<int>(ParseError::BadSide));
    TW_CHECK_EQ(static_cast<int>(parse_event("O,1,2,3,TWX,4,5,B,10o,10", e)),
                static_cast<int>(ParseError::BadInteger));
    // Trailing garbage after a valid integer must not be accepted.
    TW_CHECK_EQ(static_cast<int>(parse_event("O,1,2,3,TWX,4,5,B,100,10x", e)),
                static_cast<int>(ParseError::BadInteger));
}

TW_TEST(event_tolerates_crlf) {
    MarketEvent e;
    TW_CHECK_EQ(static_cast<int>(parse_event("X,1,2,3,TWX,4,5\r\n", e)),
                static_cast<int>(ParseError::None));
    TW_CHECK_EQ(e.order_id, 5u);
}
