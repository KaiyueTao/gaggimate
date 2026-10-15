#ifndef MUSE_WARNING_POLICY_H
#define MUSE_WARNING_POLICY_H

// Trigger policy for the Muse WARNING (worried) pose — muse-1.3.
//
// Up to muse-1.2 the pose fired on ANY active warning at WARN level or
// above (WarningManager::hasWarning() || hasError()). On real hardware
// that pins the worried face almost permanently: the temperature warning
// is active whenever the temperature has not stabilised (i.e. the whole
// heat-up, at its default WARN level), and the flush reminder is a
// persistent maintenance notice — both outranked SLEEP and HEATING.
//
// Policy (user-approved): the worried pose is only for real faults.
// A warning type triggers the pose when it is active AND either
//   - its configured level is ERROR, or
//   - it is a hard-fault type (below) that ships below ERROR: being
//     active at all then means the machine physically cannot brew —
//     the water tank is empty, or the steam switch was left on.
// Everything else stays background information at WARN level: the stock
// UI still shows those warnings, Muse just does not wear them.
//
// Type indices mirror enum WarningType (WarningManager.h) and the level
// constant mirrors WARNING_LEVEL_ERROR (Settings.h). This header is
// deliberately free of Arduino/WarningManager includes so the native
// test suite can pin the policy directly (test_muse_warning_policy).

namespace MuseWarningPolicy {

constexpr int kErrorLevel = 2; // WARNING_LEVEL_ERROR

// Hard faults that trigger on being active, regardless of the configured
// level: WARNING_WATER (0, "Water tank low") and WARNING_SWITCH
// (2, "Steam switch is on"). Both default to WARNING_LEVEL_WARN in
// Settings.h, so the ERROR-only rule alone would never surface them.
constexpr bool isHardFault(int type) { return type == 0 || type == 2; }

inline bool typeTriggers(bool active, int level, int type) {
    return active && (level == kErrorLevel || isHardFault(type));
}

// Index of the first triggering type in [0, count), or -1 when none does.
inline int firstTrigger(const bool *active, const int *level, int count) {
    for (int i = 0; i < count; i++) {
        if (typeTriggers(active[i], level[i], i))
            return i;
    }
    return -1;
}

inline bool anyTrigger(const bool *active, const int *level, int count) {
    return firstTrigger(active, level, count) >= 0;
}

} // namespace MuseWarningPolicy

#endif // MUSE_WARNING_POLICY_H
