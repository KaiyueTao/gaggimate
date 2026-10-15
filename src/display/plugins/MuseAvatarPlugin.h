#ifndef MUSEAVATARPLUGIN_H
#define MUSEAVATARPLUGIN_H

#include "MuseLines.h"
#include <Arduino.h>
#include <atomic>
#include <display/core/Plugin.h>
#ifndef GAGGIMATE_HEADLESS // the headless build has no LVGL at all
#include <lvgl.h>
#endif
#include <mutex>

// Phase 1 of the Muse display project (see REQUIREMENTS.md §4 in
// workspace/gaggimate-muse-display): a state-driven animated Muse character
// on an LVGL top-layer overlay, with speech bubbles from a local line table.
//
// Threading: loop() runs on the plugin task and only *computes* machine state
// into the atomics below (same read pattern as LedControlPlugin); it makes no
// LVGL call at all. LVGL 8.4 takes no internal locks, so even lv_async_call()
// from the plugin task would race the UI task's timer-list walk — overlay
// creation therefore happens on the UI task: loop() posts an atomic create
// request once the machine is ready, and DefaultUI fires a "ui:loop"
// PluginManager event on the UI task every pass, whose handler consumes the
// request and creates the overlay there. Afterwards the objects are only
// touched from the LVGL timer callback, i.e. on the UI task as well. There is
// deliberately no LVGL mutex: with no cross-task LVGL caller left, none is
// needed. The plugin is read-only: it never sends commands to the controller.
class MuseAvatarPlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

  private:
    // --- state computation (plugin task) ---
    void updateState();
    MuseAvatarState computeState();
    void setSnapshot(MuseAvatarState state, const String &warningText);

#ifndef GAGGIMATE_HEADLESS
    // --- overlay view (UI task only: "ui:loop" event handler + lv_timer) ---
    void maybeCreateOverlay();
    static void overlayTimerCb(lv_timer_t *timer);
    void createOverlay();
    void renderOverlay(uint32_t tick);
#endif

    Controller *controller = nullptr;
    bool initialized = false;      // controller:ready seen
    bool overlayRequested = false; // overlay creation posted to the UI task

    unsigned long lastUpdate = 0;
    unsigned long lastLineChange = 0;
    std::atomic<uint32_t> lineRotation{0};

    // Last-process bookkeeping for the DONE celebration window. The finished
    // shot is identified by value — its start/finish timestamps — never by
    // process pointer: the object is deleted by other tasks once the shot is
    // over, and a later BrewProcess can reuse the same heap address, which a
    // pointer comparison would mistake for "already shown" (GM-147 class of
    // bug). Only primitive values are ever copied out of the process lock.
    unsigned long doneShownStarted = 0;
    unsigned long doneShownFinished = 0;
    unsigned long doneAt = 0;
    int doneShotSeconds = 0;
    int doneShotVolume = 0;
    // VolumetricMeasurementSource (as uint8_t; 0 = INACTIVE) that produced
    // doneShotVolume, captured under the process lock together with it. The
    // DONE bubble renders "g" for scale sources and "ml" for flow estimation.
    uint8_t doneShotVolumeSource = 0;

    // Snapshot shared with the LVGL timer callback.
    std::atomic<uint8_t> snapState{static_cast<uint8_t>(MuseAvatarState::HIDDEN)};
    std::atomic<bool> snapBubbles{true};
    std::atomic<int> snapShotSeconds{0};
    std::atomic<int> snapShotVolume{0};
    std::atomic<uint8_t> snapShotVolumeSource{0}; // VolumetricMeasurementSource; 0 = INACTIVE
    std::mutex warningMutex;
    char warningText[48] = "";

    // Written by the plugin task, consumed by the "ui:loop" handler on the UI
    // task: the one cross-task signal for overlay creation (no LVGL involved).
    std::atomic<bool> overlayCreateRequested{false};

#ifndef GAGGIMATE_HEADLESS
    bool overlayCreated = false; // UI task only

    // LVGL objects (UI task only).
    lv_obj_t *root = nullptr; // top-layer container
    lv_obj_t *img = nullptr;  // the Muse sprite (lv_img); pose follows state
    lv_obj_t *bubble = nullptr; // speech bubble container
    lv_obj_t *bubbleLabel = nullptr;
    const lv_img_dsc_t *renderedPose = nullptr; // sprite currently shown
    MuseAvatarState renderedState = MuseAvatarState::HIDDEN;
    char renderedBubble[64] = "";
    bool compactLayout = false; // status screen: smaller, lower-left, clear of the shot readouts
    bool layoutApplied = false;
#endif
};

#endif // MUSEAVATARPLUGIN_H
