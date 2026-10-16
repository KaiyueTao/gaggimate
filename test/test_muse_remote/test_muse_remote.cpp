// Unit tests: Phase 2 remote contract units (MuseRemote.h) — the state
// snapshot publication behind GET /api/muse/state and the single-slot
// line mailbox behind POST /api/muse/line (PHASE2_DESIGN.md §6, muse-1.4).
//
// These pin the protocol so the HTTP layer stays a thin shell:
//  - state_seq increments ONLY on the §6.1 trigger set (mode, brew
//    start/end, avatar state, warning SET incl. level changes and swaps,
//    link, stale flag) — never on temperature drift or re-polls;
//  - data_age_ms is measured from the last telemetry receipt, and the
//    stale gate (disconnected / never / aged out) both reports and blocks;
//  - the boot counter step is monotone and never reuses a value;
//  - the line_seq floor survives expiry/displacement/invalidation, kills
//    A -> B -> A retries, and resets only on a boot change;
//  - duplicate / out_of_order / stale_data / stale_context are distinct;
//  - TTL counts on a monotonic clock from receipt and is wrap-safe;
//  - text/charset/length/TTL validation and the 1 s + 30/min rate limit;
//  - render precedence: WARNING and DONE stay local, SLEEP accepts remote.
//
// Host-side — pio test -e native -f test_muse_remote.

#include <unity.h>

#include <cstdint>
#include <cstring>

#include <display/plugins/MuseRemote.h>

using namespace MuseRemote;

// Scoped enums do not convert to int for Unity's assert macros.
#define ASSERT_VERDICT(expected, actual) TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

namespace {

// Warning slots mirror WarningType (WarningManager.h).
constexpr int W_WATER = 0;
constexpr int W_FLUSH = 1;
constexpr int W_SWITCH = 2;
constexpr int W_SCALE = 3;
constexpr int W_BATTERY = 4;
constexpr int W_TEMP = 5;

StateInputs baseInputs() {
    StateInputs in;
    in.mode = 0; // Standby
    in.boilerC = 24.0f;
    in.targetC = 93.0f;
    in.brewActive = false;
    in.brewElapsedS = 0;
    in.avatarState = MuseAvatarState::SLEEP;
    in.controllerConnected = true;
    for (int i = 0; i < kMaxWarnings; i++) {
        in.warnId[i] = "w";
        in.warnText[i] = "Warning";
    }
    return in;
}

// A publisher on boot 7 with telemetry flowing from t=1000.
void initPublisher(StatePublisher &pub) {
    pub.setBootId(7);
    pub.noteTelemetry(1000);
    pub.update(baseInputs(), 1000);
}

LineContext freshCtx(uint32_t seq = 0) {
    return LineContext{7, seq, true, false};
}

LineSubmission makeSub(uint32_t seq, const char *id = "req-1", const char *text = "Hello there.", uint32_t ttlS = 20,
                       uint32_t bootId = 7, uint32_t stateSeq = 0) {
    LineSubmission sub;
    std::strncpy(sub.requestId, id, sizeof(sub.requestId) - 1);
    sub.lineSeq = seq;
    sub.bootId = bootId;
    sub.stateSeq = stateSeq;
    std::strncpy(sub.text, text, sizeof(sub.text) - 1);
    sub.ttlS = ttlS;
    return sub;
}

void initMailbox(LineMailbox &box) { box.setBootId(7); }

} // namespace

// ---------------------------------------------------------------------------
// Boot counter
// ---------------------------------------------------------------------------

void test_boot_counter_first_boot_is_one() { TEST_ASSERT_EQUAL_UINT32(1, nextBootId(0)); }

void test_boot_counter_monotone() {
    uint32_t id = 0;
    for (int i = 0; i < 100; i++) {
        const uint32_t next = nextBootId(id);
        TEST_ASSERT_GREATER_THAN_UINT32(id, next);
        id = next;
    }
    TEST_ASSERT_EQUAL_UINT32(100, id);
}

void test_boot_counter_wrap_never_reuses() {
    TEST_ASSERT_EQUAL_UINT32(1, nextBootId(0xFFFFFFFFu));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, nextBootId(0xFFFFFFFEu));
}

// ---------------------------------------------------------------------------
// Publisher: state_seq triggers
// ---------------------------------------------------------------------------

void test_first_publication_establishes_context() {
    StatePublisher pub;
    initPublisher(pub);
    const StateSnapshot snap = pub.getSnapshot(1000);
    TEST_ASSERT_EQUAL_UINT32(7, snap.bootId);
    TEST_ASSERT_EQUAL_UINT32(0, snap.stateSeq);
    TEST_ASSERT_EQUAL_INT(1, snap.schemaVersion);
    TEST_ASSERT_TRUE(snap.controllerConnected);
    TEST_ASSERT_FALSE(snap.stale);
    TEST_ASSERT_EQUAL_UINT32(0, snap.dataAgeMs);
}

void test_repoll_and_temperature_drift_do_not_increment() {
    StatePublisher pub;
    initPublisher(pub);
    StateInputs in = baseInputs();
    in.boilerC = 24.4f; // drift within the same state
    pub.update(in, 1100);
    in.boilerC = 25.1f;
    in.brewElapsedS = 9; // elapsed ticking is data, not context
    pub.update(in, 1200);
    pub.update(in, 1300);
    TEST_ASSERT_EQUAL_UINT32(0, pub.getSnapshot(1300).stateSeq);
}

void test_mode_change_increments() {
    StatePublisher pub;
    initPublisher(pub);
    StateInputs in = baseInputs();
    in.mode = 1;
    pub.update(in, 1100);
    TEST_ASSERT_EQUAL_UINT32(1, pub.getSnapshot(1100).stateSeq);
    pub.update(in, 1200); // same again: no further increment
    TEST_ASSERT_EQUAL_UINT32(1, pub.getSnapshot(1200).stateSeq);
}

void test_brew_start_and_end_increment() {
    StatePublisher pub;
    initPublisher(pub);
    StateInputs in = baseInputs();
    in.brewActive = true;
    pub.update(in, 1100);
    TEST_ASSERT_EQUAL_UINT32(1, pub.getSnapshot(1100).stateSeq);
    in.brewElapsedS = 12; // still brewing: elapsed alone is not a trigger
    pub.update(in, 1200);
    TEST_ASSERT_EQUAL_UINT32(1, pub.getSnapshot(1200).stateSeq);
    in.brewActive = false;
    pub.update(in, 1300);
    TEST_ASSERT_EQUAL_UINT32(2, pub.getSnapshot(1300).stateSeq);
}

void test_avatar_state_change_increments() {
    StatePublisher pub;
    initPublisher(pub);
    StateInputs in = baseInputs();
    in.avatarState = MuseAvatarState::HEATING;
    pub.update(in, 1100);
    TEST_ASSERT_EQUAL_UINT32(1, pub.getSnapshot(1100).stateSeq);
}

void test_warning_activation_and_clear_increment() {
    StatePublisher pub;
    initPublisher(pub);
    StateInputs in = baseInputs();
    in.warnActive[W_FLUSH] = true;
    in.warnLevel[W_FLUSH] = 1;
    pub.update(in, 1100);
    TEST_ASSERT_EQUAL_UINT32(1, pub.getSnapshot(1100).stateSeq);
    in.warnActive[W_FLUSH] = false;
    pub.update(in, 1200);
    TEST_ASSERT_EQUAL_UINT32(2, pub.getSnapshot(1200).stateSeq);
}

void test_warning_level_change_increments() {
    StatePublisher pub;
    initPublisher(pub);
    StateInputs in = baseInputs();
    in.warnActive[W_TEMP] = true;
    in.warnLevel[W_TEMP] = 1; // WARN
    pub.update(in, 1100);
    TEST_ASSERT_EQUAL_UINT32(1, pub.getSnapshot(1100).stateSeq);
    in.warnLevel[W_TEMP] = 2; // escalated to ERROR, same active set
    pub.update(in, 1200);
    TEST_ASSERT_EQUAL_UINT32(2, pub.getSnapshot(1200).stateSeq);
}

void test_warning_swap_with_same_avatar_state_increments() {
    // Water warning clears, temperature error activates: the active COUNT
    // is unchanged and the avatar stays WARNING, but the SET changed.
    StatePublisher pub;
    initPublisher(pub);
    StateInputs in = baseInputs();
    in.avatarState = MuseAvatarState::WARNING;
    in.warnActive[W_WATER] = true;
    in.warnLevel[W_WATER] = 1;
    pub.update(in, 1100);
    const uint32_t seqBefore = pub.getSnapshot(1100).stateSeq;
    in.warnActive[W_WATER] = false;
    in.warnActive[W_TEMP] = true;
    in.warnLevel[W_TEMP] = 2;
    pub.update(in, 1200);
    TEST_ASSERT_EQUAL_UINT32(seqBefore + 1, pub.getSnapshot(1200).stateSeq);
}

void test_connection_change_increments() {
    StatePublisher pub;
    initPublisher(pub);
    StateInputs in = baseInputs();
    in.controllerConnected = false;
    pub.update(in, 1100);
    TEST_ASSERT_EQUAL_UINT32(1, pub.getSnapshot(1100).stateSeq);
}

void test_stale_flag_change_increments_once() {
    StatePublisher pub;
    initPublisher(pub); // telemetry at t=1000
    StateInputs in = baseInputs();
    pub.update(in, 2000); // age 1000: fresh
    TEST_ASSERT_EQUAL_UINT32(0, pub.getSnapshot(2000).stateSeq);
    pub.update(in, 7000); // age 6000 > 5000: stale flips
    TEST_ASSERT_TRUE(pub.getSnapshot(7000).stale);
    TEST_ASSERT_EQUAL_UINT32(1, pub.getSnapshot(7000).stateSeq);
    pub.update(in, 8000); // still stale: no second increment
    TEST_ASSERT_EQUAL_UINT32(1, pub.getSnapshot(8000).stateSeq);
    pub.noteTelemetry(8500); // telemetry resumes: stale flips back
    pub.update(in, 8500);
    TEST_ASSERT_FALSE(pub.getSnapshot(8500).stale);
    TEST_ASSERT_EQUAL_UINT32(2, pub.getSnapshot(8500).stateSeq);
}

void test_snapshot_carries_all_active_warnings() {
    StatePublisher pub;
    initPublisher(pub);
    StateInputs in = baseInputs();
    in.warnActive[W_WATER] = true;
    in.warnLevel[W_WATER] = 1;
    in.warnId[W_WATER] = "water";
    in.warnText[W_WATER] = "Water tank low";
    in.warnActive[W_BATTERY] = true;
    in.warnLevel[W_BATTERY] = 2;
    in.warnId[W_BATTERY] = "scaleBattery";
    in.warnText[W_BATTERY] = "Scale battery low";
    pub.update(in, 1100);
    const StateSnapshot snap = pub.getSnapshot(1100);
    TEST_ASSERT_EQUAL_INT(2, snap.warningCount);
    TEST_ASSERT_EQUAL_STRING("water", snap.warnings[0].id);
    TEST_ASSERT_EQUAL_INT(1, snap.warnings[0].level);
    TEST_ASSERT_EQUAL_STRING("Water tank low", snap.warnings[0].text);
    TEST_ASSERT_EQUAL_STRING("scaleBattery", snap.warnings[1].id);
    TEST_ASSERT_EQUAL_INT(2, snap.warnings[1].level);
}

// ---------------------------------------------------------------------------
// Publisher: freshness
// ---------------------------------------------------------------------------

void test_data_age_measured_from_telemetry_receipt() {
    StatePublisher pub;
    initPublisher(pub); // telemetry at t=1000
    pub.noteTelemetry(2000);
    const StateSnapshot snap = pub.getSnapshot(2600);
    TEST_ASSERT_EQUAL_UINT32(600, snap.dataAgeMs);
    TEST_ASSERT_FALSE(snap.stale);
}

void test_stale_when_disconnected_even_with_fresh_telemetry() {
    StatePublisher pub;
    initPublisher(pub);
    StateInputs in = baseInputs();
    in.controllerConnected = false;
    pub.update(in, 1100);
    const StateSnapshot snap = pub.getSnapshot(1100);
    TEST_ASSERT_FALSE(snap.controllerConnected);
    TEST_ASSERT_TRUE(snap.stale);
}

void test_stale_when_no_telemetry_ever() {
    StatePublisher pub;
    pub.setBootId(3);
    pub.update(baseInputs(), 100);
    TEST_ASSERT_TRUE(pub.getSnapshot(100).stale);
}

void test_stale_threshold_boundary() {
    StatePublisher pub;
    initPublisher(pub); // telemetry at t=1000
    TEST_ASSERT_FALSE(pub.getSnapshot(6000).stale); // age exactly 5000
    TEST_ASSERT_TRUE(pub.getSnapshot(6001).stale);  // age 5001
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

void test_mode_and_state_names() {
    TEST_ASSERT_EQUAL_STRING("Standby", modeName(0));
    TEST_ASSERT_EQUAL_STRING("Brew", modeName(1));
    TEST_ASSERT_EQUAL_STRING("Steam", modeName(2));
    TEST_ASSERT_EQUAL_STRING("Water", modeName(3));
    TEST_ASSERT_EQUAL_STRING("Grind", modeName(4));
    TEST_ASSERT_EQUAL_STRING("Unknown", modeName(99));
    TEST_ASSERT_EQUAL_STRING("READY", avatarStateName(MuseAvatarState::READY));
    TEST_ASSERT_EQUAL_STRING("BREWING_LATE", avatarStateName(MuseAvatarState::BREWING_LATE));
    TEST_ASSERT_EQUAL_STRING("ignore", warningLevelName(0));
    TEST_ASSERT_EQUAL_STRING("warn", warningLevelName(1));
    TEST_ASSERT_EQUAL_STRING("error", warningLevelName(2));
}

// ---------------------------------------------------------------------------
// Mailbox: acceptance, duplicates, ordering
// ---------------------------------------------------------------------------

void test_accept_valid_line() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(1), freshCtx(), 5000));
    TEST_ASSERT_TRUE(box.hasLine());
    TEST_ASSERT_EQUAL_UINT32(1, box.floor());
    char out[64] = "";
    TEST_ASSERT_TRUE(box.copyValidLine(5100, freshCtx(), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Hello there.", out);
}

void test_exact_duplicate_is_noop() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(4, "req-a"), freshCtx(), 5000));
    ASSERT_VERDICT(LineVerdict::Duplicate, box.accept(makeSub(4, "req-a"), freshCtx(), 5100));
    TEST_ASSERT_EQUAL_UINT32(4, box.floor());
}

void test_out_of_order_rejected() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(5, "req-a"), freshCtx(), 5000));
    ASSERT_VERDICT(LineVerdict::OutOfOrder, box.accept(makeSub(3, "req-b"), freshCtx(), 5100));
    // Same seq, different request id: not a duplicate, still out of order.
    ASSERT_VERDICT(LineVerdict::OutOfOrder, box.accept(makeSub(5, "req-c"), freshCtx(), 5200));
}

void test_aba_retry_cannot_resurrect_older_line() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(1, "req-a", "Line A"), freshCtx(), 5000));
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(2, "req-b", "Line B"), freshCtx(), 5100));
    ASSERT_VERDICT(LineVerdict::OutOfOrder, box.accept(makeSub(1, "req-a", "Line A"), freshCtx(), 5200));
    char out[64] = "";
    TEST_ASSERT_TRUE(box.copyValidLine(5300, freshCtx(), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Line B", out); // B still the stored line
}

void test_expiry_does_not_lower_floor() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(5, "req-a", "Short", 3), freshCtx(), 5000));
    char out[64] = "";
    TEST_ASSERT_FALSE(box.copyValidLine(9000, freshCtx(), out, sizeof(out))); // TTL 3 s long gone
    TEST_ASSERT_FALSE(box.hasLine());
    TEST_ASSERT_EQUAL_UINT32(5, box.floor()); // floor survived the expiry
    ASSERT_VERDICT(LineVerdict::OutOfOrder, box.accept(makeSub(3, "req-b"), freshCtx(), 9100));
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(6, "req-c"), freshCtx(), 9200));
}

void test_floor_resets_on_new_boot() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(9, "req-a"), freshCtx(), 5000));
    box.setBootId(8); // reboot
    TEST_ASSERT_FALSE(box.hasLine());
    TEST_ASSERT_EQUAL_UINT32(0, box.floor());
    const LineContext ctx8{8, 0, true, false};
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(1, "req-b", "Hi", 20, 8), ctx8, 6000));
}

void test_stale_context_rejected() {
    LineMailbox box;
    initMailbox(box);
    // Wrong state_seq.
    ASSERT_VERDICT(LineVerdict::StaleContext, box.accept(makeSub(1, "req-a", "Hi", 20, 7, 99), freshCtx(3), 5000));
    // Wrong boot_id.
    ASSERT_VERDICT(LineVerdict::StaleContext, box.accept(makeSub(1, "req-a", "Hi", 20, 6, 3), freshCtx(3), 5000));
    TEST_ASSERT_FALSE(box.hasLine());
}

void test_stale_data_gate_rejects() {
    LineMailbox box;
    initMailbox(box);
    LineContext staleCtx = freshCtx();
    staleCtx.stale = true;
    ASSERT_VERDICT(LineVerdict::StaleData, box.accept(makeSub(1), staleCtx, 5000));
    LineContext downCtx = freshCtx();
    downCtx.controllerConnected = false;
    ASSERT_VERDICT(LineVerdict::StaleData, box.accept(makeSub(1), downCtx, 5000));
    TEST_ASSERT_FALSE(box.hasLine());
    TEST_ASSERT_EQUAL_UINT32(0, box.floor()); // gate rejections never raise the floor
}

// ---------------------------------------------------------------------------
// Mailbox: invalidation
// ---------------------------------------------------------------------------

void test_stored_line_invalidated_when_data_goes_stale() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(1), freshCtx(), 5000));
    LineContext staleCtx = freshCtx();
    staleCtx.stale = true;
    box.service(5100, staleCtx, MuseAvatarState::SLEEP, true);
    TEST_ASSERT_FALSE(box.hasLine());
    TEST_ASSERT_EQUAL_UINT32(1, box.floor()); // floor kept through invalidation
}

void test_stored_line_invalidated_on_disconnect_via_copy() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(1), freshCtx(), 5000));
    LineContext downCtx = freshCtx();
    downCtx.controllerConnected = false;
    char out[64] = "";
    TEST_ASSERT_FALSE(box.copyValidLine(5100, downCtx, out, sizeof(out)));
    TEST_ASSERT_FALSE(box.hasLine());
}

void test_stored_line_invalidated_on_context_change() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(1, "req-a", "Hi", 20, 7, 10), freshCtx(10), 5000));
    box.service(5100, freshCtx(11), MuseAvatarState::READY, true); // state moved on
    TEST_ASSERT_FALSE(box.hasLine());
}

void test_stored_line_invalidated_on_warning_takeover() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(1), freshCtx(), 5000));
    box.service(5100, freshCtx(), MuseAvatarState::WARNING, true);
    TEST_ASSERT_FALSE(box.hasLine());
}

void test_stored_line_invalidated_when_feature_disabled() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(1), freshCtx(), 5000));
    box.service(5100, freshCtx(), MuseAvatarState::SLEEP, false);
    TEST_ASSERT_FALSE(box.hasLine());
    box.clear(); // clear() on empty is a no-op, must not crash
    TEST_ASSERT_FALSE(box.hasLine());
}

void test_stored_line_survives_service_when_all_fresh() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(1), freshCtx(), 5000));
    box.service(5100, freshCtx(), MuseAvatarState::READY, true);
    TEST_ASSERT_TRUE(box.hasLine());
}

// ---------------------------------------------------------------------------
// Mailbox: TTL on the monotonic clock
// ---------------------------------------------------------------------------

void test_ttl_expiry_boundary() {
    LineMailbox box;
    initMailbox(box);
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(1, "req-a", "Hi", 3), freshCtx(), 5000));
    char out[64] = "";
    TEST_ASSERT_TRUE(box.copyValidLine(5000 + 2999, freshCtx(), out, sizeof(out)));
    TEST_ASSERT_FALSE(box.copyValidLine(5000 + 3000, freshCtx(), out, sizeof(out)));
}

void test_ttl_wrap_safe() {
    LineMailbox box;
    initMailbox(box);
    const uint32_t t0 = 0xFFFFFFF0u; // 16 ms before the 32-bit wrap
    ASSERT_VERDICT(LineVerdict::Accepted, box.accept(makeSub(1, "req-a", "Hi", 3), freshCtx(), t0));
    char out[64] = "";
    TEST_ASSERT_TRUE(box.copyValidLine(t0 + 2000, freshCtx(), out, sizeof(out))); // wrapped clock
    TEST_ASSERT_FALSE(box.copyValidLine(t0 + 3000, freshCtx(), out, sizeof(out)));
}

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

void test_text_validation() {
    TEST_ASSERT_TRUE(lineTextValid("Hello", 5));
    TEST_ASSERT_TRUE(lineTextValid(" ", 1));                    // space is printable
    TEST_ASSERT_TRUE(lineTextValid("~", 1));                    // 0x7E top of range
    TEST_ASSERT_FALSE(lineTextValid("", 0));                    // empty
    TEST_ASSERT_FALSE(lineTextValid("x", 41));                  // too long (len rules)
    char forty[41];
    std::memset(forty, 'a', 40);
    forty[40] = '\0';
    TEST_ASSERT_TRUE(lineTextValid(forty, 40));                 // exactly 40 ok
    TEST_ASSERT_FALSE(lineTextValid("bad\x1F", 4));             // control char
    TEST_ASSERT_FALSE(lineTextValid("bad\x7F", 4));             // DEL
    const char utf8[] = "caf\xC3\xA9";                          // non-ASCII bytes
    TEST_ASSERT_FALSE(lineTextValid(utf8, sizeof(utf8) - 1));
    TEST_ASSERT_FALSE(lineTextValid(nullptr, 3));
}

void test_request_id_validation() {
    TEST_ASSERT_TRUE(requestIdValid("s-20261016-0042", 15));
    TEST_ASSERT_FALSE(requestIdValid("", 0));
    char long49[50];
    std::memset(long49, 'a', 49);
    long49[49] = '\0';
    TEST_ASSERT_TRUE(requestIdValid(long49, 48));
    TEST_ASSERT_FALSE(requestIdValid(long49, 49));
    TEST_ASSERT_FALSE(requestIdValid("a\tb", 3));
}

void test_ttl_clamp() {
    TEST_ASSERT_EQUAL_UINT32(3, clampTtlS(0));
    TEST_ASSERT_EQUAL_UINT32(3, clampTtlS(2));
    TEST_ASSERT_EQUAL_UINT32(3, clampTtlS(3));
    TEST_ASSERT_EQUAL_UINT32(60, clampTtlS(60));
    TEST_ASSERT_EQUAL_UINT32(120, clampTtlS(120));
    TEST_ASSERT_EQUAL_UINT32(120, clampTtlS(121));
    TEST_ASSERT_EQUAL_UINT32(120, clampTtlS(100000));
    TEST_ASSERT_EQUAL_UINT32(3, clampTtlS(-5));
}

// ---------------------------------------------------------------------------
// Rate limit
// ---------------------------------------------------------------------------

void test_rate_limit_one_per_second() {
    LineMailbox box;
    initMailbox(box);
    TEST_ASSERT_TRUE(box.noteAttempt(10000));
    TEST_ASSERT_FALSE(box.noteAttempt(10500)); // 500 ms later
    TEST_ASSERT_FALSE(box.noteAttempt(10999));
    TEST_ASSERT_TRUE(box.noteAttempt(11000)); // exactly 1 s later
}

void test_rate_limit_thirty_per_minute() {
    LineMailbox box;
    initMailbox(box);
    for (int i = 0; i < 30; i++) {
        TEST_ASSERT_TRUE(box.noteAttempt(10000 + static_cast<uint32_t>(i) * 1000));
    }
    // 31st attempt, still inside the minute (and >1 s after the 30th).
    TEST_ASSERT_FALSE(box.noteAttempt(10000 + 30000 + 500));
    // Once the oldest attempts slide out of the window it opens again.
    TEST_ASSERT_TRUE(box.noteAttempt(10000 + 61000));
}

// ---------------------------------------------------------------------------
// Render precedence (§6.2)
// ---------------------------------------------------------------------------

void test_precedence_warning_stays_local() {
    ASSERT_VERDICT(BubbleSource::LocalWarning, decideBubbleSource(MuseAvatarState::WARNING, true));
    ASSERT_VERDICT(BubbleSource::LocalWarning, decideBubbleSource(MuseAvatarState::WARNING, false));
}

void test_precedence_done_rejects_remote() {
    ASSERT_VERDICT(BubbleSource::LocalDone, decideBubbleSource(MuseAvatarState::DONE, true));
    ASSERT_VERDICT(BubbleSource::LocalDone, decideBubbleSource(MuseAvatarState::DONE, false));
}

void test_precedence_sleep_accepts_remote() {
    ASSERT_VERDICT(BubbleSource::Remote, decideBubbleSource(MuseAvatarState::SLEEP, true));
    ASSERT_VERDICT(BubbleSource::None, decideBubbleSource(MuseAvatarState::SLEEP, false));
}

void test_precedence_remote_beats_local_table() {
    ASSERT_VERDICT(BubbleSource::Remote, decideBubbleSource(MuseAvatarState::READY, true));
    ASSERT_VERDICT(BubbleSource::Remote, decideBubbleSource(MuseAvatarState::BREWING, true));
    ASSERT_VERDICT(BubbleSource::Remote, decideBubbleSource(MuseAvatarState::HEATING, true));
    ASSERT_VERDICT(BubbleSource::LocalTable, decideBubbleSource(MuseAvatarState::READY, false));
    ASSERT_VERDICT(BubbleSource::None, decideBubbleSource(MuseAvatarState::HIDDEN, true));
}

// ---------------------------------------------------------------------------
// Verdict mapping
// ---------------------------------------------------------------------------

void test_verdict_reason_and_status() {
    TEST_ASSERT_EQUAL_STRING("accepted", verdictReason(LineVerdict::Accepted));
    TEST_ASSERT_EQUAL_STRING("duplicate", verdictReason(LineVerdict::Duplicate));
    TEST_ASSERT_EQUAL_STRING("out_of_order", verdictReason(LineVerdict::OutOfOrder));
    TEST_ASSERT_EQUAL_STRING("stale_data", verdictReason(LineVerdict::StaleData));
    TEST_ASSERT_EQUAL_STRING("stale_context", verdictReason(LineVerdict::StaleContext));
    TEST_ASSERT_EQUAL_STRING("rate_limited", verdictReason(LineVerdict::RateLimited));
    TEST_ASSERT_EQUAL_INT(200, verdictHttpStatus(LineVerdict::Accepted));
    TEST_ASSERT_EQUAL_INT(200, verdictHttpStatus(LineVerdict::Duplicate));
    TEST_ASSERT_EQUAL_INT(409, verdictHttpStatus(LineVerdict::OutOfOrder));
    TEST_ASSERT_EQUAL_INT(409, verdictHttpStatus(LineVerdict::StaleData));
    TEST_ASSERT_EQUAL_INT(409, verdictHttpStatus(LineVerdict::StaleContext));
    TEST_ASSERT_EQUAL_INT(429, verdictHttpStatus(LineVerdict::RateLimited));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_boot_counter_first_boot_is_one);
    RUN_TEST(test_boot_counter_monotone);
    RUN_TEST(test_boot_counter_wrap_never_reuses);
    RUN_TEST(test_first_publication_establishes_context);
    RUN_TEST(test_repoll_and_temperature_drift_do_not_increment);
    RUN_TEST(test_mode_change_increments);
    RUN_TEST(test_brew_start_and_end_increment);
    RUN_TEST(test_avatar_state_change_increments);
    RUN_TEST(test_warning_activation_and_clear_increment);
    RUN_TEST(test_warning_level_change_increments);
    RUN_TEST(test_warning_swap_with_same_avatar_state_increments);
    RUN_TEST(test_connection_change_increments);
    RUN_TEST(test_stale_flag_change_increments_once);
    RUN_TEST(test_snapshot_carries_all_active_warnings);
    RUN_TEST(test_data_age_measured_from_telemetry_receipt);
    RUN_TEST(test_stale_when_disconnected_even_with_fresh_telemetry);
    RUN_TEST(test_stale_when_no_telemetry_ever);
    RUN_TEST(test_stale_threshold_boundary);
    RUN_TEST(test_mode_and_state_names);
    RUN_TEST(test_accept_valid_line);
    RUN_TEST(test_exact_duplicate_is_noop);
    RUN_TEST(test_out_of_order_rejected);
    RUN_TEST(test_aba_retry_cannot_resurrect_older_line);
    RUN_TEST(test_expiry_does_not_lower_floor);
    RUN_TEST(test_floor_resets_on_new_boot);
    RUN_TEST(test_stale_context_rejected);
    RUN_TEST(test_stale_data_gate_rejects);
    RUN_TEST(test_stored_line_invalidated_when_data_goes_stale);
    RUN_TEST(test_stored_line_invalidated_on_disconnect_via_copy);
    RUN_TEST(test_stored_line_invalidated_on_context_change);
    RUN_TEST(test_stored_line_invalidated_on_warning_takeover);
    RUN_TEST(test_stored_line_invalidated_when_feature_disabled);
    RUN_TEST(test_stored_line_survives_service_when_all_fresh);
    RUN_TEST(test_ttl_expiry_boundary);
    RUN_TEST(test_ttl_wrap_safe);
    RUN_TEST(test_text_validation);
    RUN_TEST(test_request_id_validation);
    RUN_TEST(test_ttl_clamp);
    RUN_TEST(test_rate_limit_one_per_second);
    RUN_TEST(test_rate_limit_thirty_per_minute);
    RUN_TEST(test_precedence_warning_stays_local);
    RUN_TEST(test_precedence_done_rejects_remote);
    RUN_TEST(test_precedence_sleep_accepts_remote);
    RUN_TEST(test_precedence_remote_beats_local_table);
    RUN_TEST(test_verdict_reason_and_status);
    return UNITY_END();
}
