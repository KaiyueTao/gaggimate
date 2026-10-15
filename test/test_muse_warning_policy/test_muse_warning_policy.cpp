// Unit tests: Muse WARNING-pose trigger policy (MuseWarningPolicy.h).
//
// Regression suite for the muse-1.2 hardware finding: the worried pose was
// pinned almost permanently on a real machine because the plugin triggered
// on ANY active warning at WARN level — and the temperature warning is
// active for the whole heat-up (temperature not yet stable, default WARN),
// while the flush reminder is a persistent maintenance notice. The
// simulator never activates warnings, so the old rule looked fine there.
//
// The user-approved muse-1.3 policy pinned here: the pose is only for real
// faults — a warning triggers when it is active AND (its level is ERROR OR
// it is a hard fault that ships below ERROR: WARNING_WATER / WARNING_SWITCH,
// "machine physically cannot brew" conditions, active regardless of level).
//
// Type indices below mirror enum WarningType (WarningManager.h); levels
// mirror WarningLevel (Settings.h): IGNORE=0, WARN=1, ERROR=2, with the
// Settings.h defaults: water/flush/switch/scaleConnected/temperature/
// backflush/descaling = WARN, scaleBattery = ERROR.
//
// Host-side — pio test -e native -f test_muse_warning_policy.

#include <unity.h>

#include <initializer_list>

#include <display/plugins/MuseWarningPolicy.h>

namespace {

constexpr int IGNORE = 0;
constexpr int WARN = 1;
constexpr int ERROR = 2;

// WarningType indices (WarningManager.h order).
constexpr int WATER = 0;
constexpr int FLUSH = 1;
constexpr int SWITCH = 2;
constexpr int SCALE_CONNECTED = 3;
constexpr int SCALE_BATTERY = 4;
constexpr int TEMPERATURE = 5;
constexpr int BACKFLUSH = 6;
constexpr int DESCALING = 7;
constexpr int COUNT = 8;

struct Warnings {
    bool active[COUNT] = {false};
    int level[COUNT] = {IGNORE};
    bool any() const { return MuseWarningPolicy::anyTrigger(active, level, COUNT); }
    int first() const { return MuseWarningPolicy::firstTrigger(active, level, COUNT); }
};

// Factory defaults from Settings.h.
Warnings defaults() {
    Warnings w;
    w.level[WATER] = WARN;
    w.level[FLUSH] = WARN;
    w.level[SWITCH] = WARN;
    w.level[SCALE_CONNECTED] = WARN;
    w.level[SCALE_BATTERY] = ERROR;
    w.level[TEMPERATURE] = WARN;
    w.level[BACKFLUSH] = WARN;
    w.level[DESCALING] = WARN;
    return w;
}

} // namespace

// Nothing active: never triggers, whatever the levels say.
void test_nothing_active_never_triggers() {
    Warnings w = defaults();
    TEST_ASSERT_FALSE(w.any());
    TEST_ASSERT_EQUAL_INT(-1, w.first());
}

// The incident scenario at factory defaults: heating up (temperature not
// stable, WARN) with the flush reminder also active — the exact combination
// from the user's hardware photos. Neither may trigger the pose.
void test_heatup_and_flush_at_defaults_do_not_trigger() {
    Warnings w = defaults();
    w.active[TEMPERATURE] = true;
    w.active[FLUSH] = true;
    TEST_ASSERT_FALSE(w.any());
}

// Every background notice at its default WARN level stays background, each
// on its own: flush, scale link, temperature, backflush, descaling.
void test_warn_level_background_notices_do_not_trigger() {
    for (int type : {FLUSH, SCALE_CONNECTED, TEMPERATURE, BACKFLUSH, DESCALING}) {
        Warnings w = defaults();
        w.active[type] = true;
        TEST_ASSERT_FALSE_MESSAGE(w.any(), "WARN-level notice triggered the pose");
    }
}

// Any type escalated to ERROR triggers — including the maintenance
// reminders, if the user configures them that way.
void test_error_level_always_triggers() {
    for (int type = 0; type < COUNT; type++) {
        Warnings w = defaults();
        w.level[type] = ERROR;
        w.active[type] = true;
        TEST_ASSERT_TRUE_MESSAGE(w.any(), "ERROR-level warning did not trigger");
        TEST_ASSERT_EQUAL_INT(type, w.first());
    }
}

// The verification experiment of muse-1.3 in unit form: the temperature
// warning, active throughout heat-up, triggers only once its level is
// raised to ERROR — and stops triggering when set back to WARN.
void test_temperature_toggle_warn_error_warn() {
    Warnings w = defaults();
    w.active[TEMPERATURE] = true;
    TEST_ASSERT_FALSE(w.any());
    w.level[TEMPERATURE] = ERROR;
    TEST_ASSERT_TRUE(w.any());
    TEST_ASSERT_EQUAL_INT(TEMPERATURE, w.first());
    w.level[TEMPERATURE] = WARN;
    TEST_ASSERT_FALSE(w.any());
}

// Hard faults that ship at WARN level trigger on being active: an empty
// water tank and a steam switch left on are "cannot brew" conditions.
void test_hard_faults_trigger_at_default_warn_level() {
    for (int type : {WATER, SWITCH}) {
        Warnings w = defaults();
        w.active[type] = true;
        TEST_ASSERT_TRUE_MESSAGE(w.any(), "hard fault did not trigger at WARN");
        TEST_ASSERT_EQUAL_INT(type, w.first());
    }
}

// Hard faults trigger regardless of the configured level (even IGNORE):
// activity alone means the machine cannot brew.
void test_hard_faults_trigger_regardless_of_level() {
    for (int type : {WATER, SWITCH}) {
        Warnings w = defaults();
        w.level[type] = IGNORE;
        w.active[type] = true;
        TEST_ASSERT_TRUE_MESSAGE(w.any(), "hard fault did not trigger at IGNORE");
    }
}

// Non-faults never trigger at IGNORE level either.
void test_ignore_level_never_triggers_for_non_faults() {
    for (int type : {FLUSH, SCALE_CONNECTED, SCALE_BATTERY, TEMPERATURE, BACKFLUSH, DESCALING}) {
        Warnings w = defaults();
        w.level[type] = IGNORE;
        w.active[type] = true;
        TEST_ASSERT_FALSE_MESSAGE(w.any(), "IGNORE-level notice triggered the pose");
    }
}

// The one type that ships at ERROR (scale battery) triggers out of the box
// once active — the ERROR rule covers it without a hard-fault entry.
void test_scale_battery_default_error_triggers() {
    Warnings w = defaults();
    w.active[SCALE_BATTERY] = true;
    TEST_ASSERT_TRUE(w.any());
    TEST_ASSERT_EQUAL_INT(SCALE_BATTERY, w.first());
}

// With several triggering warnings, firstTrigger reports the lowest index
// (WarningManager order), which is the label the bubble leads with.
void test_first_trigger_is_lowest_triggering_index() {
    Warnings w = defaults();
    w.active[TEMPERATURE] = true;
    w.level[TEMPERATURE] = ERROR;
    w.active[WATER] = true; // hard fault at WARN
    TEST_ASSERT_EQUAL_INT(WATER, w.first());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_nothing_active_never_triggers);
    RUN_TEST(test_heatup_and_flush_at_defaults_do_not_trigger);
    RUN_TEST(test_warn_level_background_notices_do_not_trigger);
    RUN_TEST(test_error_level_always_triggers);
    RUN_TEST(test_temperature_toggle_warn_error_warn);
    RUN_TEST(test_hard_faults_trigger_at_default_warn_level);
    RUN_TEST(test_hard_faults_trigger_regardless_of_level);
    RUN_TEST(test_ignore_level_never_triggers_for_non_faults);
    RUN_TEST(test_scale_battery_default_error_triggers);
    RUN_TEST(test_first_trigger_is_lowest_triggering_index);
    return UNITY_END();
}
