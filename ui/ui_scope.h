// SPDX-License-Identifier: GPL-3.0-or-later
//
// Spectrum and waterfall, drawn straight into RGB565 canvas buffers
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "model/radio_state.h"

typedef void (*ui_scope_tap_cb)(uint64_t hz, void *ud);
typedef void (*ui_scope_drag_cb)(int steps, void *ud);

struct ui_scope_cfg {
	int32_t w;
	int32_t spec_h;
	int32_t wf_h;
	const lv_font_t *font;
	ui_scope_tap_cb on_tap;
	ui_scope_drag_cb on_drag;
	void *ud;
	int32_t drag_px_per_step;
	int32_t wf_line_h;
};

struct ui_scope {
	struct ui_scope_cfg cfg;
	lv_obj_t *spec;
	lv_obj_t *wf;
	lv_obj_t *l_low;
	lv_obj_t *l_mid;
	lv_obj_t *l_high;
	uint16_t *spec_buf;
	uint16_t *wf_buf;
	uint16_t palette[256];

	//
	// Last row and passband
	//
	uint64_t low_hz;
	uint64_t high_hz;
	uint64_t vfo_hz;
	uint64_t pb_lo;
	uint64_t pb_hi;
	uint8_t level[2048];
	bool have_row;

	//
	// Touch
	//
	int32_t press_x;
	int32_t drag_acc;
	bool dragging;
};

int ui_scope_create(struct ui_scope *sc, lv_obj_t *parent, const struct ui_scope_cfg *cfg);
void ui_scope_destroy(struct ui_scope *sc);
void ui_scope_push(struct ui_scope *sc, const struct radio_scope_row *row, int amp_max);
void ui_scope_set_passband(struct ui_scope *sc, uint64_t vfo_hz, const char *mode, int width_hz);
