// SPDX-License-Identifier: GPL-3.0-or-later
//
// Android app core: one render thread owning LVGL, the operating screen,
// input and the session. The NativeActivity callbacks (main thread) only
// hand over windows, input queues and lifecycle events.
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <android/input.h>
#include <android/native_window.h>

#include "core/session.h"

//
// Before app_start (from the Java side, JNI)
//

void app_set_profile(const struct profile *p, bool mic_allowed);
void app_set_insets(int32_t top, int32_t bottom);

//
// Lifecycle (main thread). app_set_window / app_set_input return once the
// render thread no longer uses the previous object.
//

int app_start(void);
void app_set_window(ANativeWindow *win);
void app_set_input(AInputQueue *q);
void app_pause(void);
void app_stop(void);
