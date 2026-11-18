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
	int (*get_value)(void *ctx, const char *type);	/* ask the rig once; the answer lands in the state */
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

	//
	// The rig on / off (CI-V 18 01 / 18 00; also in standby, when the rig keeps its LAN)
	//
	int (*power)(void *ctx, int on);

	//
	// TX test tone in place of the mic (1 kHz, -6 dBFS); NULL: not offered
	//
	void (*test_tone)(void *ctx, int on);

	//
	// Two-channel RX audio: AUDIO_ROUTE_* (auto, main or sub on both); NULL: not offered
	//
	void (*audio_route)(void *ctx, int route);

	//
	// Mute: silence in place of the mic (the VOL tap); a rig file command without a
	// value (VFO A / B Select). NULL: not offered
	//
	void (*tx_mute)(void *ctx, int on);
	int (*send_cmd)(void *ctx, const char *type);
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

//
// inset_top / inset_bottom: rows covered by the status and navigation bars
//

int ui_create(lv_display_t *disp, const struct ui_ops *ops, const struct ui_font_data *font,
		int32_t inset_top, int32_t inset_bottom);
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
void ui_set_ctx(void *ctx);	/* a new session for the same screen (reconnect) */
