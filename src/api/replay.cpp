#include "replay.hpp"

#include <algorithm>
#include <fstream>

#include "tapewatch/book_state.hpp"
#include "tapewatch/json.hpp"

namespace tapewatch::api {
namespace {

void apply(BookState& book, const MarketEvent& e) {
    switch (e.kind) {
        case EventKind::Order:
            book.on_order(e);
            break;
        case EventKind::Cancel: {
            RestingOrder removed{};
            book.on_cancel(e, removed);
            break;
        }
        case EventKind::Modify: {
            RestingOrder before{}, after{};
            book.on_modify(e, before, after);
            break;
        }
        case EventKind::Trade: {
            RestingOrder maker{};
            bool gone = false;
            book.on_trade(e, maker, gone);
            break;
        }
    }
    if (book.crossed()) book.repair_cross();
}

void write_side(JsonWriter& w, const BookState& book, Side side, std::size_t depth,
                const std::vector<OrderId>& highlight, const BookState& live) {
    w.begin_array();
    for (const auto& level : book.top(side, depth)) {
        w.begin_object();
        w.field("price", static_cast<std::int64_t>(level.price));
        w.field("quantity", static_cast<std::int64_t>(level.quantity));
        w.field("orders", static_cast<std::int64_t>(level.orders));
        // Is any of the alert's own orders resting at this level right now?
        bool flagged = false;
        Qty flagged_qty = 0;
        for (OrderId id : highlight) {
            const RestingOrder* r = live.find(id);
            if (r && r->side == side && r->price == level.price) {
                flagged = true;
                flagged_qty += r->remaining;
            }
        }
        w.field("subject", flagged);
        w.field("subject_qty", static_cast<std::int64_t>(flagged_qty));
        w.end_object();
    }
    w.end_array();
}

}  // namespace

bool Replay::load(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open tape: " + path;
        return false;
    }
    std::string line;
    MarketEvent e;
    std::uint64_t bad = 0;
    while (std::getline(in, line)) {
        const ParseError err = parse_event(line, e);
        if (err == ParseError::Empty) continue;
        if (err != ParseError::None) {
            ++bad;
            continue;
        }
        events_.push_back(e);
        if (std::find(symbols_.begin(), symbols_.end(), e.symbol) == symbols_.end())
            symbols_.push_back(e.symbol);
    }
    // The tape on disk may be out of order; the console has to show the same
    // book the detectors saw, and they see timestamp order.
    std::stable_sort(events_.begin(), events_.end(),
                     [](const MarketEvent& a, const MarketEvent& b) {
                         if (a.ts != b.ts) return a.ts < b.ts;
                         return a.seq < b.seq;
                     });
    std::sort(symbols_.begin(), symbols_.end());
    loaded_ = true;
    if (bad > 0) error = std::to_string(bad) + " unparseable lines skipped";
    return true;
}

std::string Replay::frames_json(const ReplayRequest& req) const {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field("symbol", req.symbol);
    w.field("from_ts", static_cast<std::int64_t>(req.from_ts));
    w.field("to_ts", static_cast<std::int64_t>(req.to_ts));

    BookState book(200000, 1);
    std::size_t i = 0;
    for (; i < events_.size(); ++i) {
        const MarketEvent& e = events_[i];
        if (e.ts >= req.from_ts) break;
        if (e.symbol != req.symbol) continue;
        apply(book, e);
    }

    // A window with more events than max_frames is sampled rather than
    // truncated, so the console always shows the end of the episode -- the
    // cancel and the trade that followed it are the interesting part and
    // truncating from the front would cut exactly those.
    std::size_t in_window = 0;
    for (std::size_t j = i; j < events_.size() && events_[j].ts <= req.to_ts; ++j)
        if (events_[j].symbol == req.symbol) ++in_window;
    const std::size_t stride =
        req.max_frames > 0 && in_window > req.max_frames ? (in_window / req.max_frames) + 1 : 1;

    w.field("events_in_window", static_cast<std::int64_t>(in_window));
    w.field("frame_stride", static_cast<std::int64_t>(stride));

    w.key("frames");
    w.begin_array();
    std::size_t seen = 0;
    for (; i < events_.size(); ++i) {
        const MarketEvent& e = events_[i];
        if (e.ts > req.to_ts) break;
        if (e.symbol != req.symbol) continue;
        apply(book, e);
        ++seen;
        if (seen % stride != 0 && seen != in_window) continue;

        w.begin_object();
        w.field("ts", static_cast<std::int64_t>(e.ts));
        w.field("seq", static_cast<std::uint64_t>(e.seq));
        w.field("kind", kind_str(e.kind));
        w.field("participant", static_cast<std::int64_t>(e.participant));
        w.field("order_id", static_cast<std::uint64_t>(e.order_id));
        w.field("side", side_str(e.side));
        w.field("price", static_cast<std::int64_t>(e.price));
        w.field("quantity", static_cast<std::int64_t>(e.quantity));
        if (e.kind == EventKind::Trade) {
            w.field("maker_participant", static_cast<std::int64_t>(e.maker_participant));
            w.field("maker_order_id", static_cast<std::uint64_t>(e.maker_order_id));
        }
        const Price bb = book.best(Side::Buy);
        const Price ba = book.best(Side::Sell);
        if (bb == kNoPrice) {
            w.key("best_bid");
            w.null();
        } else {
            w.field("best_bid", static_cast<std::int64_t>(bb));
        }
        if (ba == kNoPrice) {
            w.key("best_ask");
            w.null();
        } else {
            w.field("best_ask", static_cast<std::int64_t>(ba));
        }
        w.field("highlighted",
                std::find(req.highlight.begin(), req.highlight.end(), e.order_id) !=
                    req.highlight.end());
        w.key("bids");
        write_side(w, book, Side::Buy, req.depth, req.highlight, book);
        w.key("asks");
        write_side(w, book, Side::Sell, req.depth, req.highlight, book);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return out;
}

}  // namespace tapewatch::api
