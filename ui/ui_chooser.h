// SPDX-License-Identifier: GPL-3.0-or-later
//
// Modal popups: list chooser, frequency keypad, slider
//

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#define UI_CHOOSER_MAX	32

typedef void (*ui_choice_cb)(int idx, void *ud);
typedef void (*ui_freq_cb)(uint64_t hz, void *ud);
typedef void (*ui_value_cb)(int value, void *ud);

void ui_chooser_init(const lv_font_t *f_item, const lv_font_t *f_title, float scale);

//
// One at a time; opening another closes the current one
//

void ui_chooser_open(const char *title, const char *const *names, size_t n, int current,
		ui_choice_cb cb, void *ud);
void ui_keypad_open(const char *title, ui_freq_cb cb, void *ud);
void ui_slider_open(const char *title, int min, int max, int value, ui_value_cb cb, void *ud);
void ui_popup_close(void);
