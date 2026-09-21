#include "store.hpp"

#include <cstring>

#include "tapewatch/json.hpp"

namespace tapewatch::api {
namespace {

// Column values that are already JSON text in the database (signals,
// weights, evidence, order_ids) are spliced in raw rather than re-escaped as
// a string, so the client gets objects instead of strings containing JSON.
void splice(std::string& out, const char* key, const char* raw, const char* fallback) {
    out += ",\"";
    out += key;
    out += "\":";
    out += (raw && *raw) ? raw : fallback;
}

const char* text(sqlite3_stmt* st, int col) {
    const unsigned char* v = sqlite3_column_text(st, col);
    return v ? reinterpret_cast<const char*>(v) : "";
}

class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st_, nullptr) != SQLITE_OK) st_ = nullptr;
    }
    ~Stmt() {
        if (st_) sqlite3_finalize(st_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    sqlite3_stmt* get() const { return st_; }
    explicit operator bool() const { return st_ != nullptr; }

private:
    sqlite3_stmt* st_{nullptr};
};

}  // namespace

Store::Store(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        error_ = db_ ? sqlite3_errmsg(db_) : "cannot open database";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return;
    }
    sqlite3_busy_timeout(db_, 3000);
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
}

Store::~Store() {
    if (db_) sqlite3_close(db_);
}

std::string Store::summary_json() {
    std::lock_guard<std::mutex> lock(mu_);
    std::string out = "{\"meta\":{";
    Stmt meta(db_, "SELECT key, value FROM meta ORDER BY key");
    bool first = true;
    if (meta) {
        while (sqlite3_step(meta.get()) == SQLITE_ROW) {
            if (!first) out += ',';
            first = false;
            std::string k = text(meta.get(), 0);
            std::string v = text(meta.get(), 1);
            JsonWriter w(out);
            w.field(k, v);
        }
    }
    out += "},\"counts\":{";

    struct Q {
        const char* key;
        const char* sql;
    };
    const Q queries[] = {
        {"alerts", "SELECT count(*) FROM alerts"},
        {"alerts_above_operating_point",
         "SELECT count(*) FROM alerts WHERE above_operating_point=1"},
        {"episodes", "SELECT count(*) FROM episodes"},
        {"episodes_detected", "SELECT count(*) FROM episodes WHERE detected=1"},
        {"open", "SELECT count(*) FROM alerts a LEFT JOIN alert_state s USING(alert_id) "
                 "WHERE coalesce(s.status,'open')='open'"},
        {"cases", "SELECT count(*) FROM cases"},
    };
    first = true;
    for (const Q& q : queries) {
        Stmt st(db_, q.sql);
        long long n = 0;
        if (st && sqlite3_step(st.get()) == SQLITE_ROW) n = sqlite3_column_int64(st.get(), 0);
        if (!first) out += ',';
        first = false;
        out += '"';
        out += q.key;
        out += "\":";
        out += std::to_string(n);
    }
    out += "},\"by_detector\":[";

    Stmt by(db_,
            "SELECT detector, count(*), "
            "sum(outcome='true_positive'), sum(outcome='false_positive'), "
            "sum(above_operating_point) "
            "FROM alerts GROUP BY detector ORDER BY detector");
    first = true;
    if (by) {
        while (sqlite3_step(by.get()) == SQLITE_ROW) {
            if (!first) out += ',';
            first = false;
            std::string row;
            JsonWriter w(row);
            w.begin_object();
            w.field("detector", text(by.get(), 0));
            w.field("alerts", static_cast<std::int64_t>(sqlite3_column_int64(by.get(), 1)));
            w.field("true_positives",
                    static_cast<std::int64_t>(sqlite3_column_int64(by.get(), 2)));
            w.field("false_positives",
                    static_cast<std::int64_t>(sqlite3_column_int64(by.get(), 3)));
            w.field("above_operating_point",
                    static_cast<std::int64_t>(sqlite3_column_int64(by.get(), 4)));
            w.end_object();
            out += row;
        }
    }
    out += "]}";
    return out;
}

std::string Store::alerts_json(const AlertFilter& f) {
    std::lock_guard<std::mutex> lock(mu_);

    std::string sql =
        "SELECT a.alert_id, a.detector, a.symbol, a.participant, a.counterparty, "
        "a.start_ts, a.end_ts, a.detect_ts, a.score, a.confidence, a.degraded, "
        "a.outcome, a.role, a.above_operating_point, a.episode_id, a.evidence, "
        "coalesce(s.status,'open'), coalesce(s.disposition,''), "
        "(SELECT count(*) FROM case_alerts ca WHERE ca.alert_id = a.alert_id) "
        "FROM alerts a LEFT JOIN alert_state s USING(alert_id) WHERE a.score >= ?";
    if (!f.detector.empty()) sql += " AND a.detector = ?";
    if (!f.symbol.empty()) sql += " AND a.symbol = ?";
    if (!f.outcome.empty()) sql += " AND a.outcome = ?";
    if (!f.status.empty()) sql += " AND coalesce(s.status,'open') = ?";
    if (f.participant >= 0) sql += " AND (a.participant = ? OR a.counterparty = ?)";
    if (f.only_above_operating_point) sql += " AND a.above_operating_point = 1";
    if (f.hide_closed) sql += " AND coalesce(s.status,'open') <> 'closed'";

    if (f.sort == "detect_ts") sql += " ORDER BY a.detect_ts ASC";
    else if (f.sort == "score") sql += " ORDER BY a.score DESC";
    else sql += " ORDER BY a.confidence DESC, a.detect_ts ASC";
    sql += " LIMIT ? OFFSET ?";

    Stmt st(db_, sql);
    if (!st) return "{\"error\":\"query failed\",\"alerts\":[]}";

    int i = 1;
    sqlite3_bind_double(st.get(), i++, f.min_score);
    if (!f.detector.empty())
        sqlite3_bind_text(st.get(), i++, f.detector.c_str(), -1, SQLITE_TRANSIENT);
    if (!f.symbol.empty())
        sqlite3_bind_text(st.get(), i++, f.symbol.c_str(), -1, SQLITE_TRANSIENT);
    if (!f.outcome.empty())
        sqlite3_bind_text(st.get(), i++, f.outcome.c_str(), -1, SQLITE_TRANSIENT);
    if (!f.status.empty())
        sqlite3_bind_text(st.get(), i++, f.status.c_str(), -1, SQLITE_TRANSIENT);
    if (f.participant >= 0) {
        sqlite3_bind_int64(st.get(), i++, f.participant);
        sqlite3_bind_int64(st.get(), i++, f.participant);
    }
    sqlite3_bind_int(st.get(), i++, f.limit);
    sqlite3_bind_int(st.get(), i++, f.offset);

    std::string out = "{\"alerts\":[";
    bool first = true;
    while (sqlite3_step(st.get()) == SQLITE_ROW) {
        if (!first) out += ',';
        first = false;
        std::string row;
        JsonWriter w(row);
        w.begin_object();
        w.field("alert_id", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 0)));
        w.field("detector", text(st.get(), 1));
        w.field("symbol", text(st.get(), 2));
        w.field("participant", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 3)));
        if (sqlite3_column_type(st.get(), 4) == SQLITE_NULL) {
            w.key("counterparty");
            w.null();
        } else {
            w.field("counterparty", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 4)));
        }
        w.field("start_ts", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 5)));
        w.field("end_ts", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 6)));
        w.field("detect_ts", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 7)));
        w.field("score", sqlite3_column_double(st.get(), 8));
        w.field("confidence", sqlite3_column_double(st.get(), 9));
        w.field("degraded", sqlite3_column_int(st.get(), 10) != 0);
        w.field("outcome", text(st.get(), 11));
        w.field("role", text(st.get(), 12));
        w.field("above_operating_point", sqlite3_column_int(st.get(), 13) != 0);
        if (sqlite3_column_type(st.get(), 14) == SQLITE_NULL) {
            w.key("episode_id");
            w.null();
        } else {
            w.field("episode_id", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 14)));
        }
        w.field("status", text(st.get(), 16));
        w.field("disposition", text(st.get(), 17));
        w.field("case_count", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 18)));
        w.end_object();
        row.pop_back();  // reopen the object to splice raw JSON columns
        splice(row, "evidence", text(st.get(), 15), "{}");
        row += '}';
        out += row;
    }
    out += "]}";
    return out;
}

std::string Store::alert_json(long long alert_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT a.alert_id, a.detector, a.symbol, a.participant, a.counterparty, "
            "a.start_ts, a.end_ts, a.detect_ts, a.score, a.penalty, a.confidence, a.degraded, "
            "a.signals, a.weights, a.evidence, a.order_ids, a.outcome, a.role, "
            "a.above_operating_point, a.episode_id, "
            "coalesce(s.status,'open'), coalesce(s.disposition,''), coalesce(s.note,'') "
            "FROM alerts a LEFT JOIN alert_state s USING(alert_id) WHERE a.alert_id = ?");
    if (!st) return "";
    sqlite3_bind_int64(st.get(), 1, alert_id);
    if (sqlite3_step(st.get()) != SQLITE_ROW) return "";

    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field("alert_id", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 0)));
    w.field("detector", text(st.get(), 1));
    w.field("symbol", text(st.get(), 2));
    w.field("participant", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 3)));
    if (sqlite3_column_type(st.get(), 4) == SQLITE_NULL) {
        w.key("counterparty");
        w.null();
    } else {
        w.field("counterparty", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 4)));
    }
    w.field("start_ts", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 5)));
    w.field("end_ts", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 6)));
    w.field("detect_ts", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 7)));
    w.field("score", sqlite3_column_double(st.get(), 8));
    w.field("penalty", sqlite3_column_double(st.get(), 9));
    w.field("confidence", sqlite3_column_double(st.get(), 10));
    w.field("degraded", sqlite3_column_int(st.get(), 11) != 0);
    w.field("outcome", text(st.get(), 16));
    w.field("role", text(st.get(), 17));
    w.field("above_operating_point", sqlite3_column_int(st.get(), 18) != 0);
    w.field("status", text(st.get(), 20));
    w.field("disposition", text(st.get(), 21));
    w.field("note", text(st.get(), 22));
    const long long episode_id =
        sqlite3_column_type(st.get(), 19) == SQLITE_NULL ? -1 : sqlite3_column_int64(st.get(), 19);
    w.end_object();
    out.pop_back();
    splice(out, "signals", text(st.get(), 12), "{}");
    splice(out, "weights", text(st.get(), 13), "{}");
    splice(out, "evidence", text(st.get(), 14), "{}");
    splice(out, "order_ids", text(st.get(), 15), "[]");

    out += ",\"episode\":";
    if (episode_id < 0) {
        out += "null";
    } else {
        Stmt ep(db_,
                "SELECT episode_id, kind, participant, counterparty, start_ts, end_ts, "
                "intensity, detected, latency_ms, notes FROM episodes WHERE episode_id = ?");
        if (ep) sqlite3_bind_int64(ep.get(), 1, episode_id);
        if (ep && sqlite3_step(ep.get()) == SQLITE_ROW) {
            std::string e;
            JsonWriter ew(e);
            ew.begin_object();
            ew.field("episode_id", static_cast<std::int64_t>(sqlite3_column_int64(ep.get(), 0)));
            ew.field("kind", text(ep.get(), 1));
            ew.field("participant",
                     static_cast<std::int64_t>(sqlite3_column_int64(ep.get(), 2)));
            ew.field("start_ts", static_cast<std::int64_t>(sqlite3_column_int64(ep.get(), 4)));
            ew.field("end_ts", static_cast<std::int64_t>(sqlite3_column_int64(ep.get(), 5)));
            ew.field("intensity", sqlite3_column_double(ep.get(), 6));
            ew.field("detected", sqlite3_column_int(ep.get(), 7) != 0);
            ew.field("latency_ms",
                     static_cast<std::int64_t>(sqlite3_column_int64(ep.get(), 8)));
            ew.end_object();
            e.pop_back();
            splice(e, "notes", text(ep.get(), 9), "{}");
            e += '}';
            out += e;
        } else {
            out += "null";
        }
    }

    out += ",\"cases\":[";
    Stmt cs(db_,
            "SELECT c.case_id, c.title, c.status FROM cases c JOIN case_alerts ca "
            "USING(case_id) WHERE ca.alert_id = ? ORDER BY c.case_id");
    bool first = true;
    if (cs) {
        sqlite3_bind_int64(cs.get(), 1, alert_id);
        while (sqlite3_step(cs.get()) == SQLITE_ROW) {
            if (!first) out += ',';
            first = false;
            std::string c;
            JsonWriter cw(c);
            cw.begin_object();
            cw.field("case_id", static_cast<std::int64_t>(sqlite3_column_int64(cs.get(), 0)));
            cw.field("title", text(cs.get(), 1));
            cw.field("status", text(cs.get(), 2));
            cw.end_object();
            out += c;
        }
    }
    out += "]}";
    return out;
}

std::string Store::participants_json() {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT p.participant, p.role, "
            "(SELECT count(*) FROM alerts a WHERE a.participant = p.participant) "
            "FROM participants p ORDER BY p.participant");
    std::string out = "{\"participants\":[";
    bool first = true;
    if (st) {
        while (sqlite3_step(st.get()) == SQLITE_ROW) {
            if (!first) out += ',';
            first = false;
            std::string row;
            JsonWriter w(row);
            w.begin_object();
            w.field("participant", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 0)));
            w.field("role", text(st.get(), 1));
            w.field("alerts", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 2)));
            w.end_object();
            out += row;
        }
    }
    out += "]}";
    return out;
}

std::string Store::cases_json() {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT c.case_id, c.title, c.status, coalesce(c.assignee,''), "
            "coalesce(c.summary,''), c.created_ts, c.updated_ts, "
            "(SELECT count(*) FROM case_alerts ca WHERE ca.case_id = c.case_id) "
            "FROM cases c ORDER BY c.case_id DESC");
    std::string out = "{\"cases\":[";
    bool first = true;
    if (st) {
        while (sqlite3_step(st.get()) == SQLITE_ROW) {
            if (!first) out += ',';
            first = false;
            std::string row;
            JsonWriter w(row);
            w.begin_object();
            w.field("case_id", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 0)));
            w.field("title", text(st.get(), 1));
            w.field("status", text(st.get(), 2));
            w.field("assignee", text(st.get(), 3));
            w.field("summary", text(st.get(), 4));
            w.field("created_ts", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 5)));
            w.field("updated_ts", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 6)));
            w.field("alert_count", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 7)));
            w.end_object();
            out += row;
        }
    }
    out += "]}";
    return out;
}

std::string Store::case_json(long long case_id) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "SELECT case_id, title, status, coalesce(assignee,''), coalesce(summary,''), "
            "created_ts, updated_ts FROM cases WHERE case_id = ?");
    if (!st) return "";
    sqlite3_bind_int64(st.get(), 1, case_id);
    if (sqlite3_step(st.get()) != SQLITE_ROW) return "";

    std::string out;
    JsonWriter w(out);
    w.begin_object();
    w.field("case_id", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 0)));
    w.field("title", text(st.get(), 1));
    w.field("status", text(st.get(), 2));
    w.field("assignee", text(st.get(), 3));
    w.field("summary", text(st.get(), 4));
    w.field("created_ts", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 5)));
    w.field("updated_ts", static_cast<std::int64_t>(sqlite3_column_int64(st.get(), 6)));
    w.end_object();
    out.pop_back();

    out += ",\"alerts\":[";
    Stmt al(db_,
            "SELECT a.alert_id, a.detector, a.participant, a.score, a.confidence, a.outcome, "
            "a.detect_ts FROM alerts a JOIN case_alerts ca USING(alert_id) "
            "WHERE ca.case_id = ? ORDER BY a.detect_ts");
    bool first = true;
    if (al) {
        sqlite3_bind_int64(al.get(), 1, case_id);
        while (sqlite3_step(al.get()) == SQLITE_ROW) {
            if (!first) out += ',';
            first = false;
            std::string row;
            JsonWriter rw(row);
            rw.begin_object();
            rw.field("alert_id", static_cast<std::int64_t>(sqlite3_column_int64(al.get(), 0)));
            rw.field("detector", text(al.get(), 1));
            rw.field("participant",
                     static_cast<std::int64_t>(sqlite3_column_int64(al.get(), 2)));
            rw.field("score", sqlite3_column_double(al.get(), 3));
            rw.field("confidence", sqlite3_column_double(al.get(), 4));
            rw.field("outcome", text(al.get(), 5));
            rw.field("detect_ts", static_cast<std::int64_t>(sqlite3_column_int64(al.get(), 6)));
            rw.end_object();
            out += row;
        }
    }
    out += "]}";
    return out;
}

bool Store::set_alert_state(long long alert_id, const std::string& status,
                            const std::string& disposition, const std::string& note,
                            std::string& err) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "INSERT INTO alert_state(alert_id, status, disposition, note, updated_ts) "
            "VALUES (?,?,?,?,strftime('%s','now')) "
            "ON CONFLICT(alert_id) DO UPDATE SET status=excluded.status, "
            "disposition=excluded.disposition, note=excluded.note, "
            "updated_ts=excluded.updated_ts");
    if (!st) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st.get(), 1, alert_id);
    sqlite3_bind_text(st.get(), 2, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.get(), 3, disposition.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.get(), 4, note.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.get()) != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    return true;
}

long long Store::create_case(const std::string& title, const std::string& assignee,
                             const std::string& summary,
                             const std::vector<long long>& alert_ids, std::string& err) {
    std::lock_guard<std::mutex> lock(mu_);
    sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    Stmt st(db_,
            "INSERT INTO cases(title, status, assignee, summary, created_ts, updated_ts) "
            "VALUES (?, 'open', ?, ?, strftime('%s','now'), strftime('%s','now'))");
    if (!st) {
        err = sqlite3_errmsg(db_);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    }
    sqlite3_bind_text(st.get(), 1, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.get(), 2, assignee.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.get(), 3, summary.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.get()) != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return -1;
    }
    const long long case_id = sqlite3_last_insert_rowid(db_);

    Stmt link(db_, "INSERT OR IGNORE INTO case_alerts(case_id, alert_id) VALUES (?, ?)");
    if (link) {
        for (long long id : alert_ids) {
            sqlite3_reset(link.get());
            sqlite3_bind_int64(link.get(), 1, case_id);
            sqlite3_bind_int64(link.get(), 2, id);
            sqlite3_step(link.get());
        }
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return case_id;
}

bool Store::update_case(long long case_id, const std::string& status,
                        const std::string& assignee, const std::string& summary,
                        std::string& err) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_,
            "UPDATE cases SET status = coalesce(nullif(?,''), status), "
            "assignee = coalesce(nullif(?,''), assignee), "
            "summary = coalesce(nullif(?,''), summary), "
            "updated_ts = strftime('%s','now') WHERE case_id = ?");
    if (!st) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st.get(), 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.get(), 2, assignee.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.get(), 3, summary.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.get(), 4, case_id);
    if (sqlite3_step(st.get()) != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    return sqlite3_changes(db_) > 0;
}

bool Store::attach_alerts(long long case_id, const std::vector<long long>& alert_ids,
                          std::string& err) {
    std::lock_guard<std::mutex> lock(mu_);
    Stmt st(db_, "INSERT OR IGNORE INTO case_alerts(case_id, alert_id) VALUES (?, ?)");
    if (!st) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    for (long long id : alert_ids) {
        sqlite3_reset(st.get());
        sqlite3_bind_int64(st.get(), 1, case_id);
        sqlite3_bind_int64(st.get(), 2, id);
        if (sqlite3_step(st.get()) != SQLITE_DONE) {
            err = sqlite3_errmsg(db_);
            return false;
        }
    }
    return true;
}

}  // namespace tapewatch::api
