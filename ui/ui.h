// SPDX-License-Identifier: GPL-3.0-or-later
//
// Operating screen (LVGL), portrait, laid out after the X6100's own
// screen: status bar, analog meter, frequency, mode line, spectrum,
// waterfall, bottom bar with PTT.
//
// The UI reaches the radio only through struct ui_ops, so the same code
// runs against a session (ui_session.c) or a host preview with made-up
// data. All calls on the UI thread.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#include "core/session.h"
#include "model/radio_state.h"

struct rig_def;

//
// Radio access for the UI
//

struct ui_ops {
	void *ctx;

	//
	// Snapshots
	//
	void (*get_state)(void *ctx, struct radio_state *out);
	void (*get_info)(void *ctx, struct session_info *out);
	bool (*get_scope_row)(void *ctx, struct radio_scope_row *out);
	const struct rig_def *(*get_rig)(void *ctx);

	//
	// Receive controls
	//
	int (*set_freq)(void *ctx, uint64_t hz);
	int (*tune)(void *ctx, int steps);
	int (*set_mode)(void *ctx, int mode_num, int data, int filter);
	int (*set_value)(void *ctx, const char *type, int value);
	int (*set_span)(void *ctx, uint32_t half_span_hz);
	void (*set_step)(void *ctx, uint32_t hz);
	uint32_t (*get_step)(void *ctx);
	void (*set_lock)(void *ctx, bool locked);
	bool (*get_lock)(void *ctx);
	void (*set_volume)(void *ctx, uint8_t volume);

	//
	// TX
	//
	int (*ptt)(void *ctx, int on);
	int (*ptt_heartbeat)(void *ctx);
	int (*atu_tune)(void *ctx);
};

//
// TrueType font data supplied by the platform (kept by the caller for the
// lifetime of the UI)
//

struct ui_font_data {
	const void *ttf;
	size_t len;
};

//
// Lifecycle
//

int ui_create(lv_display_t *disp, const struct ui_ops *ops, const struct ui_font_data *font);
void ui_destroy(void);

//
// Once per frame, before lv_timer_handler(): pull snapshots, update widgets
//

void ui_poll(void);

//
// Platform events
//

void ui_set_volume(uint8_t volume);
void ui_release_ptt(void);
void ui_ops_from_session(struct ui_ops *ops, struct session *s);
