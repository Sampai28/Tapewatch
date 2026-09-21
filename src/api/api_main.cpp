// tapewatch-api -- the console's back end.
//
//   tapewatch-api --db results/small/tapewatch.db --tape results/small/tape.csv
//                 --host 0.0.0.0 --port 8090
//
// A read-mostly REST API over the alert store, plus book replay from the
// tape. Everything is JSON; there is no HTML here and no templating. The
// console is a separate static bundle served by nginx in the compose stack,
// which keeps the back end to one job.
//
// Errors use RFC 7807 problem details rather than a bespoke shape, because
// the front end needs exactly one error-handling path and "sometimes a
// string, sometimes an object" is how you end up with three.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <httplib.h>

#include "replay.hpp"
#include "store.hpp"
#include "tapewatch/json.hpp"

namespace {

using namespace tapewatch;
using namespace tapewatch::api;

std::string problem(int status, const std::string& title, const std::string& detail) {
    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field("type", "about:blank");
    w.field("title", title);
    w.field("status", status);
    w.field("detail", detail);
    w.end_object();
    return out;
}

void fail(httplib::Response& res, int status, const std::string& title,
          const std::string& detail) {
    res.status = status;
    res.set_content(problem(status, title, detail), "application/problem+json");
}

std::string param(const httplib::Request& req, const char* key, const std::string& fallback = "") {
    auto it = req.params.find(key);
    return it == req.params.end() ? fallback : it->second;
}

long long param_int(const httplib::Request& req, const char* key, long long fallback) {
    const std::string v = param(req, key);
    if (v.empty()) return fallback;
    try {
        return std::stoll(v);
    } catch (...) {
        return fallback;
    }
}

double param_double(const httplib::Request& req, const char* key, double fallback) {
    const std::string v = param(req, key);
    if (v.empty()) return fallback;
    try {
        return std::stod(v);
    } catch (...) {
        return fallback;
    }
}

std::vector<long long> int_array(const JsonValue& v, const char* key) {
    std::vector<long long> out;
    const JsonValue* arr = v.get(key);
    if (!arr || !arr->is_array()) return out;
    for (const auto& item : arr->arr)
        if (item.is_number()) out.push_back(static_cast<long long>(item.num));
    return out;
}

bool body_json(const httplib::Request& req, httplib::Response& res, JsonValue& out) {
    std::string err;
    if (!json_parse(req.body, out, err) || !out.is_object()) {
        fail(res, 400, "Malformed request body", err.empty() ? "expected a JSON object" : err);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string db_path = "results/small/tapewatch.db";
    std::string tape_path = "results/small/tape.csv";
    std::string host = "0.0.0.0";
    int port = 8090;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "tapewatch-api: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--db") db_path = next();
        else if (a == "--tape") tape_path = next();
        else if (a == "--host") host = next();
        else if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "-h" || a == "--help") {
            std::fprintf(stderr,
                         "usage: tapewatch-api --db <file.db> --tape <tape.csv> "
                         "[--host H] [--port P]\n");
            return 0;
        } else {
            std::fprintf(stderr, "tapewatch-api: unknown argument %s\n", a.c_str());
            return 2;
        }
    }

    Store store(db_path);
    if (!store.ok()) {
        std::fprintf(stderr, "tapewatch-api: %s (%s)\n", store.error().c_str(), db_path.c_str());
        std::fprintf(stderr, "tapewatch-api: run `make eval` first -- the store is built by "
                             "the evaluation step.\n");
        return 1;
    }

    Replay replay;
    std::string replay_error;
    if (!replay.load(tape_path, replay_error)) {
        // Not fatal. Triage works without book replay; it is worse, and the
        // console says so, but refusing to start would be worse still.
        std::fprintf(stderr, "tapewatch-api: %s -- replay disabled\n", replay_error.c_str());
    } else if (!replay_error.empty()) {
        std::fprintf(stderr, "tapewatch-api: %s\n", replay_error.c_str());
    }

    httplib::Server server;
    server.new_task_queue = [] { return new httplib::ThreadPool(8); };

    server.set_post_routing_handler([](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    });
    server.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    server.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
        std::string out;
        JsonWriter w(out);
        w.begin_object();
        w.field("status", "ok");
        w.field("database", db_path);
        w.field("tape", tape_path);
        w.field("replay_available", replay.loaded());
        w.field("tape_events", static_cast<std::int64_t>(replay.event_count()));
        w.end_object();
        res.set_content(out, "application/json");
    });

    server.Get("/api/summary", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(store.summary_json(), "application/json");
    });

    server.Get("/api/participants", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(store.participants_json(), "application/json");
    });

    server.Get("/api/alerts", [&](const httplib::Request& req, httplib::Response& res) {
        AlertFilter f;
        f.detector = param(req, "detector");
        f.symbol = param(req, "symbol");
        f.status = param(req, "status");
        f.outcome = param(req, "outcome");
        f.participant = param_int(req, "participant", -1);
        f.min_score = param_double(req, "min_score", 0.0);
        f.only_above_operating_point = param(req, "tuned") == "1";
        f.hide_closed = param(req, "hide_closed") == "1";
        f.sort = param(req, "sort", "confidence");
        f.limit = static_cast<int>(param_int(req, "limit", 100));
        f.offset = static_cast<int>(param_int(req, "offset", 0));
        if (f.limit < 1) f.limit = 1;
        if (f.limit > 1000) f.limit = 1000;   // a page, not a dump
        if (f.offset < 0) f.offset = 0;
        res.set_content(store.alerts_json(f), "application/json");
    });

    server.Get(R"(/api/alerts/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        const long long id = std::stoll(req.matches[1]);
        const std::string body = store.alert_json(id);
        if (body.empty()) {
            fail(res, 404, "No such alert", "alert " + std::to_string(id) + " is not in the store");
            return;
        }
        res.set_content(body, "application/json");
    });

    server.Post(R"(/api/alerts/(\d+)/state)",
                [&](const httplib::Request& req, httplib::Response& res) {
                    const long long id = std::stoll(req.matches[1]);
                    JsonValue body;
                    if (!body_json(req, res, body)) return;
                    const std::string status = body.str_or("status", "open");
                    if (status != "open" && status != "reviewing" && status != "closed") {
                        fail(res, 422, "Invalid status",
                             "status must be one of open, reviewing, closed");
                        return;
                    }
                    std::string err;
                    if (!store.set_alert_state(id, status, body.str_or("disposition", ""),
                                               body.str_or("note", ""), err)) {
                        fail(res, 500, "Could not update alert", err);
                        return;
                    }
                    res.set_content(store.alert_json(id), "application/json");
                });

    server.Get("/api/cases", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(store.cases_json(), "application/json");
    });

    server.Post("/api/cases", [&](const httplib::Request& req, httplib::Response& res) {
        JsonValue body;
        if (!body_json(req, res, body)) return;
        const std::string title = body.str_or("title", "");
        if (title.empty()) {
            fail(res, 422, "Missing title", "a case needs a title");
            return;
        }
        std::string err;
        const long long id = store.create_case(title, body.str_or("assignee", ""),
                                               body.str_or("summary", ""),
                                               int_array(body, "alert_ids"), err);
        if (id < 0) {
            fail(res, 500, "Could not create case", err);
            return;
        }
        res.status = 201;
        res.set_content(store.case_json(id), "application/json");
    });

    server.Get(R"(/api/cases/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        const long long id = std::stoll(req.matches[1]);
        const std::string body = store.case_json(id);
        if (body.empty()) {
            fail(res, 404, "No such case", "case " + std::to_string(id) + " is not in the store");
            return;
        }
        res.set_content(body, "application/json");
    });

    server.Post(R"(/api/cases/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        const long long id = std::stoll(req.matches[1]);
        JsonValue body;
        if (!body_json(req, res, body)) return;
        std::string err;
        if (!store.update_case(id, body.str_or("status", ""), body.str_or("assignee", ""),
                               body.str_or("summary", ""), err)) {
            fail(res, err.empty() ? 404 : 500, err.empty() ? "No such case" : "Update failed",
                 err.empty() ? "case " + std::to_string(id) + " is not in the store" : err);
            return;
        }
        const std::vector<long long> ids = int_array(body, "alert_ids");
        if (!ids.empty() && !store.attach_alerts(id, ids, err)) {
            fail(res, 500, "Could not attach alerts", err);
            return;
        }
        res.set_content(store.case_json(id), "application/json");
    });

    server.Get("/api/replay", [&](const httplib::Request& req, httplib::Response& res) {
        if (!replay.loaded()) {
            fail(res, 503, "Replay unavailable", "the tape could not be read at startup");
            return;
        }
        ReplayRequest r;
        r.symbol = param(req, "symbol");
        r.from_ts = param_int(req, "from_ts", 0);
        r.to_ts = param_int(req, "to_ts", 0);
        r.depth = static_cast<std::size_t>(param_int(req, "depth", 8));
        r.max_frames = static_cast<std::size_t>(param_int(req, "max_frames", 400));
        if (r.symbol.empty() && !replay.symbols().empty()) r.symbol = replay.symbols().front();
        if (r.to_ts <= r.from_ts) {
            fail(res, 422, "Invalid window", "to_ts must be greater than from_ts");
            return;
        }
        if (r.depth < 1) r.depth = 1;
        if (r.depth > 40) r.depth = 40;
        if (r.max_frames < 1) r.max_frames = 1;
        if (r.max_frames > 2000) r.max_frames = 2000;

        const std::string ids = param(req, "order_ids");
        std::size_t start = 0;
        while (start < ids.size()) {
            const std::size_t comma = ids.find(',', start);
            const std::string piece = ids.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!piece.empty()) {
                try {
                    r.highlight.push_back(std::stoull(piece));
                } catch (...) {
                }
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }

        const auto t0 = std::chrono::steady_clock::now();
        std::string body = replay.frames_json(r);
        const auto ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        res.set_header("X-Replay-Ms", std::to_string(ms));
        res.set_content(body, "application/json");
    });

    server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
            std::string what = "unknown";
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
            }
            res.status = 500;
            res.set_content(problem(500, "Internal error", what), "application/problem+json");
        });

    std::fprintf(stderr, "tapewatch-api: %s, %zu tape events, listening on %s:%d\n",
                 db_path.c_str(), replay.event_count(), host.c_str(), port);
    if (!server.listen(host.c_str(), port)) {
        std::fprintf(stderr, "tapewatch-api: could not bind %s:%d\n", host.c_str(), port);
        return 1;
    }
    return 0;
}
