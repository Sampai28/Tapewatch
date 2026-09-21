#include "tape_builder.hpp"
#include "test_harness.hpp"

using namespace tapewatch;
using namespace twtest;

namespace {

// The spoof from test_spoofing, factored out so that the pipeline tests can
// vary one thing at a time around a scenario known to fire.
void spoof(Tape& t, Ts base_ms, OrderId oid) {
    t.at(base_ms).order(50, oid, Side::Buy, 100, 200);
    t.at(base_ms + 300).cancel(50, oid);
    t.at(base_ms + 400).trade(50, oid + 1, 1, 101, Side::Sell, 99, 20);
}

void thin_book(Tape& t) {
    t.at(1000);
    t.order(1, 101, Side::Buy, 99, 2000);
    t.order(1, 102, Side::Buy, 98, 20);
    t.order(2, 201, Side::Sell, 103, 20);
}

}  // namespace

TW_TEST(pipeline_assigns_increasing_alert_ids) {
    Tape t;
    thin_book(t);
    spoof(t, 1100, 500);
    spoof(t, 40000, 600);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK(w.alerts.size() >= 2);
    for (std::size_t i = 1; i < w.alerts.size(); ++i)
        TW_CHECK(w.alerts[i].alert_id > w.alerts[i - 1].alert_id);
}

TW_TEST(pipeline_cooldown_collapses_a_repeated_pattern) {
    Tape t;
    thin_book(t);
    for (int i = 0; i < 6; ++i) spoof(t, 1100 + i * 600, 500 + i * 10);

    EngineConfig cfg = test_config();
    cfg.spoofing.cooldown_ns = 10'000 * kNsPerMs;  // longer than the whole tape

    CollectingWriter w;
    Pipeline p(cfg, w);
    t.run(p);

    TW_CHECK_EQ(w.count(DetectorKind::Spoofing), static_cast<std::size_t>(1));
    TW_CHECK(p.stats().alerts_suppressed_cooldown >= 1);
    // Suppressed alerts are counted, not forgotten. Alerts per analyst hour
    // is a headline metric and it has to know what it dropped.
    TW_CHECK_EQ(p.stats().suppressed_by_detector[static_cast<int>(DetectorKind::Spoofing)],
                p.stats().alerts_suppressed_cooldown);
}

TW_TEST(pipeline_marks_alerts_raised_over_a_damaged_feed) {
    Tape t;
    thin_book(t);
    t.skip_seq(500);  // a gap right before the behaviour of interest
    spoof(t, 1100, 500);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);

    TW_CHECK_EQ(p.stats().feed.sequence_gaps, static_cast<std::uint64_t>(1));
    const Alert* a = w.first(DetectorKind::Spoofing);
    TW_CHECK(a != nullptr);
    if (!a) return;
    TW_CHECK(a->degraded);
    // Confidence is cut; the raw score is not, so a damaged feed cannot
    // silently move the operating point the sweep chose.
    TW_CHECK(a->confidence < a->score);
    TW_CHECK_NEAR(a->confidence, a->score * 0.6, 1e-6);
}

TW_TEST(pipeline_clean_feed_leaves_confidence_alone) {
    Tape t;
    thin_book(t);
    spoof(t, 1100, 500);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    const Alert* a = w.first(DetectorKind::Spoofing);
    TW_CHECK(a != nullptr);
    if (!a) return;
    TW_CHECK(!a->degraded);
    TW_CHECK_NEAR(a->confidence, a->score, 1e-9);
}

TW_TEST(pipeline_recovers_an_out_of_order_arrival) {
    // The cancel is written before the order it cancels. Inside the reorder
    // tolerance the buffer puts them back in the right sequence, and the
    // detector sees the same scenario it would have seen on a clean feed.
    Tape clean;
    thin_book(clean);
    spoof(clean, 1100, 500);

    std::vector<std::string> lines = clean.lines();
    // The spoof's order line and its cancel are adjacent in the tape; swapping
    // them puts a cancel ahead of its own order.
    std::size_t order_idx = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
        if (lines[i].rfind("O,", 0) == 0 && lines[i].find(",50,500,") != std::string::npos)
            order_idx = i;
    TW_CHECK(order_idx > 0);
    std::swap(lines[order_idx], lines[order_idx + 1]);

    EngineConfig cfg = test_config();
    cfg.reorder_tolerance_ns = 1000 * kNsPerMs;  // wide enough to cover 300ms

    CollectingWriter w;
    Pipeline p(cfg, w);
    for (const auto& l : lines) p.feed_line(l);
    p.finish();

    TW_CHECK(p.stats().feed.reordered_recovered >= 1);
    TW_CHECK(w.count(DetectorKind::Spoofing, 50) >= 1);
}

TW_TEST(pipeline_drops_duplicate_events_without_double_counting) {
    Tape t;
    thin_book(t);
    spoof(t, 1100, 500);
    std::vector<std::string> lines = t.lines();

    CollectingWriter clean_w;
    Pipeline clean_p(test_config(), clean_w);
    for (const auto& l : lines) clean_p.feed_line(l);
    clean_p.finish();

    CollectingWriter dup_w;
    Pipeline dup_p(test_config(), dup_w);
    for (const auto& l : lines) {
        dup_p.feed_line(l);
        dup_p.feed_line(l);  // every message twice
    }
    dup_p.finish();

    TW_CHECK_EQ(dup_p.stats().feed.events_admitted, clean_p.stats().feed.events_admitted);
    TW_CHECK_EQ(dup_p.stats().feed.duplicate_event_ids,
                static_cast<std::uint64_t>(lines.size()));
}

TW_TEST(pipeline_counts_parse_failures_by_kind) {
    CollectingWriter w;
    Pipeline p(test_config(), w);
    p.feed_line("O,1,2,3,TWX,4,5,B,100,10");
    p.feed_line("garbage");
    p.feed_line("Q,1,2,3,TWX,4,5,B,100,10");
    p.feed_line("");
    p.feed_line("# comment");
    p.finish();

    TW_CHECK_EQ(p.stats().feed.lines_read, static_cast<std::uint64_t>(5));
    TW_CHECK_EQ(p.stats().feed.parse_failures, static_cast<std::uint64_t>(2));
    TW_CHECK_EQ(p.stats().feed.events_admitted, static_cast<std::uint64_t>(1));
}

TW_TEST(pipeline_handles_several_symbols_independently) {
    Tape a("AAA");
    thin_book(a);
    spoof(a, 1100, 500);

    Tape b("BBB");
    // A quiet second symbol must not pick up the first one's alerts, and its
    // book must not be polluted by the first one's orders. Its ids continue
    // where the first tape's left off, because both feed one sequenced stream.
    b.start_ids(1000, 1000).at(1600).order(9, 900, Side::Buy, 500, 10);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    for (const auto& l : a.lines()) p.feed_line(l);
    for (const auto& l : b.lines()) p.feed_line(l);
    p.finish();

    TW_CHECK_EQ(p.stats().symbols, static_cast<std::uint64_t>(2));
    for (const auto& al : w.alerts) TW_CHECK_EQ(al.symbol, std::string("AAA"));
}

TW_TEST(pipeline_final_pass_judges_the_end_of_the_tape) {
    // The spoof is the last thing on the tape. Without the flush in finish()
    // its evaluation window never closes and the alert is silently lost --
    // a recall hole at the end of every single run.
    Tape t;
    thin_book(t);
    spoof(t, 1100, 500);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK(w.count(DetectorKind::Spoofing) >= 1);
    TW_CHECK(p.stats().scans > 0);
}

TW_TEST(pipeline_reports_the_tape_span_it_saw) {
    Tape t;
    thin_book(t);
    spoof(t, 1100, 500);

    CollectingWriter w;
    Pipeline p(test_config(), w);
    t.run(p);
    TW_CHECK_EQ(p.stats().first_ts, static_cast<Ts>(1000 * kNsPerMs));
    TW_CHECK_EQ(p.stats().last_ts, static_cast<Ts>(1500 * kNsPerMs));
    TW_CHECK_EQ(p.stats().tape_span(), static_cast<Ts>(500 * kNsPerMs));
}
