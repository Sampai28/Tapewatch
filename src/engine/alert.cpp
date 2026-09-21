#include "tapewatch/alert.hpp"

#include "tapewatch/json.hpp"

namespace tapewatch {

std::string alert_to_json(const Alert& a) {
    std::string out;
    out.reserve(768);
    JsonWriter w(out);
    w.begin_object();
    w.field("alert_id", static_cast<std::uint64_t>(a.alert_id));
    w.field("detector", detector_str(a.detector));
    w.field("symbol", a.symbol);
    w.field("participant", static_cast<std::uint64_t>(a.participant));
    if (a.has_counterparty) {
        w.field("counterparty", static_cast<std::uint64_t>(a.counterparty));
    } else {
        w.key("counterparty");
        w.null();
    }
    w.field("start_ts", static_cast<std::int64_t>(a.start_ts));
    w.field("end_ts", static_cast<std::int64_t>(a.end_ts));
    w.field("detect_ts", static_cast<std::int64_t>(a.detect_ts));
    w.field("seq_first", static_cast<std::uint64_t>(a.seq_first));
    w.field("seq_last", static_cast<std::uint64_t>(a.seq_last));
    w.field("score", a.score);
    w.field("penalty", a.penalty);
    w.field("confidence", a.confidence);
    w.field("degraded", a.degraded);

    w.key("signals");
    w.begin_object();
    for (const auto& s : a.signals) w.field(s.name, s.value);
    w.end_object();

    w.key("weights");
    w.begin_object();
    for (const auto& s : a.signals) w.field(s.name, s.weight);
    w.end_object();

    w.key("evidence");
    w.begin_object();
    for (const auto& [k, v] : a.evidence) w.field(k, static_cast<std::int64_t>(v));
    w.end_object();

    w.key("order_ids");
    w.begin_array();
    for (OrderId id : a.order_ids) w.value(static_cast<std::uint64_t>(id));
    w.end_array();

    w.end_object();
    return out;
}

}  // namespace tapewatch
