#include "MuseAvatarPlugin.h"
#include "MuseRemote.h"
#include "MuseSpriteFit.h"
#include "MuseWarningPolicy.h"
#include <Preferences.h>
#include <cstring>
#include <display/core/Controller.h>
#include <display/core/WarningManager.h>
#include <display/core/constants.h>
#include <display/core/process/BrewProcess.h>

namespace {
constexpr unsigned long STATE_POLL_MS = 150;
constexpr unsigned long LINE_ROTATE_MS = 6000;
constexpr unsigned long DONE_WINDOW_MS = 10000;
constexpr float READY_TEMP_TOLERANCE = 3.0f;
} // namespace

#ifndef GAGGIMATE_HEADLESS
#include "MuseSprites.h"
#include <display/ui/default/eez/eez-flow.h>
#include <display/ui/default/eez/screens.h>
#include <display/ui/default/eez/structs.h>
#include <display/ui/default/eez/vars.h>

// Layout (480x480 round panel): two variants. The standby screen keeps the
// large avatar centred in the lower half — nothing sits behind it there but
// the clock and the logo. Every content screen (brew/status/steam/water/
// grind) uses the compact avatar in the bottom-left region (muse-1.3):
// measured against the stock screens, the inner bottom-left is empty on all
// of them, while the centre carries the profile name, the shot timer/weight
// and the steam controls, and the bottom-right carries the water-status
// icon. The bubble is a caption in the same bottom band: it starts right of
// the sprite and its bottom edge sits below every content screen's action
// row, growing upward — it never crosses the centre stack (an earlier draft
// anchored it above the sprite, where it landed on the profile name / shot
// timer; the band below the action row is the only strip that is empty on
// brew, status and steam alike). On standby the bubble is always a warning
// notice and parks in the empty band between the clock and the logo
// instead.
// Disc fit (muse-1.3.1): the panel is a disc, centre (240,240), radius
// 240 — a square-screen corner placement clips. The muse-1.3 compact box
// (x 74..182, y 314..422) put its bottom-left corner at radius 246, outside
// the disc, and the character's lower-left was cut off by the round edge.
// The compact box is now derived from the disc: every corner of the sprite
// box — and of the bubble — must sit within an effective radius of 230.
// Box 96x96 at x 102..198, y 322..418: worst corner (102,418) at 225.2.
// Bubble x 202..378 (4 px right of the box), bottom edge y=420: worst
// corner (378,420) at 226.8. At 108 px wide the box's right edge (208+)
// would have pushed the bubble's bottom-right corner past 230, so the
// compact sprite shrank to 96 rather than moving back out or into the
// centre content. The top edge (322) also gains clearance over muse-1.3:
// the stock icon row ends ~19 px above the old 314 edge.
// The sprite is fitted into a box (per layout) with lv_img zoom; all poses
// share one scale in the source assets, so the character size is
// consistent across states. Fitting alone is not enough: the img must be
// anchored by its pivot, never aligned by its object rect — in LVGL 8.4
// VIRTUAL size mode that rect is the *untransformed* source size, so
// aligning it placed the zoomed artwork's head outside the root's clip
// (the muse-1.3/1.3.1 head-crop; see MuseSpriteFit.h for the full story
// and the fit/anchor math, pinned by test_muse_sprite_fit).
// Layering: the overlay lives on lv_layer_top() (createOverlay), above
// every screen; renderOverlay() re-asserts the foreground order each pass
// so nothing created later on that layer can ever draw over the avatar.
namespace {
// Large layout (standby) fit box / root placement (align-center offsets).
constexpr int BOX_W = 168;
constexpr int BOX_H = 158;
constexpr int ROOT_Y = 128;
// Compact layout (content screens): bottom-left region, inset to fit the
// round panel's disc (see the disc-fit note above). Root offsets are
// align-center deltas: box centre (150,370) for a 96x96 box.
constexpr int BOX_W_C = 96;
constexpr int BOX_H_C = 96;
constexpr int ROOT_X_C = -90;
constexpr int ROOT_Y_C = 130;
// Speech bubble: narrow, small font, thin padding, translucent — it is a
// caption for the avatar, not a banner (muse-1.3; the old 250 px opaque
// pill covered the profile name and the shot readouts). The compact width
// keeps the bubble's corners inside the disc at its bottom-band position
// (muse-1.3.1).
constexpr int BUBBLE_W = 208;   // large layout
constexpr int BUBBLE_W_C = 176; // compact layout
constexpr int BUBBLE_PAD = 4;

void setHidden(lv_obj_t *obj, bool hidden) {
    if (hidden)
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

// State -> sprite pose. BREWING_LATE uses the celebrating pose: past 25 s
// the shot deserves a small party, and DONE already uses the espresso-cup
// pose for the finish summary.
const lv_img_dsc_t *poseForState(MuseAvatarState state) {
    switch (state) {
    case MuseAvatarState::SLEEP:
        return &muse_pose_sleep;
    case MuseAvatarState::HEATING:
        return &muse_pose_heating;
    case MuseAvatarState::READY:
        return &muse_pose_ready;
    case MuseAvatarState::BREWING:
        return &muse_pose_brewing;
    case MuseAvatarState::BREWING_LATE:
        return &muse_pose_celebrate;
    case MuseAvatarState::STEAMING:
        return &muse_pose_steaming;
    case MuseAvatarState::DONE:
        return &muse_pose_done;
    case MuseAvatarState::WARNING:
        return &muse_pose_warning;
    default:
        return &muse_pose_ready;
    }
}

// Per-state animation envelope: the worst phase the sprite fit must
// survive (see MuseSpriteFit.h). angleMax is the max |angle| of the
// phase switch in renderOverlay (0.1-degree units), up the max upward
// travel (-dy), side the max |dx|, breath the zoom added back at some
// phases (SLEEP breathing). Keep in step with that switch — the native
// suite pins the containment these numbers promise.
MuseSpriteFit::Envelope animEnvelope(MuseAvatarState state) {
    using MuseSpriteFit::Envelope;
    switch (state) {
    case MuseAvatarState::SLEEP:
        return Envelope{0, 0, 0, 4}; // breathing zoom +4
    case MuseAvatarState::HEATING:
        return Envelope{12, 2, 0, 0};
    case MuseAvatarState::READY:
        return Envelope{14, 2, 0, 0};
    case MuseAvatarState::BREWING:
        return Envelope{10, 0, 0, 0};
    case MuseAvatarState::BREWING_LATE:
        return Envelope{16, 8, 0, 0}; // hop dy=-8
    case MuseAvatarState::STEAMING:
        return Envelope{20, 2, 0, 0};
    case MuseAvatarState::DONE:
        return Envelope{0, 12, 0, 0}; // celebration hop dy=-12
    case MuseAvatarState::WARNING:
        return Envelope{0, 0, 2, 0}; // shake dx=+/-2
    default:
        return Envelope{0, 0, 0, 0};
    }
}
} // namespace
#endif // GAGGIMATE_HEADLESS

void MuseAvatarPlugin::setup(Controller *controller, PluginManager *pluginManager) {
    this->controller = controller;
    pluginManager->on("controller:ready", [this](Event const) { initialized = true; });
    // Phase 2 (§6.1): the boot identity for the state snapshot comes from a
    // persistent NVS boot counter, incremented exactly once here at
    // startup — monotonic across restarts, never a compiled-in default,
    // never reused. The counter lives in its own NVS namespace so it can
    // never collide with a user setting.
    {
        Preferences prefs;
        prefs.begin("muse", false);
        const uint32_t stored = prefs.getUInt("boot_ctr", 0);
        bootId = MuseRemote::nextBootId(stored);
        prefs.putUInt("boot_ctr", bootId);
        prefs.end();
    }
    museStatePublisher.setBootId(bootId);
    museLineMailbox.setBootId(bootId);
    // Telemetry freshness (§6.1): the Controller fires this event from its
    // sensor-data callback for every telemetry frame the controller board
    // sends — that receipt is what data_age_ms is measured from, never an
    // HTTP query or a render poll. The handler runs in the comms callback
    // context, so it does exactly one atomic store.
    pluginManager->on("pump:volume:change", [](Event const &) {
        museStatePublisher.noteTelemetry(static_cast<uint32_t>(millis()));
    });
#ifndef GAGGIMATE_HEADLESS
    // DefaultUI triggers "ui:loop" on the UI task every pass; the handler is
    // where overlay creation happens, so no LVGL call ever leaves the UI
    // task (see the class comment in the header).
    pluginManager->on("ui:loop", [this](Event const &) { maybeCreateOverlay(); });
#endif
}

void MuseAvatarPlugin::loop() {
    if (!initialized || controller == nullptr) {
        return;
    }
#ifndef GAGGIMATE_HEADLESS
    if (!overlayRequested) {
        overlayRequested = true;
        // Post the creation request only — the overlay objects are created
        // by the "ui:loop" handler on the UI task (maybeCreateOverlay), so
        // this task never touches LVGL. LVGL is up by construction there:
        // the event fires from inside DefaultUI::loop().
        overlayCreateRequested.store(true);
    }
#endif
    const unsigned long now = millis();
    if (now - lastUpdate >= STATE_POLL_MS) {
        lastUpdate = now;
        updateState();
    }
}

// ---------------------------------------------------------------------------
// State computation (plugin task; read-only, mirrors LedControlPlugin access)
// ---------------------------------------------------------------------------

void MuseAvatarPlugin::setSnapshot(MuseAvatarState state, const String &warning) {
    snapState.store(static_cast<uint8_t>(state));
    if (warning.length() > 0) {
        std::lock_guard<std::mutex> guard(warningMutex);
        strncpy(warningText, warning.c_str(), sizeof(warningText) - 1);
        warningText[sizeof(warningText) - 1] = '\0';
    }
}

MuseAvatarState MuseAvatarPlugin::computeState() {
    // Warnings override everything (§4.1 priority) — but since muse-1.3
    // only real faults count (MuseWarningPolicy): ERROR-level warnings,
    // plus the hard faults that ship at WARN level (empty water tank,
    // steam switch left on). WARN-level background notices — temperature
    // not yet stable during heat-up, the flush/backflush/descaling
    // maintenance reminders, scale link/battery chatter — no longer pin
    // the worried pose over SLEEP/HEATING on real hardware.
    const WarningManager &wm = controller->getWarnings();
    if (controller->isErrorState()) {
        setSnapshot(MuseAvatarState::WARNING, controller->getSystemStateMessage());
        return MuseAvatarState::WARNING;
    }
    {
        bool active[WARNING_TYPE_COUNT];
        int level[WARNING_TYPE_COUNT];
        for (int i = 0; i < WARNING_TYPE_COUNT; i++) {
            const auto type = static_cast<WarningType>(i);
            active[i] = wm.isActive(type);
            level[i] = wm.getLevel(type);
        }
        if (MuseWarningPolicy::anyTrigger(active, level, WARNING_TYPE_COUNT)) {
            // The bubble lists only the triggering warnings' labels, not
            // every active one (getLabels() would also name the WARN-level
            // notices that did not trigger the pose).
            String labels;
            for (int i = 0; i < WARNING_TYPE_COUNT; i++) {
                if (!MuseWarningPolicy::typeTriggers(active[i], level[i], i))
                    continue;
                if (labels.length() > 0)
                    labels += "\n";
                labels += WarningManager::label(static_cast<WarningType>(i));
            }
            const int nl = labels.indexOf('\n');
            setSnapshot(MuseAvatarState::WARNING, nl >= 0 ? labels.substring(0, nl) : labels);
            return MuseAvatarState::WARNING;
        }
    }

    const int mode = controller->getMode();
    bool brewRunning = false;
    unsigned long brewElapsedMs = 0;
    {
        // All Process derefs happen under the process lock — other tasks
        // delete processes (GM-147). Only primitive values are copied out;
        // no Process* is stored or used after the lock is released.
        std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
        Process *current = controller->getProcess();
        if (current != nullptr && current->getType() == MODE_BREW && current->isActive()) {
            brewRunning = true;
            brewElapsedMs = millis() - static_cast<BrewProcess *>(current)->processStarted;
            // A new shot is running: re-arm the DONE celebration so its
            // completion always shows, whatever its timestamps turn out to be.
            doneShownStarted = 0;
            doneShownFinished = 0;
        } else {
            Process *last = controller->getLastProcess();
            if (last != nullptr && last->getType() == MODE_BREW && last->isComplete()) {
                auto *brew = static_cast<BrewProcess *>(last);
                // Identify the finished shot by value (its start/finish
                // timestamps), not by address: the object is deleted once
                // the shot is cleared, and a later BrewProcess may reuse
                // the same heap address — a pointer comparison would then
                // mistake that new shot for the one already celebrated and
                // swallow its DONE.
                if (brew->finished > brew->processStarted &&
                    (brew->processStarted != doneShownStarted || brew->finished != doneShownFinished)) {
                    doneShownStarted = brew->processStarted;
                    doneShownFinished = brew->finished;
                    doneAt = millis();
                    doneShotSeconds = static_cast<int>((brew->finished - brew->processStarted) / 1000);
                    doneShotVolume = static_cast<int>(brew->currentVolume);
                    // The source that fed this process is still the
                    // controller's locked volumetric source at this moment:
                    // it is only reset inside clearLocked(), together with
                    // the deletion of lastProcess and under this same lock.
                    // getEffectiveScaleSource() re-takes the (recursive)
                    // process lock and returns exactly that source while a
                    // last process exists.
                    doneShotVolumeSource = static_cast<uint8_t>(controller->getEffectiveScaleSource());
                }
            }
        }
    }
    if (brewRunning) {
        return brewElapsedMs > 25000 ? MuseAvatarState::BREWING_LATE : MuseAvatarState::BREWING;
    }
    if (controller->isActive() && mode == MODE_STEAM) {
        return MuseAvatarState::STEAMING;
    }
    if (doneAt > 0 && millis() - doneAt < DONE_WINDOW_MS) {
        snapShotSeconds.store(doneShotSeconds);
        snapShotVolume.store(doneShotVolume);
        snapShotVolumeSource.store(doneShotVolumeSource);
        return MuseAvatarState::DONE;
    }
    if (mode == MODE_STANDBY) {
        return MuseAvatarState::SLEEP;
    }
    if (mode == MODE_STEAM) {
        return MuseAvatarState::STEAMING;
    }
    // Awake (brew/water/grind modes): heating until the boiler is at target.
    const float target = controller->getTargetTemp();
    if (target > 0 && controller->getCurrentTemp() < target - READY_TEMP_TOLERANCE) {
        return MuseAvatarState::HEATING;
    }
    return MuseAvatarState::READY;
}

void MuseAvatarPlugin::updateState() {
    const Settings &settings = controller->getSettings();
    const bool enabled = settings.isMuseEnabled();
    MuseAvatarState state = MuseAvatarState::HIDDEN;
    if (!enabled) {
        snapState.store(static_cast<uint8_t>(MuseAvatarState::HIDDEN));
    } else {
        snapBubbles.store(settings.isMuseBubbles());
        state = computeState();
        if (state != MuseAvatarState::WARNING) {
            setSnapshot(state, "");
        }
        if (millis() - lastLineChange >= LINE_ROTATE_MS) {
            lastLineChange = millis();
            lineRotation.fetch_add(1);
        }
    }
    // Phase 2: the snapshot is published even with the avatar disabled —
    // GET /api/muse/state reports the machine, and avatar_state simply
    // reads HIDDEN then. Mailbox servicing runs on the same poll: a stored
    // remote line is invalidated as soon as its context, the data
    // freshness, or the WARNING presentation says it must go (§6.2).
    publishSnapshot(state);
    const uint32_t nowMs = static_cast<uint32_t>(millis());
    const auto snap = museStatePublisher.getSnapshot(nowMs);
    museLineMailbox.service(nowMs, MuseRemote::lineContextOf(snap), state, enabled && settings.isMuseRemoteLines());
}

void MuseAvatarPlugin::readBrewActivity(bool &running, unsigned long &elapsedMs) {
    running = false;
    elapsedMs = 0;
    // Same discipline as computeState(): process derefs only under the
    // process lock, primitive values only copied out (GM-147).
    std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
    Process *current = controller->getProcess();
    if (current != nullptr && current->getType() == MODE_BREW && current->isActive()) {
        running = true;
        elapsedMs = millis() - static_cast<BrewProcess *>(current)->processStarted;
    }
}

void MuseAvatarPlugin::publishSnapshot(MuseAvatarState avatarState) {
    MuseRemote::StateInputs in;
    in.mode = controller->getMode();
    in.boilerC = controller->getCurrentTemp();
    in.targetC = controller->getTargetTemp();
    bool brewRunning = false;
    unsigned long brewElapsedMs = 0;
    readBrewActivity(brewRunning, brewElapsedMs);
    in.brewActive = brewRunning;
    in.brewElapsedS = brewRunning ? static_cast<uint32_t>(brewElapsedMs / 1000) : 0;
    in.avatarState = avatarState;
    const WarningManager &wm = controller->getWarnings();
    for (int i = 0; i < WARNING_TYPE_COUNT && i < MuseRemote::kMaxWarnings; i++) {
        const auto type = static_cast<WarningType>(i);
        in.warnActive[i] = wm.isActive(type);
        in.warnLevel[i] = wm.getLevel(type);
        in.warnId[i] = WarningManager::key(type);
        in.warnText[i] = WarningManager::label(type);
    }
    in.controllerConnected = controller->getClientController()->isConnected();
    museStatePublisher.update(in, static_cast<uint32_t>(millis()));
}

#ifndef GAGGIMATE_HEADLESS
// ---------------------------------------------------------------------------
// Overlay view (UI task only)
// ---------------------------------------------------------------------------

void MuseAvatarPlugin::maybeCreateOverlay() {
    // Runs on the UI task (the "ui:loop" PluginManager event is triggered
    // from DefaultUI::loop()). Consumes the plugin task's creation request;
    // a no-op on every pass until the request is posted, and createOverlay()
    // itself stays a one-shot via overlayCreated.
    if (overlayCreateRequested.exchange(false)) {
        createOverlay();
    }
}

void MuseAvatarPlugin::overlayTimerCb(lv_timer_t *timer) {
    auto *self = static_cast<MuseAvatarPlugin *>(timer->user_data);
    self->renderOverlay(lv_tick_get());
}

void MuseAvatarPlugin::createOverlay() {
    if (overlayCreated) {
        return;
    }
    overlayCreated = true;

    root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(root, BOX_W, BOX_H);
    lv_obj_align(root, LV_ALIGN_CENTER, 0, ROOT_Y);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    img = lv_img_create(root);
    lv_img_set_antialias(img, true);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

    bubble = lv_obj_create(lv_layer_top());
    lv_obj_set_width(bubble, BUBBLE_W);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_align(bubble, LV_ALIGN_CENTER, 0, 32);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(bubble, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(bubble, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bubble, BUBBLE_PAD, LV_PART_MAIN);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    bubbleLabel = makeLabel(bubble, &lv_font_montserrat_12, lv_color_hex(0x26221F));
    lv_label_set_text(bubbleLabel, "");
    lv_obj_set_width(bubbleLabel, BUBBLE_W - 2 * BUBBLE_PAD);
    lv_label_set_long_mode(bubbleLabel, LV_LABEL_LONG_DOT);
    lv_obj_align(bubbleLabel, LV_ALIGN_CENTER, 0, 0);

    lv_timer_create(overlayTimerCb, 120, this);
    renderOverlay(0);
}

void MuseAvatarPlugin::renderOverlay(uint32_t tick) {
    if (root == nullptr) {
        return;
    }
    const auto state = static_cast<MuseAvatarState>(snapState.load());

    // Hide entirely on menu/info/profile screens so nothing blocks operation,
    // and when the avatar is disabled (stock UI, §4.3).
    const int16_t screen = eez_flow_get_current_screen();
    const bool onMainScreen = screen == SCREEN_ID_STANDBY_SCREEN || screen == SCREEN_ID_BREW_SCREEN ||
                              screen == SCREEN_ID_STATUS_SCREEN || screen == SCREEN_ID_STEAM_SCREEN ||
                              screen == SCREEN_ID_WATER_SCREEN || screen == SCREEN_ID_GRIND_SCREEN;
    // The brew screen hosts the Adjust Profile editor (temperature /
    // volumetric target) as a sub-view under the same EEZ screen id;
    // DefaultUI flags it through the UI-flags flow variable. It is a
    // profile screen for our purposes: hide there too (muse-1.3 matrix
    // finding — the screen-id whitelist alone cannot see it).
    bool adjustmentsOpen = false;
    if (screen == SCREEN_ID_BREW_SCREEN) {
        UIFlagsValue flags(eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_UI_FLAGS));
        if (flags)
            adjustmentsOpen = flags.brew_adjustments();
    }
    const bool visible = state != MuseAvatarState::HIDDEN && onMainScreen && !adjustmentsOpen;
    setHidden(root, !visible);
    if (!visible) {
        lv_obj_add_flag(bubble, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    // The overlay sits on lv_layer_top(), above every screen — but within
    // a layer, draw order follows creation order, so anything created on
    // the top layer after the overlay would paint over the avatar.
    // Re-assert the order (sprite container, then bubble on top of it)
    // whenever it has drifted; today the plugin is the layer's only user,
    // so this is a cheap no-op guard for future stock/plugin users.
    {
        lv_obj_t *topLayer = lv_layer_top();
        const uint32_t siblings = lv_obj_get_child_cnt(topLayer);
        if (siblings >= 2 && (lv_obj_get_index(root) != static_cast<int>(siblings) - 2 ||
                              lv_obj_get_index(bubble) != static_cast<int>(siblings) - 1)) {
            lv_obj_move_foreground(root);
            lv_obj_move_foreground(bubble);
        }
    }

    // Only the standby screen uses the large layout; every content screen
    // uses the compact corner layout (muse-1.3, see the layout note at the
    // top of this file). Both use the same sprite asset — the compact
    // layout is just a smaller zoom.
    const bool compact = screen != SCREEN_ID_STANDBY_SCREEN;
    const int boxW = compact ? BOX_W_C : BOX_W;
    const int boxH = compact ? BOX_H_C : BOX_H;
    if (compact != compactLayout || !layoutApplied) {
        compactLayout = compact;
        layoutApplied = true;
        lv_obj_set_size(root, boxW, boxH);
        lv_obj_set_width(bubble, compact ? BUBBLE_W_C : BUBBLE_W);
        lv_obj_set_width(bubbleLabel, (compact ? BUBBLE_W_C : BUBBLE_W) - 2 * BUBBLE_PAD);
        renderedPose = nullptr; // force pose re-apply at the new zoom
    }
    const int rootX = compact ? ROOT_X_C : 0;
    const int rootY = compact ? ROOT_Y_C : ROOT_Y;
    lv_obj_align(root, LV_ALIGN_CENTER, rootX, rootY);
    // Bubble placement (muse-1.3, see the layout note at the top): on
    // content screens it captions the sprite from the same bottom band —
    // left edge 4 px right of the sprite box, bottom edge at y=420 (below
    // the brew play row, the status pause button and the steam glyph,
    // above the bottom-centre chevron), growing upward as its content
    // height changes. On standby it parks in the empty band between the
    // clock (ends ~y=105) and the logo (starts ~y=222).
    lv_obj_update_layout(bubble);
    const int bubbleH = lv_obj_get_height(bubble);
    if (compact) {
        const int bubbleW = lv_obj_get_width(bubble);
        const int bubbleX = rootX + boxW / 2 + 4 + bubbleW / 2;
        lv_obj_align(bubble, LV_ALIGN_CENTER, bubbleX, 420 - bubbleH / 2 - 240);
    } else {
        lv_obj_align(bubble, LV_ALIGN_CENTER, 0, -72);
    }

    // --- sprite pose + transform animation (tick-driven, ~8.3 fps) ---
    const lv_img_dsc_t *pose = poseForState(state);
    if (pose != renderedPose) {
        renderedPose = pose;
        lv_img_set_src(img, pose);
        lv_img_set_pivot(img, pose->header.w / 2, pose->header.h / 2);
    }
    const uint32_t phase = tick / 120; // ~8.3 fps animation steps
    // Fit so the pose's AABB survives the state's worst animation phase,
    // then apply this phase's transform (MuseSpriteFit.h). The SLEEP
    // breathing rides on top of the fitted base; the fit already reserved
    // that headroom in the envelope.
    int zoom = MuseSpriteFit::fitZoom(pose->header.w, pose->header.h, boxW, boxH, animEnvelope(state));
    int angle = 0; // 0.1 degree units
    int dx = 0;
    int dy = 0;
    switch (state) {
    case MuseAvatarState::SLEEP:
        zoom += (phase % 10) < 5 ? 0 : 4; // slow breathing
        break;
    case MuseAvatarState::HEATING:
        dy = (phase % 8) < 4 ? 0 : -2;
        angle = (phase % 12) < 6 ? -12 : 12;
        break;
    case MuseAvatarState::READY:
        dy = (phase % 8) < 4 ? 0 : -2; // gentle idle bob + sway
        angle = (phase % 16) < 8 ? -14 : 14;
        break;
    case MuseAvatarState::BREWING:
        angle = (phase % 10) < 5 ? -10 : 10; // focused, steady
        break;
    case MuseAvatarState::BREWING_LATE:
        dy = phase % 4 == 0 ? -8 : 0; // little hop of excitement
        angle = (phase % 8) < 4 ? -16 : 16;
        break;
    case MuseAvatarState::STEAMING:
        dy = (phase % 8) < 4 ? 0 : -2;
        angle = (phase % 12) < 6 ? -20 : 20;
        break;
    case MuseAvatarState::DONE:
        dy = phase % 4 == 0 ? -12 : 0; // celebration jump
        break;
    case MuseAvatarState::WARNING:
        dx = phase % 2 == 0 ? -2 : 2; // worried shake
        break;
    default:
        break;
    }
    lv_img_set_zoom(img, zoom);
    lv_img_set_angle(img, angle);
    // Anchor the drawn sprite by its pivot (MuseSpriteFit.h): the pivot
    // goes where the AABB's bottom edge is boxH + dy (feet planted, hops
    // rise) and its centre is boxW/2 + dx. Aligning the object rect
    // instead (LV_ALIGN_BOTTOM_MID) is what cropped the head in the
    // compact layout — the rect is the untransformed source size.
    {
        const int px = pose->header.w / 2;
        const int py = pose->header.h / 2;
        int tx = 0;
        int ty = 0;
        MuseSpriteFit::pivotTarget(pose->header.w, pose->header.h, px, py, zoom, angle, boxW, boxH, dx, dy, tx, ty);
        lv_obj_set_pos(img, tx - px, ty - py);
    }

    // --- speech bubble ---
    // Text selection follows the §6.2 precedence via
    // MuseRemote::decideBubbleSource (pinned by test_muse_remote):
    // WARNING and DONE always speak locally; a valid stored remote line
    // (Phase 2) outranks the local tables everywhere else, SLEEP included;
    // the local tables are the fallback. A remote line is copied out of
    // the mailbox under its lock into this UI-owned buffer first — the
    // local sources' static-storage lifetime does not extend to it (§6.3).
    char text[64] = "";
    if (snapBubbles.load()) {
        char remoteText[MuseRemote::kLineTextMax + 1] = "";
        bool hasRemote = false;
        {
            const uint32_t nowMs = static_cast<uint32_t>(millis());
            const auto snap = museStatePublisher.getSnapshot(nowMs);
            hasRemote = museLineMailbox.copyValidLine(nowMs, MuseRemote::lineContextOf(snap), remoteText, sizeof(remoteText));
        }
        const MuseRemote::BubbleSource source = MuseRemote::decideBubbleSource(state, hasRemote);
        if (source == MuseRemote::BubbleSource::LocalWarning) {
            std::lock_guard<std::mutex> guard(warningMutex);
            strncpy(text, warningText, sizeof(text) - 1);
        } else if (source == MuseRemote::BubbleSource::Remote) {
            strncpy(text, remoteText, sizeof(text) - 1);
        } else if (source == MuseRemote::BubbleSource::LocalDone) {
            snprintf(text, sizeof(text), "Done! %ds shot", snapShotSeconds.load());
            const int volume = snapShotVolume.load();
            if (volume > 0) {
                // BrewProcess.currentVolume is a weight in grams when it was
                // fed by a scale and a pumped-volume estimate in ml when it
                // was fed by flow estimation — label it with the unit of the
                // source captured alongside it, never a hardcoded one. An
                // indeterminate source shows the bare number rather than a
                // wrong unit.
                const char *unit = "";
                switch (static_cast<VolumetricMeasurementSource>(snapShotVolumeSource.load())) {
                case VolumetricMeasurementSource::BLUETOOTH:
                case VolumetricMeasurementSource::HARDWARE:
                    unit = " g";
                    break;
                case VolumetricMeasurementSource::FLOW_ESTIMATION:
                    unit = " ml";
                    break;
                default:
                    unit = "";
                    break;
                }
                char extra[24];
                snprintf(extra, sizeof(extra), ", %d%s", volume, unit);
                strncat(text, extra, sizeof(text) - strlen(text) - 1);
            }
        } else if (source == MuseRemote::BubbleSource::LocalTable) {
            strncpy(text, museLocalLines.getLine(state, lineRotation.load()), sizeof(text) - 1);
        }
    }
    text[sizeof(text) - 1] = '\0';
    const bool showBubble = text[0] != '\0';
    setHidden(bubble, !showBubble);
    if (showBubble && strcmp(text, renderedBubble) != 0) {
        strncpy(renderedBubble, text, sizeof(renderedBubble) - 1);
        renderedBubble[sizeof(renderedBubble) - 1] = '\0';
        lv_label_set_text(bubbleLabel, text);
    }
    renderedState = state;
}
#endif // GAGGIMATE_HEADLESS
