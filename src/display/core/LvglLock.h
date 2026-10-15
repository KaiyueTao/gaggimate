#ifndef LVGLLOCK_H
#define LVGLLOCK_H

#include <mutex>

// The one mutex that serializes every entry into LVGL.
//
// This build uses LVGL 8.4 with no OS abstraction (no LV_USE_OS in
// lv_conf.h), so LVGL itself takes no locks: lv_task_handler() walking the
// timer list on the UI task races with any LVGL call made from another task.
// Even lv_async_call() is not exempt in 8.4 — it allocates from the LVGL
// memory pool and creates a timer on the same unlocked list, so calling it
// from a plugin task while the UI task is inside lv_task_handler() can
// corrupt both structures.
//
// Rule: the UI task holds this mutex around its LVGL servicing
// (ui_tick() + lv_task_handler() in DefaultUI), and any other task holds it
// for the duration of a one-shot LVGL call (e.g. lv_async_call()). LVGL
// callbacks (timer/event handlers) already run inside lv_task_handler(),
// i.e. under the mutex, and must not take it again in a blocking way — it is
// recursive precisely so such re-entry stays safe.
inline std::recursive_mutex &lvglMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

#endif // LVGLLOCK_H
