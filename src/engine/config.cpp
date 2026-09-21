#include "tapewatch/config.hpp"

#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>

#include "tapewatch/json.hpp"

namespace tapewatch {
namespace {

// Durations are milliseconds in the file and nanoseconds in the struct. The
// file is edited by humans and swept by a script; nanoseconds in a config are
// a typo waiting to happen.
Ts ms(const JsonValue& o, const char* key, Ts fallback_ns) {
    const JsonValue* v = o.get(key);
    if (!v || v->type != JsonValue::Type::Number) return fallback_ns;
    return static_cast<Ts>(v->num * static_cast<double>(kNsPerMs));
}

void collect_unknown(const JsonValue& o, const std::string& prefix,
                     std::initializer_list<const char*> known, std::vector<std::string>& out) {
    if (!o.is_object()) return;
    std::set<std::string> ks(known.begin(), known.end());
    for (const auto& [k, _] : o.obj)
        if (!ks.count(k)) out.push_back(prefix + k);
}

void weights(const JsonValue& parent, const std::string& prefix,
             std::initializer_list<const char*> names, std::vector<std::string>& unknown) {
    const JsonValue* w = parent.get("weights");
    if (w) collect_unknown(*w, prefix + "weights.", names, unknown);
}

double wt(const JsonValue& parent, const char* name, double fallback) {
    const JsonValue* w = parent.get("weights");
    if (!w) return fallback;
    return w->num_or(name, fallback);
}

}  // namespace

ConfigLoad parse_config(const std::string& text) {
    ConfigLoad out;
    JsonValue root;
    std::string err;
    if (!json_parse(text, root, err)) {
        out.ok = false;
        out.error = "config parse failed: " + err;
        return out;
    }
    if (!root.is_object()) {
        out.ok = false;
        out.error = "config root is not an object";
        return out;
    }

    EngineConfig c;
    c.tick_size = root.int_or("tick_size", c.tick_size);
    c.max_resting_orders =
        static_cast<std::size_t>(root.int_or("max_resting_orders",
                                             static_cast<std::int64_t>(c.max_resting_orders)));
    c.window_capacity = static_cast<std::size_t>(
        root.int_or("window_capacity", static_cast<std::int64_t>(c.window_capacity)));
    c.feed_degrade_ns = ms(root, "feed_degrade_ms", c.feed_degrade_ns);
    c.degrade_factor = root.num_or("degrade_factor", c.degrade_factor);
    c.reorder_tolerance_ns = ms(root, "reorder_tolerance_ms", c.reorder_tolerance_ns);
    c.scan_interval_ns = ms(root, "scan_interval_ms", c.scan_interval_ns);

    collect_unknown(root, "",
                    {"tick_size", "max_resting_orders", "window_capacity", "feed_degrade_ms",
                     "degrade_factor", "reorder_tolerance_ms", "scan_interval_ms", "spoofing",
                     "layering", "wash_trading", "momentum_ignition", "_comment"},
                    out.unknown_keys);

    if (const JsonValue* s = root.get("spoofing")) {
        auto& d = c.spoofing;
        d.enabled = s->bool_or("enabled", d.enabled);
        d.opposite_window_ns = ms(*s, "opposite_window_ms", d.opposite_window_ns);
        d.max_lifetime_ns = ms(*s, "max_lifetime_ms", d.max_lifetime_ns);
        d.size_depth_multiple = s->num_or("size_depth_multiple", d.size_depth_multiple);
        d.near_touch_ticks = static_cast<int>(s->int_or("near_touch_ticks", d.near_touch_ticks));
        d.min_qty = s->int_or("min_qty", d.min_qty);
        d.min_score = s->num_or("min_score", d.min_score);
        d.cooldown_ns = ms(*s, "cooldown_ms", d.cooldown_ns);
        d.two_sided_penalty = s->num_or("two_sided_penalty", d.two_sided_penalty);
        d.w_size = wt(*s, "size", d.w_size);
        d.w_life = wt(*s, "life", d.w_life);
        d.w_unfilled = wt(*s, "unfilled", d.w_unfilled);
        d.w_opposite = wt(*s, "opposite", d.w_opposite);
        d.w_press = wt(*s, "press", d.w_press);
        collect_unknown(*s, "spoofing.",
                        {"enabled", "opposite_window_ms", "max_lifetime_ms", "size_depth_multiple",
                         "near_touch_ticks", "min_qty", "min_score", "cooldown_ms",
                         "two_sided_penalty", "weights"},
                        out.unknown_keys);
        weights(*s, "spoofing.", {"size", "life", "unfilled", "opposite", "press"},
                out.unknown_keys);
    }

    if (const JsonValue* s = root.get("layering")) {
        auto& d = c.layering;
        d.enabled = s->bool_or("enabled", d.enabled);
        d.placement_window_ns = ms(*s, "placement_window_ms", d.placement_window_ns);
        d.cancel_span_ns = ms(*s, "cancel_span_ms", d.cancel_span_ns);
        d.opposite_window_ns = ms(*s, "opposite_window_ms", d.opposite_window_ns);
        d.min_levels = static_cast<int>(s->int_or("min_levels", d.min_levels));
        d.min_orders = static_cast<int>(s->int_or("min_orders", d.min_orders));
        d.near_touch_ticks = static_cast<int>(s->int_or("near_touch_ticks", d.near_touch_ticks));
        d.imbalance_target = s->num_or("imbalance_target", d.imbalance_target);
        d.min_score = s->num_or("min_score", d.min_score);
        d.cooldown_ns = ms(*s, "cooldown_ms", d.cooldown_ns);
        d.two_sided_penalty = s->num_or("two_sided_penalty", d.two_sided_penalty);
        d.w_levels = wt(*s, "levels", d.w_levels);
        d.w_imbalance = wt(*s, "imbalance", d.w_imbalance);
        d.w_coord = wt(*s, "coord", d.w_coord);
        d.w_opposite = wt(*s, "opposite", d.w_opposite);
        d.w_unfilled = wt(*s, "unfilled", d.w_unfilled);
        collect_unknown(*s, "layering.",
                        {"enabled", "placement_window_ms", "cancel_span_ms", "opposite_window_ms",
                         "min_levels", "min_orders", "near_touch_ticks", "imbalance_target",
                         "min_score", "cooldown_ms", "two_sided_penalty", "weights"},
                        out.unknown_keys);
        weights(*s, "layering.", {"levels", "imbalance", "coord", "opposite", "unfilled"},
                out.unknown_keys);
    }

    if (const JsonValue* s = root.get("wash_trading")) {
        auto& d = c.wash;
        d.enabled = s->bool_or("enabled", d.enabled);
        d.window_ns = ms(*s, "window_ms", d.window_ns);
        d.min_gross_qty = s->int_or("min_gross_qty", d.min_gross_qty);
        d.recip_target = s->num_or("recip_target", d.recip_target);
        d.share_target = s->num_or("share_target", d.share_target);
        d.offmarket_ticks = static_cast<int>(s->int_or("offmarket_ticks", d.offmarket_ticks));
        d.breadth_penalty = s->num_or("breadth_penalty", d.breadth_penalty);
        d.breadth_target = s->num_or("breadth_target", d.breadth_target);
        d.min_score = s->num_or("min_score", d.min_score);
        d.cooldown_ns = ms(*s, "cooldown_ms", d.cooldown_ns);
        d.w_self = wt(*s, "self", d.w_self);
        d.w_recip = wt(*s, "recip", d.w_recip);
        d.w_nonet = wt(*s, "nonet", d.w_nonet);
        d.w_share = wt(*s, "share", d.w_share);
        d.w_price = wt(*s, "price", d.w_price);
        collect_unknown(*s, "wash_trading.",
                        {"enabled", "window_ms", "min_gross_qty", "recip_target", "share_target",
                         "offmarket_ticks", "breadth_penalty", "breadth_target", "min_score",
                         "cooldown_ms", "weights"},
                        out.unknown_keys);
        weights(*s, "wash_trading.", {"self", "recip", "nonet", "share", "price"},
                out.unknown_keys);
    }

    if (const JsonValue* s = root.get("momentum_ignition")) {
        auto& d = c.momentum;
        d.enabled = s->bool_or("enabled", d.enabled);
        d.burst_ns = ms(*s, "burst_ms", d.burst_ns);
        d.reversal_ns = ms(*s, "reversal_ms", d.reversal_ns);
        d.revert_ns = ms(*s, "revert_ms", d.revert_ns);
        d.min_move_ticks = static_cast<int>(s->int_or("min_move_ticks", d.min_move_ticks));
        d.move_target_ticks = static_cast<int>(s->int_or("move_target_ticks", d.move_target_ticks));
        d.min_orders = static_cast<int>(s->int_or("min_orders", d.min_orders));
        d.burst_target_orders =
            static_cast<int>(s->int_or("burst_target_orders", d.burst_target_orders));
        d.share_target = s->num_or("share_target", d.share_target);
        d.min_share = s->num_or("min_share", d.min_share);
        d.min_reverse_fraction = s->num_or("min_reverse_fraction", d.min_reverse_fraction);
        d.min_score = s->num_or("min_score", d.min_score);
        d.cooldown_ns = ms(*s, "cooldown_ms", d.cooldown_ns);
        d.w_burst = wt(*s, "burst", d.w_burst);
        d.w_move = wt(*s, "move", d.w_move);
        d.w_share = wt(*s, "share", d.w_share);
        d.w_reverse = wt(*s, "reverse", d.w_reverse);
        d.w_revert = wt(*s, "revert", d.w_revert);
        collect_unknown(*s, "momentum_ignition.",
                        {"enabled", "burst_ms", "reversal_ms", "revert_ms", "min_move_ticks",
                         "move_target_ticks", "min_orders", "burst_target_orders", "share_target",
                         "min_share", "min_reverse_fraction", "min_score", "cooldown_ms",
                         "weights"},
                        out.unknown_keys);
        weights(*s, "momentum_ignition.", {"burst", "move", "share", "reverse", "revert"},
                out.unknown_keys);
    }

    if (c.tick_size <= 0) {
        out.ok = false;
        out.error = "tick_size must be positive";
        return out;
    }
    if (c.window_capacity == 0) {
        out.ok = false;
        out.error = "window_capacity must be positive";
        return out;
    }
    if (c.scan_interval_ns <= 0) {
        out.ok = false;
        out.error = "scan_interval_ms must be positive";
        return out;
    }

    out.config = c;
    return out;
}

ConfigLoad load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        ConfigLoad out;
        out.ok = false;
        out.error = "cannot open config: " + path;
        return out;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_config(ss.str());
}

std::string config_to_json(const EngineConfig& c) {
    std::string out;
    JsonWriter w(out);
    auto msv = [](Ts ns) { return static_cast<double>(ns) / static_cast<double>(kNsPerMs); };

    w.begin_object();
    w.field("tick_size", static_cast<std::int64_t>(c.tick_size));
    w.field("max_resting_orders", static_cast<std::int64_t>(c.max_resting_orders));
    w.field("window_capacity", static_cast<std::int64_t>(c.window_capacity));
    w.field("feed_degrade_ms", msv(c.feed_degrade_ns));
    w.field("degrade_factor", c.degrade_factor);
    w.field("reorder_tolerance_ms", msv(c.reorder_tolerance_ns));
    w.field("scan_interval_ms", msv(c.scan_interval_ns));

    w.key("spoofing");
    w.begin_object();
    w.field("enabled", c.spoofing.enabled);
    w.field("opposite_window_ms", msv(c.spoofing.opposite_window_ns));
    w.field("max_lifetime_ms", msv(c.spoofing.max_lifetime_ns));
    w.field("size_depth_multiple", c.spoofing.size_depth_multiple);
    w.field("near_touch_ticks", c.spoofing.near_touch_ticks);
    w.field("min_qty", static_cast<std::int64_t>(c.spoofing.min_qty));
    w.field("min_score", c.spoofing.min_score);
    w.field("cooldown_ms", msv(c.spoofing.cooldown_ns));
    w.field("two_sided_penalty", c.spoofing.two_sided_penalty);
    w.key("weights");
    w.begin_object();
    w.field("size", c.spoofing.w_size);
    w.field("life", c.spoofing.w_life);
    w.field("unfilled", c.spoofing.w_unfilled);
    w.field("opposite", c.spoofing.w_opposite);
    w.field("press", c.spoofing.w_press);
    w.end_object();
    w.end_object();

    w.key("layering");
    w.begin_object();
    w.field("enabled", c.layering.enabled);
    w.field("placement_window_ms", msv(c.layering.placement_window_ns));
    w.field("cancel_span_ms", msv(c.layering.cancel_span_ns));
    w.field("opposite_window_ms", msv(c.layering.opposite_window_ns));
    w.field("min_levels", c.layering.min_levels);
    w.field("min_orders", c.layering.min_orders);
    w.field("near_touch_ticks", c.layering.near_touch_ticks);
    w.field("imbalance_target", c.layering.imbalance_target);
    w.field("min_score", c.layering.min_score);
    w.field("cooldown_ms", msv(c.layering.cooldown_ns));
    w.field("two_sided_penalty", c.layering.two_sided_penalty);
    w.key("weights");
    w.begin_object();
    w.field("levels", c.layering.w_levels);
    w.field("imbalance", c.layering.w_imbalance);
    w.field("coord", c.layering.w_coord);
    w.field("opposite", c.layering.w_opposite);
    w.field("unfilled", c.layering.w_unfilled);
    w.end_object();
    w.end_object();

    w.key("wash_trading");
    w.begin_object();
    w.field("enabled", c.wash.enabled);
    w.field("window_ms", msv(c.wash.window_ns));
    w.field("min_gross_qty", static_cast<std::int64_t>(c.wash.min_gross_qty));
    w.field("recip_target", c.wash.recip_target);
    w.field("share_target", c.wash.share_target);
    w.field("offmarket_ticks", c.wash.offmarket_ticks);
    w.field("breadth_penalty", c.wash.breadth_penalty);
    w.field("breadth_target", c.wash.breadth_target);
    w.field("min_score", c.wash.min_score);
    w.field("cooldown_ms", msv(c.wash.cooldown_ns));
    w.key("weights");
    w.begin_object();
    w.field("self", c.wash.w_self);
    w.field("recip", c.wash.w_recip);
    w.field("nonet", c.wash.w_nonet);
    w.field("share", c.wash.w_share);
    w.field("price", c.wash.w_price);
    w.end_object();
    w.end_object();

    w.key("momentum_ignition");
    w.begin_object();
    w.field("enabled", c.momentum.enabled);
    w.field("burst_ms", msv(c.momentum.burst_ns));
    w.field("reversal_ms", msv(c.momentum.reversal_ns));
    w.field("revert_ms", msv(c.momentum.revert_ns));
    w.field("min_move_ticks", c.momentum.min_move_ticks);
    w.field("move_target_ticks", c.momentum.move_target_ticks);
    w.field("min_orders", c.momentum.min_orders);
    w.field("burst_target_orders", c.momentum.burst_target_orders);
    w.field("share_target", c.momentum.share_target);
    w.field("min_share", c.momentum.min_share);
    w.field("min_reverse_fraction", c.momentum.min_reverse_fraction);
    w.field("min_score", c.momentum.min_score);
    w.field("cooldown_ms", msv(c.momentum.cooldown_ns));
    w.key("weights");
    w.begin_object();
    w.field("burst", c.momentum.w_burst);
    w.field("move", c.momentum.w_move);
    w.field("share", c.momentum.w_share);
    w.field("reverse", c.momentum.w_reverse);
    w.field("revert", c.momentum.w_revert);
    w.end_object();
    w.end_object();

    w.end_object();
    return out;
}

}  // namespace tapewatch
