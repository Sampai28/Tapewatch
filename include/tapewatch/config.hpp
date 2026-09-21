// Detector configuration.
//
// Everything a detector can be tuned on lives here and is loaded from JSON.
// Nothing is a magic number in the detector source, because every one of these
// values gets swept in python/eval/sweep.py and the sweep has to be able to
// reach them.
//
// One thing is deliberately NOT here: the list of registered market makers.
// The engine is never told who the market makers are. If it were, the
// market-maker false positive rate -- the number this project exists to
// report honestly -- would be measuring a lookup table instead of a detector.
// The two-sided-quoting defence in the spoofing detector infers market making
// from behaviour on the tape. The MM roster lives in the generator's ground
// truth and is read only by the evaluation harness.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "tapewatch/types.hpp"

namespace tapewatch {

struct SpoofingConfig {
    bool enabled{true};
    Ts opposite_window_ns{3000 * kNsPerMs};  // look this far either side of the cancel
    Ts max_lifetime_ns{2000 * kNsPerMs};     // a pull faster than this scores 1.0
    double size_depth_multiple{3.0};         // order qty / mean resting order qty
    int near_touch_ticks{5};
    Qty min_qty{20};                         // below this an order cannot move anyone
    double min_score{0.30};
    Ts cooldown_ns{5000 * kNsPerMs};
    double w_size{0.28};
    double w_life{0.18};
    double w_unfilled{0.12};
    double w_opposite{0.30};
    double w_press{0.12};
    // Subtracted from the composite when the participant was quoting both
    // sides across the window. Large on purpose: at 0.35 a market maker
    // running the same shape as a spoofer at ordinary size still cleared the
    // threshold, so this is the setting that buys most of the false-positive
    // reduction, and it is the one to lower first if recall matters more.
    // See docs/detectors.md.
    double two_sided_penalty{0.45};
};

struct LayeringConfig {
    bool enabled{true};
    Ts placement_window_ns{4000 * kNsPerMs};
    Ts cancel_span_ns{1500 * kNsPerMs};
    Ts opposite_window_ns{4000 * kNsPerMs};
    int min_levels{3};
    int min_orders{3};
    int near_touch_ticks{8};
    double imbalance_target{0.55};  // participant share of side depth that scores 1.0
    double min_score{0.30};
    Ts cooldown_ns{6000 * kNsPerMs};
    double w_levels{0.24};
    double w_imbalance{0.24};
    double w_coord{0.18};
    double w_opposite{0.22};
    double w_unfilled{0.12};
    double two_sided_penalty{0.30};
};

struct WashConfig {
    bool enabled{true};
    Ts window_ns{30000 * kNsPerMs};
    Qty min_gross_qty{40};
    double recip_target{0.70};  // min(AB,BA)/max(AB,BA) that scores 1.0
    // How much of the busier party's volume went through this one
    // counterparty. A wash pair is near 1; a market maker's biggest single
    // relationship is a small fraction of its book.
    double share_target{0.60};
    int offmarket_ticks{4};
    // Counterparty breadth defence, the wash-trade analogue of the two-sided
    // quoting penalty in spoofing. Trading with many distinct counterparties
    // in the window is market making; trading with one is not.
    double breadth_penalty{0.45};
    double breadth_target{4.0};
    double min_score{0.30};
    Ts cooldown_ns{10000 * kNsPerMs};
    double w_self{0.34};
    double w_recip{0.26};
    double w_nonet{0.16};
    double w_share{0.16};
    double w_price{0.08};
};

struct MomentumConfig {
    bool enabled{true};
    Ts burst_ns{2000 * kNsPerMs};
    Ts reversal_ns{6000 * kNsPerMs};
    Ts revert_ns{8000 * kNsPerMs};
    int min_move_ticks{4};
    int move_target_ticks{10};
    // Distinct aggressive *orders*, not fills. One marketable order sweeping
    // five price levels prints five trades; counting fills made "a burst of
    // four" the description of a single ordinary market order, and every
    // uninformed taker on the venue cleared the bar.
    int min_orders{4};
    int burst_target_orders{10};
    double share_target{0.60};
    // Hard gates. An informed trader who was simply right keeps the position
    // and takes a modest share; without floors on both, this detector spends
    // its whole budget on people who were correct about the price.
    double min_share{0.35};
    double min_reverse_fraction{0.40};
    double min_score{0.30};
    Ts cooldown_ns{8000 * kNsPerMs};
    double w_burst{0.18};
    double w_move{0.22};
    double w_share{0.18};
    double w_reverse{0.26};
    double w_revert{0.16};
};

struct EngineConfig {
    Price tick_size{1};
    std::size_t max_resting_orders{200000};
    std::size_t window_capacity{4096};
    // How long after a sequence gap or a clock anomaly alerts stay marked
    // degraded, and how much their confidence is cut while they are.
    Ts feed_degrade_ns{5000 * kNsPerMs};
    double degrade_factor{0.60};
    // Out-of-order events inside this tolerance are accepted and applied;
    // beyond it they are dropped as unusable.
    Ts reorder_tolerance_ns{50 * kNsPerMs};
    // How often, in tape time, detectors are asked to look at what is new.
    // This is the floor on detection latency, and it is reported as such.
    Ts scan_interval_ns{500 * kNsPerMs};

    SpoofingConfig spoofing;
    LayeringConfig layering;
    WashConfig wash;
    MomentumConfig momentum;
};

struct ConfigLoad {
    EngineConfig config;
    bool ok{true};
    std::string error;
    // Keys present in the file that nothing reads. A silently ignored typo in
    // a threshold is a run that measured something other than what was asked
    // for, so these are printed and written into the run manifest.
    std::vector<std::string> unknown_keys;
};

ConfigLoad load_config(const std::string& path);
ConfigLoad parse_config(const std::string& text);
std::string config_to_json(const EngineConfig& c);

}  // namespace tapewatch
