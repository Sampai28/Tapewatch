// tapewatch-detect -- run the detectors over a tape.
//
//   tapewatch-detect --input tape.csv --config detectors.json
//                    --alerts alerts.jsonl --manifest manifest.json
//
// Reads from a file or stdin, writes JSON Lines alerts and a manifest
// describing what the run actually did: the resolved config, the feed
// integrity counters, and the throughput. The manifest is the reason a number
// in the report can be traced back to the run that produced it.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "tapewatch/config.hpp"
#include "tapewatch/json.hpp"
#include "tapewatch/pipeline.hpp"

namespace {

using namespace tapewatch;

class FileAlertWriter final : public AlertWriter {
public:
    explicit FileAlertWriter(std::ostream& os) : os_(os) {}
    void write(const Alert& a) override { os_ << alert_to_json(a) << '\n'; }

private:
    std::ostream& os_;
};

void usage() {
    std::fprintf(stderr,
                 "usage: tapewatch-detect --input <tape.csv|-> [--config <detectors.json>]\n"
                 "                        [--alerts <alerts.jsonl>] [--manifest <manifest.json>]\n"
                 "                        [--quiet]\n");
}

std::string manifest_json(const EngineConfig& cfg, const RunStats& s, const ConfigLoad& loaded,
                          const std::string& input, Ts history_ns) {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field("input", input);
    w.field("events_admitted", s.feed.events_admitted);
    w.field("symbols", s.symbols);
    w.field("scans", s.scans);
    w.field("first_ts", static_cast<std::int64_t>(s.first_ts));
    w.field("last_ts", static_cast<std::int64_t>(s.last_ts));
    w.field("tape_span_seconds",
            static_cast<double>(s.tape_span()) / static_cast<double>(kNsPerSec));
    w.field("wall_seconds", s.wall_seconds);
    w.field("events_per_second",
            s.wall_seconds > 0 ? static_cast<double>(s.feed.events_admitted) / s.wall_seconds : 0.0);
    w.field("history_ns", static_cast<std::int64_t>(history_ns));
    w.field("window_overflows", s.window_overflows);

    w.field("alerts_emitted", s.alerts_emitted);
    w.field("alerts_suppressed_cooldown", s.alerts_suppressed_cooldown);
    w.field("alerts_degraded", s.alerts_degraded);
    w.key("alerts_by_detector");
    w.begin_object();
    for (int i = 0; i < static_cast<int>(DetectorKind::Count); ++i)
        w.field(detector_str(static_cast<DetectorKind>(i)), s.alerts_by_detector[i]);
    w.end_object();
    w.key("suppressed_by_detector");
    w.begin_object();
    for (int i = 0; i < static_cast<int>(DetectorKind::Count); ++i)
        w.field(detector_str(static_cast<DetectorKind>(i)), s.suppressed_by_detector[i]);
    w.end_object();

    w.key("book");
    w.begin_object();
    w.field("orders_added", s.book.orders_added);
    w.field("orders_dropped_cap", s.book.orders_dropped_cap);
    w.field("unknown_cancel", s.book.unknown_cancel);
    w.field("unknown_modify", s.book.unknown_modify);
    w.field("unknown_maker", s.book.unknown_maker);
    w.field("duplicate_order_id", s.book.duplicate_order_id);
    w.field("crossed_observations", s.book.crossed_observations);
    w.field("oversized_fill", s.book.oversized_fill);
    w.field("stale_levels_removed", s.book.stale_levels_removed);
    w.field("stale_orders_removed", s.book.stale_orders_removed);
    w.end_object();

    // Nested raw JSON: both of these are produced by the same writer, so
    // splicing the text avoids a second representation of the same data.
    out += ",\"feed\":";
    out += feed_stats_to_json(s.feed);
    out += ",\"config\":";
    out += config_to_json(cfg);

    out += ",\"config_unknown_keys\":[";
    for (std::size_t i = 0; i < loaded.unknown_keys.size(); ++i) {
        if (i) out += ',';
        out += '"';
        out += loaded.unknown_keys[i];
        out += '"';
    }
    out += "]}";
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string input, config_path, alerts_path, manifest_path;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "tapewatch-detect: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--input") input = next("--input");
        else if (a == "--config") config_path = next("--config");
        else if (a == "--alerts") alerts_path = next("--alerts");
        else if (a == "--manifest") manifest_path = next("--manifest");
        else if (a == "--quiet") quiet = true;
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "tapewatch-detect: unknown argument %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (input.empty()) {
        usage();
        return 2;
    }

    ConfigLoad loaded;
    if (!config_path.empty()) {
        loaded = load_config(config_path);
        if (!loaded.ok) {
            std::fprintf(stderr, "tapewatch-detect: %s\n", loaded.error.c_str());
            return 1;
        }
        for (const std::string& k : loaded.unknown_keys)
            std::fprintf(stderr, "tapewatch-detect: warning: config key not used: %s\n", k.c_str());
    }
    const EngineConfig& cfg = loaded.config;

    std::ifstream fin;
    std::istream* in = &std::cin;
    if (input != "-") {
        fin.open(input);
        if (!fin) {
            std::fprintf(stderr, "tapewatch-detect: cannot open %s\n", input.c_str());
            return 1;
        }
        in = &fin;
    }

    std::ofstream fout;
    std::ostream* alerts_os = &std::cout;
    if (!alerts_path.empty()) {
        fout.open(alerts_path);
        if (!fout) {
            std::fprintf(stderr, "tapewatch-detect: cannot write %s\n", alerts_path.c_str());
            return 1;
        }
        alerts_os = &fout;
    }

    FileAlertWriter writer(*alerts_os);
    Pipeline pipe(cfg, writer);

    const auto t0 = std::chrono::steady_clock::now();
    std::string line;
    while (std::getline(*in, line)) pipe.feed_line(line);
    pipe.finish();
    const auto t1 = std::chrono::steady_clock::now();

    RunStats s = pipe.stats();
    s.wall_seconds = std::chrono::duration<double>(t1 - t0).count();

    alerts_os->flush();

    const std::string manifest = manifest_json(cfg, s, loaded, input, pipe.history_ns());
    if (!manifest_path.empty()) {
        std::ofstream mf(manifest_path);
        if (!mf) {
            std::fprintf(stderr, "tapewatch-detect: cannot write %s\n", manifest_path.c_str());
            return 1;
        }
        mf << manifest << '\n';
    }

    if (!quiet) {
        std::fprintf(stderr,
                     "tapewatch-detect: %llu events, %llu alerts (%llu suppressed, %llu "
                     "degraded), %.2fs, %.0f ev/s\n",
                     (unsigned long long)s.feed.events_admitted,
                     (unsigned long long)s.alerts_emitted,
                     (unsigned long long)s.alerts_suppressed_cooldown,
                     (unsigned long long)s.alerts_degraded, s.wall_seconds,
                     s.wall_seconds > 0 ? s.feed.events_admitted / s.wall_seconds : 0.0);
        if (s.window_overflows > 0)
            std::fprintf(stderr,
                         "tapewatch-detect: warning: %llu window overflows -- raise "
                         "window_capacity or the windows were narrower than configured\n",
                         (unsigned long long)s.window_overflows);
    }
    return 0;
}
