// What a detector produces.
//
// An alert is deliberately fat. It carries every component signal that went
// into its score, not just the total, for two reasons. An analyst reading the
// console needs to know *why* something fired before they can dismiss it in
// under a minute. And the evaluation harness re-derives the composite score in
// Python from these components, which is what makes threshold sweeps and
// weight ablations free -- no re-run of the C++ engine, no chance of the two
// implementations drifting apart, because there is only one implementation of
// the weighted sum that matters and it reads these numbers.

#pragma once

#include <string>
#include <vector>

#include "tapewatch/types.hpp"

namespace tapewatch {

struct SignalValue {
    std::string name;
    double value{0.0};   // always in [0, 1]
    double weight{0.0};  // the weight this run applied
};

struct Alert {
    std::uint64_t alert_id{0};
    DetectorKind detector{DetectorKind::Spoofing};
    std::string symbol;
    ParticipantId participant{0};

    // The behaviour window the alert describes.
    Ts start_ts{0};
    Ts end_ts{0};
    // The timestamp of the event that made the detector fire. Detection
    // latency is detect_ts - episode.start_ts, and it is a headline metric:
    // an alert that arrives forty minutes after the abuse is an audit trail,
    // not surveillance.
    Ts detect_ts{0};
    SeqNum seq_first{0};
    SeqNum seq_last{0};

    double score{0.0};
    // Subtracted from the weighted sum before thresholding. Kept separate
    // from the signal vector, and emitted, because it is not a weight: the
    // Python harness re-derives `score` as weighted_sum - penalty, and an
    // ablation that zeroes the penalty is a legitimate experiment.
    double penalty{0.0};
    // score scaled down when the window overlapped a damaged feed. The console
    // sorts on confidence; the harness scores on `score` so that feed quality
    // does not silently move the operating point.
    double confidence{0.0};
    bool degraded{false};

    std::vector<SignalValue> signals;

    // Free-form integer evidence, rendered in the console's detail pane.
    std::vector<std::pair<std::string, std::int64_t>> evidence;
    std::vector<OrderId> order_ids;
    // Second participant, where the pattern needs one (wash trading).
    ParticipantId counterparty{0};
    bool has_counterparty{false};

    void signal(const char* name, double value, double weight) {
        signals.push_back({name, clamp01(value), weight});
    }
    void note(const char* name, std::int64_t v) { evidence.emplace_back(name, v); }

    double weighted_score() const {
        double total = 0.0, wsum = 0.0;
        for (const auto& s : signals) {
            total += s.value * s.weight;
            wsum += s.weight;
        }
        // Normalising by the weight sum means a config whose weights do not
        // add to 1 still produces a score in [0, 1], instead of an operating
        // point that silently shifts when someone edits one weight.
        return wsum > 0.0 ? total / wsum : 0.0;
    }
};

// One JSON object, no newline. Written one per line to the alerts file.
std::string alert_to_json(const Alert& a);

}  // namespace tapewatch
