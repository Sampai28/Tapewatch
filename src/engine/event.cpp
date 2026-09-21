#include "tapewatch/event.hpp"

#include <charconv>
#include <cstdio>

namespace tapewatch {
namespace {

// Splits on commas without allocating. Returns the number of fields written,
// which may exceed `max` -- the caller checks for an exact count, so an
// over-long line is rejected rather than silently truncated.
std::size_t split(std::string_view line, std::string_view* out, std::size_t max) {
    std::size_t n = 0;
    std::size_t start = 0;
    while (true) {
        std::size_t comma = line.find(',', start);
        if (n < max) out[n] = line.substr(start, comma == std::string_view::npos
                                                     ? std::string_view::npos
                                                     : comma - start);
        ++n;
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return n;
}

template <typename T>
bool to_int(std::string_view s, T& out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

}  // namespace

const char* parse_error_str(ParseError e) {
    switch (e) {
        case ParseError::None: return "none";
        case ParseError::Empty: return "empty_line";
        case ParseError::UnknownKind: return "unknown_kind";
        case ParseError::FieldCount: return "field_count";
        case ParseError::BadInteger: return "bad_integer";
        case ParseError::BadSide: return "bad_side";
    }
    return "unknown";
}

ParseError parse_event(std::string_view line, MarketEvent& out) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (line.empty() || line.front() == '#') return ParseError::Empty;

    std::string_view f[12];
    const std::size_t n = split(line, f, 12);
    if (n < 7) return ParseError::FieldCount;
    if (f[0].size() != 1) return ParseError::UnknownKind;

    out = MarketEvent{};

    if (!to_int(f[1], out.ts)) return ParseError::BadInteger;
    if (!to_int(f[2], out.seq)) return ParseError::BadInteger;
    if (!to_int(f[3], out.eid)) return ParseError::BadInteger;
    out.symbol.assign(f[4]);
    if (out.symbol.empty()) return ParseError::FieldCount;

    switch (f[0][0]) {
        case 'O':
        case 'M': {
            if (n != 10) return ParseError::FieldCount;
            out.kind = f[0][0] == 'O' ? EventKind::Order : EventKind::Modify;
            if (!to_int(f[5], out.participant)) return ParseError::BadInteger;
            if (!to_int(f[6], out.order_id)) return ParseError::BadInteger;
            if (f[7].size() != 1 || !parse_side(f[7][0], out.side)) return ParseError::BadSide;
            if (!to_int(f[8], out.price)) return ParseError::BadInteger;
            if (!to_int(f[9], out.quantity)) return ParseError::BadInteger;
            return ParseError::None;
        }
        case 'X': {
            if (n != 7) return ParseError::FieldCount;
            out.kind = EventKind::Cancel;
            if (!to_int(f[5], out.participant)) return ParseError::BadInteger;
            if (!to_int(f[6], out.order_id)) return ParseError::BadInteger;
            return ParseError::None;
        }
        case 'T': {
            if (n != 12) return ParseError::FieldCount;
            out.kind = EventKind::Trade;
            if (!to_int(f[5], out.participant)) return ParseError::BadInteger;
            if (!to_int(f[6], out.order_id)) return ParseError::BadInteger;
            if (!to_int(f[7], out.maker_participant)) return ParseError::BadInteger;
            if (!to_int(f[8], out.maker_order_id)) return ParseError::BadInteger;
            if (f[9].size() != 1 || !parse_side(f[9][0], out.side)) return ParseError::BadSide;
            if (!to_int(f[10], out.price)) return ParseError::BadInteger;
            if (!to_int(f[11], out.quantity)) return ParseError::BadInteger;
            return ParseError::None;
        }
        default:
            return ParseError::UnknownKind;
    }
}

std::string format_event(const MarketEvent& e) {
    char buf[320];
    int n = 0;
    switch (e.kind) {
        case EventKind::Order:
        case EventKind::Modify:
            n = std::snprintf(buf, sizeof buf, "%s,%lld,%llu,%llu,%s,%u,%llu,%s,%lld,%lld",
                              kind_str(e.kind), (long long)e.ts, (unsigned long long)e.seq,
                              (unsigned long long)e.eid, e.symbol.c_str(), e.participant,
                              (unsigned long long)e.order_id, side_str(e.side),
                              (long long)e.price, (long long)e.quantity);
            break;
        case EventKind::Cancel:
            n = std::snprintf(buf, sizeof buf, "X,%lld,%llu,%llu,%s,%u,%llu", (long long)e.ts,
                              (unsigned long long)e.seq, (unsigned long long)e.eid,
                              e.symbol.c_str(), e.participant, (unsigned long long)e.order_id);
            break;
        case EventKind::Trade:
            n = std::snprintf(buf, sizeof buf,
                              "T,%lld,%llu,%llu,%s,%u,%llu,%u,%llu,%s,%lld,%lld", (long long)e.ts,
                              (unsigned long long)e.seq, (unsigned long long)e.eid,
                              e.symbol.c_str(), e.participant, (unsigned long long)e.order_id,
                              e.maker_participant, (unsigned long long)e.maker_order_id,
                              side_str(e.side), (long long)e.price, (long long)e.quantity);
            break;
    }
    return std::string(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
}

}  // namespace tapewatch
