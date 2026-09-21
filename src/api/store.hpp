// SQLite access for the alert and case store.
//
// A thin wrapper over the C API rather than an ORM: there are eleven queries
// in this file and all of them are visible.
//
// The schema is created by python/eval/store.py, which owns it. This side
// reads the derived tables and writes only the analyst-state ones
// (alert_state, cases, case_alerts), so re-running the evaluation refreshes
// the alerts without destroying somebody's triage work.

#pragma once

#include <sqlite3.h>

#include <mutex>
#include <string>
#include <vector>

namespace tapewatch::api {

struct AlertFilter {
    std::string detector;
    std::string symbol;
    std::string status;
    std::string outcome;
    long long participant{-1};
    double min_score{0.0};
    bool only_above_operating_point{false};
    bool hide_closed{false};
    std::string sort{"confidence"};  // confidence | detect_ts | score
    int limit{100};
    int offset{0};
};

class Store {
public:
    explicit Store(const std::string& path);
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    bool ok() const { return db_ != nullptr; }
    const std::string& error() const { return error_; }

    std::string summary_json();
    std::string alerts_json(const AlertFilter& f);
    std::string alert_json(long long alert_id);
    std::string participants_json();
    std::string cases_json();
    std::string case_json(long long case_id);

    bool set_alert_state(long long alert_id, const std::string& status,
                         const std::string& disposition, const std::string& note,
                         std::string& err);
    long long create_case(const std::string& title, const std::string& assignee,
                          const std::string& summary, const std::vector<long long>& alert_ids,
                          std::string& err);
    bool update_case(long long case_id, const std::string& status, const std::string& assignee,
                     const std::string& summary, std::string& err);
    bool attach_alerts(long long case_id, const std::vector<long long>& alert_ids,
                       std::string& err);

private:
    sqlite3* db_{nullptr};
    std::string error_;
    // One connection, one mutex. The server is eight threads and SQLite's
    // serialized mode would do this internally, but an explicit lock makes
    // the multi-statement case-creation path atomic too.
    std::mutex mu_;
};

}  // namespace tapewatch::api
