#ifndef MUSE_REMOTE_H
#define MUSE_REMOTE_H

// Phase 2 contract units (PHASE2_DESIGN.md §6, muse-1.4): the state
// snapshot publication behind GET /api/muse/state and the single-slot
// remote-line mailbox behind POST /api/muse/line.
//
// Everything in this header is deliberately free of Arduino / LVGL / HTTP
// includes so the native test suite can pin the contract directly
// (test_muse_remote). The firmware glue lives in MuseAvatarPlugin
// (publisher inputs, mailbox servicing, render decision) and WebUIPlugin
// (the two HTTP routes); the two global instances are defined in
// MuseRemote.cpp.
//
// Threading (§6.3): the publisher and the mailbox each serialise on their
// own mutex. Any task may read the published snapshot or submit a line;
// nobody ever holds a pointer into memory another task can rewrite —
// snapshots and lines are copied in and out by value.

#include "MuseLines.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace MuseRemote {

// ---------------------------------------------------------------------------
// Constants (§6.1 / §6.2 / §6.3)
// ---------------------------------------------------------------------------

constexpr int kSchemaVersion = 1;

// A snapshot is stale when the controller link is down, when no telemetry
// was ever received, or when the newest telemetry is older than this.
constexpr uint32_t kStaleThresholdMs = 5000; // MUSE_STATE_STALE_MS (§6.1)

// The one v1 sender: the paired Muse session via the companion gadget.
constexpr const char *kSenderId = "session-main"; // (§6.2)

constexpr size_t kLineBodyMax = 512;      // JSON body cap, bytes (§6.3)
constexpr size_t kLineTextMax = 40;       // text cap, chars (§6.3)
constexpr size_t kRequestIdMax = 48;      // request_id cap, chars
constexpr uint32_t kLineTtlMinS = 3;      // TTL clamp (§6.2)
constexpr uint32_t kLineTtlMaxS = 120;    //
constexpr uint32_t kRateMinIntervalMs = 1000; // at most one POST / second
constexpr uint32_t kRateWindowMs = 60000;     // ... and 30 per minute (§6.3)
constexpr size_t kRateWindowMax = 30;

constexpr int kMaxWarnings = 8; // WARNING_TYPE_COUNT

// ---------------------------------------------------------------------------
// Boot counter (§6.1): a persistent NVS counter incremented exactly once at
// every startup. The pure step lives here so the native suite can pin it;
// the NVS read/increment/write happens in MuseAvatarPlugin::setup().
// ---------------------------------------------------------------------------

inline uint32_t nextBootId(uint32_t stored) {
    if (stored == 0 || stored == 0xFFFFFFFFu)
        return 1;
    return stored + 1;
}

// ---------------------------------------------------------------------------
// Small pure helpers
// ---------------------------------------------------------------------------

// Controller mode string (§6.1); values mirror constants.h MODE_*.
inline const char *modeName(int mode) {
    switch (mode) {
    case 0:
        return "Standby";
    case 1:
        return "Brew";
    case 2:
        return "Steam";
    case 3:
        return "Water";
    case 4:
        return "Grind";
    default:
        return "Unknown";
    }
}

inline const char *avatarStateName(MuseAvatarState state) {
    switch (state) {
    case MuseAvatarState::HIDDEN:
        return "HIDDEN";
    case MuseAvatarState::SLEEP:
        return "SLEEP";
    case MuseAvatarState::HEATING:
        return "HEATING";
    case MuseAvatarState::READY:
        return "READY";
    case MuseAvatarState::BREWING:
        return "BREWING";
    case MuseAvatarState::BREWING_LATE:
        return "BREWING_LATE";
    case MuseAvatarState::STEAMING:
        return "STEAMING";
    case MuseAvatarState::DONE:
        return "DONE";
    case MuseAvatarState::WARNING:
        return "WARNING";
    }
    return "UNKNOWN";
}

// Warning level string (§6.1: "level as configured: ignore/warn/error");
// values mirror WarningLevel in Settings.h.
inline const char *warningLevelName(int level) {
    switch (level) {
    case 0:
        return "ignore";
    case 1:
        return "warn";
    case 2:
        return "error";
    default:
        return "unknown";
    }
}

// Text rule (§6.3): 1..40 chars, printable ASCII (0x20..0x7E) only — the
// charset enforcement is the v1 English-only policy made executable.
inline bool lineTextValid(const char *text, size_t len) {
    if (text == nullptr || len < 1 || len > kLineTextMax)
        return false;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c > 0x7E)
            return false;
    }
    return true;
}

inline bool requestIdValid(const char *id, size_t len) {
    if (id == nullptr || len < 1 || len > kRequestIdMax)
        return false;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        if (c < 0x20 || c > 0x7E)
            return false;
    }
    return true;
}

// TTL is clamped into [3, 120] s, never rejected for range (§6.2).
inline uint32_t clampTtlS(int64_t ttlS) {
    if (ttlS < static_cast<int64_t>(kLineTtlMinS))
        return kLineTtlMinS;
    if (ttlS > static_cast<int64_t>(kLineTtlMaxS))
        return kLineTtlMaxS;
    return static_cast<uint32_t>(ttlS);
}

// ---------------------------------------------------------------------------
// State snapshot (§6.1)
// ---------------------------------------------------------------------------

struct WarningEntry {
    char id[24] = "";
    int level = 0;
    char text[48] = "";
};

// One consistent publication: context identity (boot_id, state_seq) and the
// data it describes are assembled under the publisher lock and copied out
// whole. Consumers never stitch together independent atomics (§6.1).
struct StateSnapshot {
    int schemaVersion = kSchemaVersion;
    uint32_t bootId = 0;
    uint32_t stateSeq = 0;
    int mode = 0;
    float boilerC = 0.0f;
    float targetC = 0.0f;
    bool brewActive = false;
    uint32_t brewElapsedS = 0;
    MuseAvatarState avatarState = MuseAvatarState::HIDDEN;
    bool controllerConnected = false;
    uint32_t dataAgeMs = 0;
    bool stale = true;
    WarningEntry warnings[kMaxWarnings];
    int warningCount = 0;
};

// What the plugin task samples each poll and hands to the publisher.
// warnId/warnText point at static storage (WarningManager::key/label) and
// are copied from during update(); level[] is the configured level.
struct StateInputs {
    int mode = 0;
    float boilerC = 0.0f;
    float targetC = 0.0f;
    bool brewActive = false;
    uint32_t brewElapsedS = 0;
    MuseAvatarState avatarState = MuseAvatarState::HIDDEN;
    bool warnActive[kMaxWarnings] = {};
    int warnLevel[kMaxWarnings] = {};
    const char *warnId[kMaxWarnings] = {};
    const char *warnText[kMaxWarnings] = {};
    bool controllerConnected = false;
};

class StatePublisher {
  public:
    void setBootId(uint32_t bootId) {
        std::lock_guard<std::mutex> guard(mutex);
        snapshot.bootId = bootId;
    }

    uint32_t bootId() const {
        std::lock_guard<std::mutex> guard(mutex);
        return snapshot.bootId;
    }

    // Called on every controller telemetry frame (the display's sensor-data
    // path — the comms event the Controller fires per frame). Lock-free:
    // the caller can be a comms callback task. Only the timestamp crosses.
    void noteTelemetry(uint32_t nowMs) { lastTelemetryMs.store(nowMs); hasTelemetry.store(true); }

    // Plugin-task publication. state_seq increments ONLY on the §6.1
    // trigger set, detected here against the previously published context:
    // mode change, brew start/end, avatar_state change, warning SET change
    // (activation, clearing, or level change — one packed signature), link
    // change, stale flag change. Temperature drift, elapsed ticking and
    // re-polls change data fields only, never the sequence.
    void update(const StateInputs &in, uint32_t nowMs) {
        std::lock_guard<std::mutex> guard(mutex);
        const bool staleNow = computeStale(in.controllerConnected, nowMs);
        const uint32_t warnSig = warningSignature(in);
        if (!published) {
            published = true; // first publication establishes the context
        } else if (in.mode != snapshot.mode || in.brewActive != snapshot.brewActive ||
                   in.avatarState != snapshot.avatarState || warnSig != warningSigPublished ||
                   in.controllerConnected != snapshot.controllerConnected || staleNow != snapshot.stale) {
            snapshot.stateSeq++;
        }
        warningSigPublished = warnSig;
        snapshot.mode = in.mode;
        snapshot.boilerC = in.boilerC;
        snapshot.targetC = in.targetC;
        snapshot.brewActive = in.brewActive;
        snapshot.brewElapsedS = in.brewElapsedS;
        snapshot.avatarState = in.avatarState;
        snapshot.controllerConnected = in.controllerConnected;
        snapshot.stale = staleNow;
        snapshot.warningCount = 0;
        for (int i = 0; i < kMaxWarnings; i++) {
            if (!in.warnActive[i])
                continue;
            WarningEntry &entry = snapshot.warnings[snapshot.warningCount++];
            copyStr(entry.id, sizeof(entry.id), in.warnId[i]);
            entry.level = in.warnLevel[i];
            copyStr(entry.text, sizeof(entry.text), in.warnText[i]);
        }
    }

    // A whole-snapshot copy. data_age_ms / stale are re-derived at serve
    // time from the telemetry timestamp (age is a serve-time quantity);
    // state_seq moves only via update(), so a freshness flip is reflected
    // in the sequence at the next plugin poll (150 ms), while the fields
    // below are never older than the caller's nowMs.
    StateSnapshot getSnapshot(uint32_t nowMs) const {
        std::lock_guard<std::mutex> guard(mutex);
        StateSnapshot out = snapshot;
        out.dataAgeMs = dataAgeMs(nowMs);
        out.stale = computeStale(out.controllerConnected, nowMs);
        return out;
    }

  private:
    static void copyStr(char *dst, size_t cap, const char *src) {
        if (cap == 0)
            return;
        if (src == nullptr)
            src = "";
        std::strncpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
    }

    // Per type: 0 when inactive, level+1 when active (levels are 0..2, so
    // 2 bits would do; 4 bits per type keeps the packing obvious).
    static uint32_t warningSignature(const StateInputs &in) {
        uint32_t sig = 0;
        for (int i = 0; i < kMaxWarnings; i++) {
            if (in.warnActive[i])
                sig |= static_cast<uint32_t>(in.warnLevel[i] + 1) << (4 * i);
        }
        return sig;
    }

    uint32_t dataAgeMs(uint32_t nowMs) const {
        if (!hasTelemetry.load())
            return nowMs; // nothing ever received: age of the boot itself
        return nowMs - lastTelemetryMs.load(); // unsigned: wrap-safe
    }

    bool computeStale(bool connected, uint32_t nowMs) const {
        if (!connected || !hasTelemetry.load())
            return true;
        return dataAgeMs(nowMs) > kStaleThresholdMs;
    }

    mutable std::mutex mutex;
    StateSnapshot snapshot;
    uint32_t warningSigPublished = 0;
    bool published = false;
    std::atomic<uint32_t> lastTelemetryMs{0};
    std::atomic<bool> hasTelemetry{false};
};

// ---------------------------------------------------------------------------
// Line mailbox (§6.2 / §6.3)
// ---------------------------------------------------------------------------

// The context a line is validated and rendered against: read from the one
// published snapshot by both the POST handler and the UI path.
struct LineContext {
    uint32_t bootId = 0;
    uint32_t stateSeq = 0;
    bool controllerConnected = false;
    bool stale = true;
};

inline LineContext lineContextOf(const StateSnapshot &snap) {
    return LineContext{snap.bootId, snap.stateSeq, snap.controllerConnected, snap.stale};
}

// A syntactically validated submission (the HTTP layer checks JSON shape,
// field types, sender, text charset/length and clamps the TTL before
// building this; the mailbox owns every semantic rule).
struct LineSubmission {
    char requestId[kRequestIdMax + 1] = "";
    uint32_t lineSeq = 0;
    uint32_t bootId = 0;
    uint32_t stateSeq = 0;
    char text[kLineTextMax + 1] = "";
    uint32_t ttlS = 0;
};

enum class LineVerdict : uint8_t {
    Accepted,
    Duplicate,
    OutOfOrder,
    StaleData,
    StaleContext,
    RateLimited,
};

inline const char *verdictReason(LineVerdict verdict) {
    switch (verdict) {
    case LineVerdict::Accepted:
        return "accepted";
    case LineVerdict::Duplicate:
        return "duplicate";
    case LineVerdict::OutOfOrder:
        return "out_of_order";
    case LineVerdict::StaleData:
        return "stale_data";
    case LineVerdict::StaleContext:
        return "stale_context";
    case LineVerdict::RateLimited:
        return "rate_limited";
    }
    return "unknown";
}

inline int verdictHttpStatus(LineVerdict verdict) {
    switch (verdict) {
    case LineVerdict::Accepted:
    case LineVerdict::Duplicate:
        return 200;
    case LineVerdict::RateLimited:
        return 429;
    default:
        return 409; // out_of_order / stale_data / stale_context
    }
}

class LineMailbox {
  public:
    // Startup (and, in tests, a simulated reboot): a new boot gets an
    // empty slot and a zeroed sequence floor — pre-restart lines can never
    // be mistaken for current context.
    void setBootId(uint32_t bootId) {
        std::lock_guard<std::mutex> guard(mutex);
        currentBootId = bootId;
        stored = StoredLine{};
        floorSeq = 0;
        lastAcceptedRequestId[0] = '\0';
        hasAccepted = false;
    }

    // Front-door rate limit (§6.3: at most one POST per second and 30 per
    // minute). Every attempt is recorded, whatever its later fate — the
    // limiter guards the endpoint, not just accepted lines. Returns false
    // when this attempt exceeds the limit.
    bool noteAttempt(uint32_t nowMs) {
        std::lock_guard<std::mutex> guard(mutex);
        if (hasLastAttempt && nowMs - lastAttemptMs < kRateMinIntervalMs)
            return false;
        // Count attempts inside the trailing window (ring of the last
        // kRateWindowMax timestamps; older entries are simply not counted).
        size_t inWindow = 0;
        for (size_t i = 0; i < attemptCount; i++) {
            if (nowMs - attemptTimes[i] < kRateWindowMs)
                inWindow++;
        }
        if (inWindow >= kRateWindowMax)
            return false;
        attemptTimes[attemptHead] = nowMs;
        attemptHead = (attemptHead + 1) % kRateWindowMax;
        if (attemptCount < kRateWindowMax)
            attemptCount++;
        lastAttemptMs = nowMs;
        hasLastAttempt = true;
        return true;
    }

    // Semantic acceptance, in a fixed order: boot identity → exact
    // duplicate → sequence floor → freshness gate → state context.
    // nowMs is the device's monotonic receipt time; the TTL counts from it.
    LineVerdict accept(const LineSubmission &sub, const LineContext &ctx, uint32_t nowMs) {
        std::lock_guard<std::mutex> guard(mutex);
        if (sub.bootId != ctx.bootId || sub.bootId != currentBootId)
            return LineVerdict::StaleContext;
        if (hasAccepted && sub.lineSeq == floorSeq && std::strcmp(sub.requestId, lastAcceptedRequestId) == 0)
            return LineVerdict::Duplicate;
        if (sub.lineSeq <= floorSeq && hasAccepted)
            return LineVerdict::OutOfOrder;
        if (!ctx.controllerConnected || ctx.stale)
            return LineVerdict::StaleData;
        if (sub.stateSeq != ctx.stateSeq)
            return LineVerdict::StaleContext;
        stored.present = true;
        copyStr(stored.text, sizeof(stored.text), sub.text);
        copyStr(stored.requestId, sizeof(stored.requestId), sub.requestId);
        stored.lineSeq = sub.lineSeq;
        stored.bootId = sub.bootId;
        stored.stateSeq = sub.stateSeq;
        stored.receivedAtMs = nowMs;
        stored.ttlMs = sub.ttlS * 1000u;
        floorSeq = sub.lineSeq;
        copyStr(lastAcceptedRequestId, sizeof(lastAcceptedRequestId), sub.requestId);
        hasAccepted = true;
        return LineVerdict::Accepted;
    }

    // UI-thread read: copies the stored line out iff it is present,
    // unexpired (monotonic receipt clock) and still bound to the current
    // published context on fresh data. Anything else is dropped on the
    // spot (lazy invalidation) and reported as absent.
    bool copyValidLine(uint32_t nowMs, const LineContext &ctx, char *out, size_t outCap) {
        std::lock_guard<std::mutex> guard(mutex);
        if (!stored.present)
            return false;
        if (!lineStillValid(nowMs, ctx)) {
            stored = StoredLine{};
            return false;
        }
        if (out != nullptr && outCap > 0)
            copyStr(out, outCap, stored.text);
        return true;
    }

    // Plugin-loop servicing: active invalidation (§6.2) — a stored line is
    // dropped immediately when its context stops matching the published
    // snapshot, when the data goes stale / the link drops, when the local
    // WARNING presentation takes over, or when the remote-line feature is
    // disabled. The sequence floor is NOT touched: invalidation never
    // lowers it.
    void service(uint32_t nowMs, const LineContext &ctx, MuseAvatarState avatarState, bool remoteEnabled) {
        std::lock_guard<std::mutex> guard(mutex);
        if (!stored.present)
            return;
        if (!remoteEnabled || avatarState == MuseAvatarState::WARNING || !lineStillValid(nowMs, ctx))
            stored = StoredLine{};
    }

    // Settings turned the feature off (§6.2): drop any stored line.
    void clear() {
        std::lock_guard<std::mutex> guard(mutex);
        stored = StoredLine{};
    }

    bool hasLine() const {
        std::lock_guard<std::mutex> guard(mutex);
        return stored.present;
    }

    uint32_t floor() const {
        std::lock_guard<std::mutex> guard(mutex);
        return floorSeq;
    }

  private:
    struct StoredLine {
        bool present = false;
        char text[kLineTextMax + 1] = "";
        char requestId[kRequestIdMax + 1] = "";
        uint32_t lineSeq = 0;
        uint32_t bootId = 0;
        uint32_t stateSeq = 0;
        uint32_t receivedAtMs = 0;
        uint32_t ttlMs = 0;
    };

    static void copyStr(char *dst, size_t cap, const char *src) {
        if (cap == 0)
            return;
        if (src == nullptr)
            src = "";
        std::strncpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
    }

    // Caller holds the mutex and has checked stored.present.
    bool lineStillValid(uint32_t nowMs, const LineContext &ctx) const {
        if (nowMs - stored.receivedAtMs >= stored.ttlMs) // unsigned: wrap-safe
            return false;
        if (stored.bootId != ctx.bootId || stored.stateSeq != ctx.stateSeq)
            return false;
        if (!ctx.controllerConnected || ctx.stale)
            return false;
        return true;
    }

    mutable std::mutex mutex;
    uint32_t currentBootId = 0;
    StoredLine stored;
    uint32_t floorSeq = 0;
    char lastAcceptedRequestId[kRequestIdMax + 1] = "";
    bool hasAccepted = false;
    // Rate-limit bookkeeping.
    uint32_t attemptTimes[kRateWindowMax] = {};
    size_t attemptHead = 0;
    size_t attemptCount = 0;
    uint32_t lastAttemptMs = 0;
    bool hasLastAttempt = false;
};

// ---------------------------------------------------------------------------
// Render precedence (§6.2), evaluated on the UI thread
// ---------------------------------------------------------------------------

enum class BubbleSource : uint8_t {
    None,         // no bubble text (e.g. SLEEP with nothing to say)
    LocalWarning, // local warning presentation wins over everything remote
    LocalDone,    // DONE carries local measured data; remote never displaces
    Remote,       // a valid stored remote line
    LocalTable,   // Phase 1 local line tables
};

inline BubbleSource decideBubbleSource(MuseAvatarState state, bool hasValidRemote) {
    if (state == MuseAvatarState::HIDDEN) // hide rules outrank everything
        return BubbleSource::None;
    if (state == MuseAvatarState::WARNING)
        return BubbleSource::LocalWarning;
    if (state == MuseAvatarState::DONE)
        return BubbleSource::LocalDone;
    if (hasValidRemote)
        return BubbleSource::Remote;
    if (state == MuseAvatarState::SLEEP)
        return BubbleSource::None;
    return BubbleSource::LocalTable;
}

} // namespace MuseRemote

// The two shared instances: driven by MuseAvatarPlugin, served by
// WebUIPlugin's /api/muse/* routes.
extern MuseRemote::StatePublisher museStatePublisher;
extern MuseRemote::LineMailbox museLineMailbox;

#endif // MUSE_REMOTE_H
