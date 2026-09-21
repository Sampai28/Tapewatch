// Book replay for the console.
//
// An analyst dismissing a spoofing alert needs to see the book, not a score.
// Was the order actually at the front? How deep was the other side? Did the
// spread move when it was pulled? None of that is in the alert; all of it is
// in the tape.
//
// The tape is loaded once at startup and kept in memory -- a few tens of
// megabytes for a session, and the alternative is re-reading a file on every
// keystroke. A request replays the book from the start of the tape up to the
// window it wants and then emits one frame per event. Replaying from the
// beginning sounds wasteful and is: applying a book event is a handful of map
// operations, so a full session replays in well under a second, and the
// alternative -- periodic snapshots -- is a caching layer nobody has asked
// for yet. If the tape ever gets long enough for that to hurt, the fix is
// snapshots every N events, and this comment is where to start.

#pragma once

#include <string>
#include <vector>

#include "tapewatch/event.hpp"
#include "tapewatch/types.hpp"

namespace tapewatch::api {

struct ReplayRequest {
    std::string symbol;
    Ts from_ts{0};
    Ts to_ts{0};
    std::size_t depth{8};
    std::size_t max_frames{400};
    // Orders belonging to the alert, highlighted in the returned frames so
    // the console can colour them without matching by price.
    std::vector<OrderId> highlight;
};

class Replay {
public:
    // Returns false and sets `error` when the tape cannot be read.
    bool load(const std::string& path, std::string& error);

    bool loaded() const { return loaded_; }
    std::size_t event_count() const { return events_.size(); }
    Ts first_ts() const { return events_.empty() ? 0 : events_.front().ts; }
    Ts last_ts() const { return events_.empty() ? 0 : events_.back().ts; }
    const std::vector<std::string>& symbols() const { return symbols_; }

    std::string frames_json(const ReplayRequest& req) const;

private:
    std::vector<MarketEvent> events_;
    std::vector<std::string> symbols_;
    bool loaded_{false};
};

}  // namespace tapewatch::api
