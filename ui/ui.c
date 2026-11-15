// SPDX-License-Identifier: GPL-3.0-or-later
//
// Operating screen, portrait. Sizes are designed for a 480 pixel wide
// screen and scaled to the display width; text is rendered from the
// platform's TrueType font at the scaled sizes.
//

#include "ui/ui.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/rigfile.h"
#include "ui/ui_chooser.h"
#include "ui/ui_meter.h"
#include "ui/ui_scope.h"

#define C_DESIGN_W	480
#define C_METER_W	340
#define C_METER_H	200

//
// Colours (X6100 operating screen)
//
#define C_BG		0x0a0e14
#define C_PANEL		0x161c26
#define C_TEXT		0xe8ecf0
#define C_DIM		0x8a96a6
#define C_ACCENT	0x4fc3f7
#define C_RED		0xff5252
#define C_GREEN		0x66bb6a
#define C_AMBER		0xffb74d
#define C_FREQ		0xffffff

//
// Tuning steps offered (the rig's own step command is not used for this,
// as wfview: the step is a client setting)
//
static const uint32_t C_STEPS[] = { 1, 10, 50, 100, 500, 1000, 2500, 5000, 9000, 10000, 12500, 25000, 100000 };
#define C_N_STEPS	(sizeof(C_STEPS) / sizeof(C_STEPS[0]))

// The rig-name menu lines
enum { MENU_POWER_ON, MENU_POWER_OFF, MENU_TONE, MENU_AUDIO, MENU_N };

#define C_SUB		0x80deea	/* the other receiver (dual watch) */

static struct {
	struct ui_ops ops;
	float scale;
	int32_t w;
	int32_t h;

	//
	// Fonts
	//
	lv_font_t *f_small;
	lv_font_t *f_med;
	lv_font_t *f_large;
	lv_font_t *f_freq;

	//
	// Status bar
	//
	lv_obj_t *l_rxtx;
	lv_obj_t *l_vfo;
	lv_obj_t *l_model;
	lv_obj_t *l_link;

	//
	// Meter
	//
	struct ui_meter *vu;
	struct ui_font_data font;

	//
	// Frequency and mode lines
	//
	lv_obj_t *l_freq;
	lv_obj_t *l_mode;
	lv_obj_t *l_filter;
	lv_obj_t *l_step;
	lv_obj_t *l_agc;
	lv_obj_t *l_att;
	lv_obj_t *l_pre;
	lv_obj_t *l_split;

	//
	// Scope
	//
	struct ui_scope scope;
	lv_obj_t *l_scope_mode;

	//
	// Bottom bar
	//
	lv_obj_t *b_lock;
	lv_obj_t *l_lock;
	lv_obj_t *b_tune;
	lv_obj_t *l_tune;
	lv_obj_t *b_vol;
	lv_obj_t *l_vol;
	lv_obj_t *b_ptt;
	lv_obj_t *l_ptt;

	//
	// Not connected overlay
	//
	lv_obj_t *overlay;
	lv_obj_t *l_overlay;
	lv_obj_t *b_power_on;	/* in the overlay: the rig on while it gives no state */
	bool power_on_sent;	/* tapped: "switching on" until the rig answers */

	//
	// The mic peak on the PTT button while keyed, held 0.5 s
	//
	double tx_hold_db;
	uint32_t tx_hold_t;
	bool tone_on;		/* TX test tone in place of the mic */

	//
	// Dual watch (2026-11-15, step 1): DW in the AGC row, the other receiver on a line
	// over the top of the spectrum while it is on, the audio route from the menu
	//
	lv_obj_t *l_dw;
	lv_obj_t *l_sub;	/* the other receiver; a tap makes it the active one (step 2) */
	lv_obj_t *l_active;	/* MAIN / SUB on the big frequency: the active receiver */
	uint32_t rxsel_next;	/* next ask of VFO Main/Sub Band */
	int route;		/* AUDIO_ROUTE_* */
	int menu[MENU_N];	/* the rig-name menu as shown: MENU_* per line */

	//
	// Last snapshots
	//
	struct radio_state st;
	struct session_info info;
	struct radio_scope_row row;
	uint32_t row_seq;
	bool have_row;
	uint8_t volume;
	bool ptt_held;

	// The tuner asked often for a while after a press (ATU in / out, tune): atu_*
	uint32_t atu_from;	/* lv_tick_get() of the press */
	uint32_t atu_until;	/* asked until then (0 none) */
	uint32_t atu_next;	/* next ask */
	bool atu_tune;		/* a tune: ends early once the rig no longer reports TUNING */
} G;

static int32_t U(float v)
{
	return (int32_t)lrintf(v * G.scale);
}

//
// Formatting
//

static void fmt_freq(char *buf, size_t size, uint64_t hz)
{
	const unsigned int mhz = (unsigned int)(hz / 1000000u);
	const unsigned int khz = (unsigned int)(hz / 1000u % 1000u);
	const unsigned int h = (unsigned int)(hz % 1000u);

	snprintf(buf, size, "%u.%03u.%03u", mhz, khz, h);
}

static void fmt_step(char *buf, size_t size, uint32_t hz)
{
	if (hz >= 1000u && hz % 1000u == 0)
		snprintf(buf, size, "STEP %uk", (unsigned int)(hz / 1000u));
	else if (hz >= 1000u)
		snprintf(buf, size, "STEP %.1fk", (double)hz / 1000.0);
	else
		snprintf(buf, size, "STEP %u", (unsigned int)hz);
}

static void fmt_span(char *buf, size_t size, int mode, int half_span)
{
	const char *m = mode == 0 ? "CENTER" : (mode == 1 ? "FIXED" : "SCROLL");

	if (half_span >= 1000)
		snprintf(buf, size, "%s \xc2\xb1%dk", m, half_span / 1000);
	else
		snprintf(buf, size, "%s \xc2\xb1%d", m, half_span);
}

//
// Widget helpers
//

static lv_obj_t *box(lv_obj_t *parent, int32_t h, uint32_t bg)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_set_size(o, LV_PCT(100), h);
	lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	return o;
}

static lv_obj_t *row(lv_obj_t *parent, int32_t h)
{
	lv_obj_t *o = box(parent, h, C_BG);

	lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(o, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *txt)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	lv_label_set_text(l, txt);
	return l;
}

static lv_obj_t *tap_label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *txt,
		lv_event_cb_t cb)
{
	lv_obj_t *l = label(parent, f, color, txt);

	lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(l, U(8));
	lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, NULL);
	return l;
}

static lv_obj_t *button(lv_obj_t *parent, int32_t w, int32_t h, const char *txt, uint32_t bg, lv_obj_t **lbl)
{
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_t *l;

	lv_obj_set_size(b, w, h);
	lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
	lv_obj_set_style_radius(b, U(6), 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	l = label(b, G.f_med, C_TEXT, txt);
	lv_obj_center(l);
	if (lbl != NULL)
		*lbl = l;
	return b;
}

//
// Choosers (popups) and their results
//

static void on_mode_chosen(int idx, void *ud)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);
	const struct radio_mode *m = &G.st.rx[0].mode[0];

	(void)ud;
	if (r == NULL || idx < 0 || (size_t)idx >= r->n_modes)
		return;
	G.ops.set_mode(G.ops.ctx, r->modes[idx].num, 0, m->valid ? m->filter : 1);
}

static void on_mode(lv_event_t *e)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);
	const char *names[UI_CHOOSER_MAX];
	size_t i;
	int cur = -1;

	(void)e;
	if (r == NULL)
		return;
	for (i = 0; i < r->n_modes && i < UI_CHOOSER_MAX; i++) {
		names[i] = r->modes[i].name;
		if (G.st.rx[0].mode[0].valid && r->modes[i].num == G.st.rx[0].mode[0].num)
			cur = (int)i;
	}
	ui_chooser_open("Mode", names, i, cur, on_mode_chosen, NULL);
}

static void on_filter(lv_event_t *e)
{
	const struct radio_mode *m = &G.st.rx[0].mode[0];
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);
	int next;

	(void)e;
	if (!m->valid || r == NULL || r->n_filters == 0)
		return;
	// Cycle FIL1 -> FIL2 -> FIL3 -> FIL1 as the X6100 FIL key
	next = m->filter + 1;
	if (next > (int)r->n_filters)
		next = 1;
	G.ops.set_mode(G.ops.ctx, m->num, m->data, next);
}

static void on_step_chosen(int idx, void *ud)
{
	(void)ud;
	if (idx >= 0 && (size_t)idx < C_N_STEPS)
		G.ops.set_step(G.ops.ctx, C_STEPS[idx]);
}

static void on_step(lv_event_t *e)
{
	static char texts[C_N_STEPS][16];
	const char *names[C_N_STEPS];
	const uint32_t cur = G.ops.get_step(G.ops.ctx);
	int sel = -1;
	size_t i;

	(void)e;
	for (i = 0; i < C_N_STEPS; i++) {
		fmt_step(texts[i], sizeof(texts[i]), C_STEPS[i]);
		names[i] = texts[i] + 5;
		if (C_STEPS[i] == cur)
			sel = (int)i;
	}
	ui_chooser_open("Tuning step", names, C_N_STEPS, sel, on_step_chosen, NULL);
}

//
// Value choosers driven by the rig file lists: name of the rig command,
// value list from the definition
//

struct value_choice {
	const char *cmd;
	int values[UI_CHOOSER_MAX];
};

static struct value_choice G_VC;

static void on_value_chosen(int idx, void *ud)
{
	(void)ud;
	if (idx >= 0 && idx < UI_CHOOSER_MAX)
		G.ops.set_value(G.ops.ctx, G_VC.cmd, G_VC.values[idx]);
}

static void choose_named(const char *title, const char *cmd, const struct rig_named *list, size_t n,
		const struct radio_value *cur)
{
	const char *names[UI_CHOOSER_MAX];
	size_t i;
	int sel = -1;

	G_VC.cmd = cmd;
	for (i = 0; i < n && i < UI_CHOOSER_MAX; i++) {
		names[i] = list[i].name;
		G_VC.values[i] = list[i].num;
		if (cur->valid && cur->value == list[i].num)
			sel = (int)i;
	}
	ui_chooser_open(title, names, i, sel, on_value_chosen, NULL);
}

static void on_att(lv_event_t *e)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);

	(void)e;
	if (r != NULL && r->n_attenuators > 0)
		choose_named("Attenuator", "Attenuator Status", r->attenuators, r->n_attenuators, &G.st.attenuator);
}

// Step 2 (2026-11-15): the line over the spectrum is the other receiver; a tap makes it
// the active one on the rig (VFO Main/Sub Band 0 main, 1 sub), the scope with it (Scope
// Main/Sub). Tuning, mode and filter then go there: Icom's selected receiver
static void on_sub_tap(lv_event_t *e)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);
	const int to = G.st.rx_selected.valid && G.st.rx_selected.value != 0 ? 0 : 1;

	(void)e;
	if (r == NULL || rigfile_cmd(r, "VFO Main/Sub Band") == NULL)
		return;
	G.ops.set_value(G.ops.ctx, "VFO Main/Sub Band", to);
	if (rigfile_cmd(r, "Scope Main/Sub") != NULL)
		G.ops.set_value(G.ops.ctx, "Scope Main/Sub", to);
}

// The active receiver, asked once a second on a dual watch rig (the rig file does not
// poll it; a switch on the rig shows that way)
static void rxsel_poll(void)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);
	const uint32_t t = lv_tick_get();

	if (G.ops.get_value == NULL || !G.st.rig_valid || r == NULL || r->receivers < 2 ||
			rigfile_cmd(r, "VFO Main/Sub Band") == NULL || (int32_t)(t - G.rxsel_next) < 0)
		return;
	G.ops.get_value(G.ops.ctx, "VFO Main/Sub Band");
	G.rxsel_next = t + 1000u;
}

// DW: dual watch on / off on the rig (VFO Dual Watch 1 / 0); the rig file polls it, the
// set reads it back
static void on_dw(lv_event_t *e)
{
	const bool on = G.st.dual_watch.valid && G.st.dual_watch.value != 0;

	(void)e;
	G.ops.set_value(G.ops.ctx, "VFO Dual Watch", on ? 0 : 1);
}

static void on_pre(lv_event_t *e)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);

	(void)e;
	if (r != NULL && r->n_preamps > 0)
		choose_named("Preamp", "Preamp Status", r->preamps, r->n_preamps, &G.st.preamp);
}

//
// AGC Status names. Icom: 1 FAST, 2 MID, 3 SLOW. The X6100 (libx6100
// x6100_trx CI-V map) puts AUTO on 2 and reads OFF as 0; OFF cannot be
// set over CI-V (rig file range 1-3), so it is shown but not offered.
//

static const char *const *agc_names(void)
{
	static const char *const icom[] = { "OFF", "FAST", "MID", "SLOW" };
	static const char *const x6100[] = { "OFF", "FAST", "AUTO", "SLOW" };

	return strcmp(G.st.model, "X6100") == 0 ? x6100 : icom;
}

static const char *agc_name(int v)
{
	static char buf[16];

	snprintf(buf, sizeof(buf), "AGC %s", v >= 0 && v <= 3 ? agc_names()[v] : "?");
	return buf;
}

//
// The rig on / off (2026-11-15): a tap on the rig name at the top; off asks again. Works
// in standby only when the rig keeps its LAN alive (its power on over LAN setting)
//

static void on_power_off_sure(int idx, void *ud)
{
	(void)ud;
	if (idx == 1 && G.ops.power != NULL)
		G.ops.power(G.ops.ctx, 0);
}

// The audio route of a two-channel stream (main left, sub right): auto as received (a
// single receiver on both sides), or one receiver on both
static const char *const C_ROUTES[] = { "Auto: main left, sub right", "Main on both sides", "Sub on both sides" };

static void on_route_chosen(int idx, void *ud)
{
	(void)ud;
	if (idx < 0 || idx > 2 || G.ops.audio_route == NULL)
		return;
	G.route = idx;	/* AUDIO_ROUTE_AUTO, _MAIN, _SUB in this order */
	G.ops.audio_route(G.ops.ctx, idx);
}

// A rig with two receivers and dual watch in its rig file
static bool rig_has_dw(void)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);

	return r != NULL && r->receivers >= 2 && rigfile_cmd(r, "VFO Dual Watch") != NULL;
}

static void on_power_chosen(int idx, void *ud)
{
	static const char *const C_SURE[] = { "No", "Yes, power off" };

	(void)ud;
	if (idx >= 0 && idx < MENU_N && G.menu[idx] == MENU_TONE) {
		G.tone_on = !G.tone_on;
		G.ops.test_tone(G.ops.ctx, G.tone_on ? 1 : 0);
		return;
	}
	if (idx >= 0 && idx < MENU_N && G.menu[idx] == MENU_AUDIO) {
		ui_chooser_open("Audio (two receivers)", C_ROUTES, 3, G.route, on_route_chosen, NULL);
		return;
	}
	if (G.ops.power == NULL)
		return;
	if (idx == 0)
		G.ops.power(G.ops.ctx, 1);
	else if (idx == 1)
		ui_chooser_open("Power off the rig?", C_SURE, 2, 0, on_power_off_sure, NULL);
}

// The overlay's Power on (a rig in standby answers nothing, so no state comes)
static void on_overlay_power_on(lv_event_t *e)
{
	(void)e;
	if (G.ops.power != NULL && G.ops.power(G.ops.ctx, 1) == 0)
		G.power_on_sent = true;
}

static void on_model(lv_event_t *e)
{
	// Power on / off, the test tone (1 kHz at -6 dBFS in place of the mic while keyed, to
	// tell the mic chain from the rig's LAN input) and, for two receivers, the audio route
	static const char *names[MENU_N];
	size_t n = 0;

	(void)e;
	G.menu[n] = MENU_POWER_ON;
	names[n++] = "Power on";
	G.menu[n] = MENU_POWER_OFF;
	names[n++] = "Power off";
	if (G.ops.test_tone != NULL) {
		G.menu[n] = MENU_TONE;
		names[n++] = G.tone_on ? "Test tone off" : "Test tone on";
	}
	if (G.ops.audio_route != NULL && rig_has_dw()) {
		G.menu[n] = MENU_AUDIO;
		names[n++] = "Audio: main / sub";
	}
	ui_chooser_open("Rig", names, n, -1, on_power_chosen, NULL);
}

static void on_agc_chosen(int idx, void *ud)
{
	(void)ud;
	if (idx >= 0 && idx <= 2)
		G.ops.set_value(G.ops.ctx, "AGC Status", idx + 1);
}

static void on_agc(lv_event_t *e)
{
	const char *const *n = agc_names();
	const char *names[] = { n[1], n[2], n[3] };

	(void)e;
	ui_chooser_open("AGC", names, 3, G.st.agc.valid ? G.st.agc.value - 1 : -1, on_agc_chosen, NULL);
}

static void on_span_chosen(int idx, void *ud)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);

	(void)ud;
	if (r != NULL && idx >= 0 && (size_t)idx < r->n_spans)
		G.ops.set_span(G.ops.ctx, r->spans[idx].freq);
}

static void on_scope_mode(lv_event_t *e)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);
	const char *names[UI_CHOOSER_MAX];
	size_t i;
	int sel = -1;

	(void)e;
	if (r == NULL)
		return;
	for (i = 0; i < r->n_spans && i < UI_CHOOSER_MAX; i++) {
		names[i] = r->spans[i].name;
		if (G.st.rx[0].scope.span_hz.valid && (int)r->spans[i].freq == G.st.rx[0].scope.span_hz.value)
			sel = (int)i;
	}
	ui_chooser_open("Span", names, i, sel, on_span_chosen, NULL);
}

static void on_scope_mode_long(lv_event_t *e)
{
	(void)e;
	// Long press: centre <-> fixed
	if (G.st.rx[0].scope.mode.valid)
		G.ops.set_value(G.ops.ctx, "Scope Mode", G.st.rx[0].scope.mode.value == 0 ? 1 : 0);
}

//
// Keypad entry: refused unless inside one of the rig file bands
//

static void on_freq_entered(uint64_t hz, void *ud)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);
	size_t i;

	(void)ud;
	if (r == NULL)
		return;
	for (i = 0; i < r->n_bands; i++) {
		if (hz >= r->bands[i].start && hz <= r->bands[i].end) {
			G.ops.set_freq(G.ops.ctx, hz);
			return;
		}
	}
}

static void on_freq(lv_event_t *e)
{
	(void)e;
	ui_keypad_open("Frequency (MHz)", on_freq_entered, NULL);
}

static void on_vol_changed(int value, void *ud)
{
	(void)ud;
	G.volume = (uint8_t)value;
	G.ops.set_volume(G.ops.ctx, G.volume);
}

static void on_vol(lv_event_t *e)
{
	(void)e;
	ui_slider_open("Volume", 0, 255, G.volume, on_vol_changed, NULL);
}

static void on_lock(lv_event_t *e)
{
	(void)e;
	G.ops.set_lock(G.ops.ctx, !G.ops.get_lock(G.ops.ctx));
}

//
// ATU button: short press the tuner in / out (Tuner/ATU Status 1 / 0, no transmission),
// long press a tune cycle (transmits: only with TX available). Each asks the tuner state
// back once, so the button follows at once also where the rig file does not poll it
//

// After a press the tuner is asked every 0.5 s (atu_poll): for 1.5 s after in / out, and
// after a tune until the rig no longer reports TUNING (at least 1.5 s, at most 30 s)
static void atu_watch(bool tune)
{
	const uint32_t t = lv_tick_get();

	G.atu_from = t;
	G.atu_until = t + (tune ? 30000u : 1500u);
	G.atu_next = t;
	G.atu_tune = tune;
}

// The rig file is in and has no tuner (IC-9700, 2026-11-15): no ATU button, no polling.
// Before the rig file the button stays as ATU ?
static bool rig_no_tuner(void)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);

	return r != NULL && rigfile_cmd(r, "Tuner/ATU Status") == NULL;
}

// Also without a press (the TUNER key on the rig, a tune started there; Icom does not
// announce the tuner as it does the frequency): asked once a second, every 0.5 s while
// the rig reports TUNING. One small CI-V query a second, next to the scope and audio
static void atu_poll(void)
{
	const uint32_t t = lv_tick_get();
	uint32_t every = 1000u;

	if (G.ops.get_value == NULL || !G.st.rig_valid || rig_no_tuner())
		return;

	if (G.atu_until != 0 && ((uint32_t)(t - G.atu_from) >= 30000u || (int32_t)(t - G.atu_until) >= 0
			|| (G.atu_tune && (uint32_t)(t - G.atu_from) >= 1500u && G.st.tuner.valid && G.st.tuner.value != 2)))
		G.atu_until = 0;

	if (G.atu_until != 0 || (G.st.tuner.valid && G.st.tuner.value == 2))
		every = 500u;

	if ((int32_t)(t - G.atu_next) >= 0) {
		G.ops.get_value(G.ops.ctx, "Tuner/ATU Status");
		G.atu_next = t + every;
	}
}

static void on_atu(lv_event_t *e)
{
	const bool on = G.st.tuner.valid && G.st.tuner.value != 0;

	(void)e;
	G.ops.set_value(G.ops.ctx, "Tuner/ATU Status", on ? 0 : 1);
	atu_watch(false);
}

static void on_tune(lv_event_t *e)
{
	(void)e;
	if (!G.info.tx_available)
		return;
	G.ops.atu_tune(G.ops.ctx);
	atu_watch(true);
}

//
// PTT: hold to talk. Released on finger up, on the press being lost
// (finger slides off), and by the platform (background, focus loss)
//

static void on_ptt(lv_event_t *e)
{
	const lv_event_code_t code = lv_event_get_code(e);

	if (code == LV_EVENT_PRESSED) {
		if (G.ops.ptt(G.ops.ctx, 1) == 0)
			G.ptt_held = true;
	} else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		ui_release_ptt();
	}
}

void ui_release_ptt(void)
{
	if (G.ptt_held) {
		G.ptt_held = false;
		G.ops.ptt(G.ops.ctx, 0);
	}
}

void ui_set_volume(uint8_t volume)
{
	G.volume = volume;
	G.ops.set_volume(G.ops.ctx, volume);
}

//
// Scope touch: tap tunes to the point, horizontal drag tunes by steps
//

static void on_scope_tap(uint64_t hz, void *ud)
{
	const uint32_t step = G.ops.get_step(G.ops.ctx);

	(void)ud;
	if (step > 1)
		hz = (hz + step / 2) / step * step;
	G.ops.set_freq(G.ops.ctx, hz);
}

static void on_scope_drag(int steps, void *ud)
{
	(void)ud;
	G.ops.tune(G.ops.ctx, steps);
}

//
// Layout
//

static void build_status(lv_obj_t *scr)
{
	lv_obj_t *r = box(scr, U(30), C_PANEL);

	G.l_rxtx = label(r, G.f_med, C_GREEN, "RX");
	lv_obj_align(G.l_rxtx, LV_ALIGN_LEFT_MID, U(8), 0);
	G.l_vfo = label(r, G.f_small, C_TEXT, "VFO A");
	lv_obj_align(G.l_vfo, LV_ALIGN_LEFT_MID, U(52), 0);
	G.l_model = tap_label(r, G.f_small, C_DIM, "", on_model);	/* tap: the rig on / off */
	lv_obj_align(G.l_model, LV_ALIGN_CENTER, 0, 0);
	G.l_link = label(r, G.f_small, C_DIM, "");
	lv_obj_align(G.l_link, LV_ALIGN_RIGHT_MID, -U(8), 0);
}

static void build_meter(lv_obj_t *scr)
{
	const int32_t face_w = U(C_METER_W);
	lv_obj_t *p = box(scr, U(C_METER_H), C_BG);

	G.vu = ui_meter_create(p, face_w, G.font.ttf, G.font.len);
	if (G.vu != NULL)
		lv_obj_align(ui_meter_obj(G.vu), LV_ALIGN_CENTER, 0, 0);
}

static void build_freq(lv_obj_t *scr)
{
	lv_obj_t *r = box(scr, U(76), C_BG);

	G.l_freq = tap_label(r, G.f_freq, C_FREQ, "-.---.---", on_freq);
	lv_obj_center(G.l_freq);
	// MAIN / SUB: the active receiver, top left of the frequency box (dual watch rigs)
	G.l_active = label(r, G.f_small, C_DIM, "");
	lv_obj_align(G.l_active, LV_ALIGN_TOP_LEFT, U(8), U(2));
	lv_obj_add_flag(G.l_active, LV_OBJ_FLAG_HIDDEN);

	r = row(scr, U(32));
	G.l_mode = tap_label(r, G.f_large, C_ACCENT, "---", on_mode);
	G.l_filter = tap_label(r, G.f_large, C_ACCENT, "FIL-", on_filter);
	G.l_step = tap_label(r, G.f_large, C_ACCENT, "STEP", on_step);

	r = row(scr, U(28));
	G.l_agc = tap_label(r, G.f_med, C_TEXT, "", on_agc);
	G.l_att = tap_label(r, G.f_med, C_TEXT, "", on_att);
	G.l_pre = tap_label(r, G.f_med, C_TEXT, "", on_pre);
	G.l_dw = tap_label(r, G.f_med, C_DIM, "", on_dw);
	lv_obj_add_flag(G.l_dw, LV_OBJ_FLAG_HIDDEN);
	G.l_split = label(r, G.f_med, C_AMBER, "");
}

static void build_scope(lv_obj_t *scr, int32_t spec_h, int32_t wf_h)
{
	struct ui_scope_cfg c;

	memset(&c, 0, sizeof(c));
	c.w = G.w;
	c.spec_h = spec_h;
	c.wf_h = wf_h;
	c.font = G.f_small;
	c.on_tap = on_scope_tap;
	c.on_drag = on_scope_drag;
	c.drag_px_per_step = U(10);
	// about 1.5 design pixels per scope line: 3 px on a 1080 wide phone
	c.wf_line_h = U(1.5f) > 1 ? U(1.5f) : 1;
	ui_scope_create(&G.scope, scr, &c);

	G.l_scope_mode = tap_label(G.scope.spec, G.f_small, C_ACCENT, "", on_scope_mode);
	lv_obj_add_event_cb(G.l_scope_mode, on_scope_mode_long, LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_align(G.l_scope_mode, LV_ALIGN_TOP_RIGHT, -U(6), U(2));

	// The other receiver while dual watch is on: over the top left of the spectrum, on a
	// dark strip, so it costs no height (a row would take it from the scope on every rig)
	G.l_sub = tap_label(G.scope.spec, G.f_med, C_SUB, "", on_sub_tap);
	lv_obj_set_style_bg_color(G.l_sub, lv_color_hex(C_BG), 0);
	lv_obj_set_style_bg_opa(G.l_sub, LV_OPA_70, 0);
	lv_obj_set_style_pad_hor(G.l_sub, U(4), 0);
	lv_obj_align(G.l_sub, LV_ALIGN_TOP_LEFT, U(4), U(2));
	lv_obj_add_flag(G.l_sub, LV_OBJ_FLAG_HIDDEN);
}

static void build_bottom(lv_obj_t *scr)
{
	lv_obj_t *r = row(scr, U(72));

	lv_obj_set_style_bg_color(r, lv_color_hex(C_PANEL), 0);
	G.b_lock = button(r, U(84), U(52), "LOCK", 0x263241, &G.l_lock);
	lv_obj_add_event_cb(G.b_lock, on_lock, LV_EVENT_CLICKED, NULL);
	G.b_tune = button(r, U(84), U(52), "ATU", 0x263241, &G.l_tune);
	lv_obj_add_event_cb(G.b_tune, on_atu, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(G.b_tune, on_tune, LV_EVENT_LONG_PRESSED, NULL);
	G.b_vol = button(r, U(96), U(52), "VOL", 0x263241, &G.l_vol);
	lv_obj_add_event_cb(G.b_vol, on_vol, LV_EVENT_CLICKED, NULL);
	G.b_ptt = button(r, U(170), U(60), "PTT", 0x3a2328, &G.l_ptt);
	lv_obj_set_style_text_font(G.l_ptt, G.f_large, 0);
	lv_obj_add_event_cb(G.b_ptt, on_ptt, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(G.b_ptt, on_ptt, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(G.b_ptt, on_ptt, LV_EVENT_PRESS_LOST, NULL);
	// a finger sliding over the PTT button must not scroll anything
	lv_obj_remove_flag(G.b_ptt, LV_OBJ_FLAG_SCROLL_CHAIN);
}

static void build_overlay(lv_obj_t *scr)
{
	G.overlay = lv_obj_create(scr);
	lv_obj_remove_style_all(G.overlay);
	lv_obj_set_size(G.overlay, LV_PCT(100), LV_PCT(100));
	lv_obj_add_flag(G.overlay, LV_OBJ_FLAG_FLOATING);
	lv_obj_set_style_bg_color(G.overlay, lv_color_hex(0x000000), 0);
	lv_obj_set_style_bg_opa(G.overlay, LV_OPA_80, 0);
	G.l_overlay = label(G.overlay, G.f_large, C_TEXT, "");
	lv_label_set_long_mode(G.l_overlay, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(G.l_overlay, LV_PCT(90));
	lv_obj_set_style_text_align(G.l_overlay, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_center(G.l_overlay);

	// Power on while the rig gives no state (in standby it answers nothing, and this
	// overlay covered the rig name that opens the power menu, 2026-11-15)
	G.b_power_on = button(G.overlay, U(220), U(60), "Power on", 0x1f5d3a, NULL);
	lv_obj_align(G.b_power_on, LV_ALIGN_CENTER, 0, U(110));
	lv_obj_add_event_cb(G.b_power_on, on_overlay_power_on, LV_EVENT_CLICKED, NULL);
	lv_obj_add_flag(G.b_power_on, LV_OBJ_FLAG_HIDDEN);
}

int ui_create(lv_display_t *disp, const struct ui_ops *ops, const struct ui_font_data *font,
		int32_t inset_top, int32_t inset_bottom)
{
	lv_obj_t *scr;
	int32_t fixed;
	int32_t rest;

	memset(&G, 0, sizeof(G));
	G.ops = *ops;
	G.font = *font;
	G.w = lv_display_get_horizontal_resolution(disp);
	G.h = lv_display_get_vertical_resolution(disp);
	G.scale = (float)G.w / (float)C_DESIGN_W;
	G.volume = 128;

	G.f_small = lv_tiny_ttf_create_data(font->ttf, font->len, U(14));
	G.f_med = lv_tiny_ttf_create_data(font->ttf, font->len, U(17));
	G.f_large = lv_tiny_ttf_create_data(font->ttf, font->len, U(22));
	G.f_freq = lv_tiny_ttf_create_data(font->ttf, font->len, U(64));
	if (G.f_small == NULL || G.f_med == NULL || G.f_large == NULL || G.f_freq == NULL)
		return -1;
	ui_chooser_init(G.f_med, G.f_large, G.scale);

	scr = lv_display_get_screen_active(disp);
	lv_obj_remove_style_all(scr);
	lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_top(scr, inset_top, 0);
	lv_obj_set_style_pad_bottom(scr, inset_bottom, 0);

	build_status(scr);
	build_meter(scr);
	build_freq(scr);

	// Spectrum about a third of the scope area, waterfall the rest, above the bottom bar
	fixed = U(30) + U(C_METER_H) + U(76) + U(32) + U(28) + U(72);
	rest = G.h - inset_top - inset_bottom - fixed;
	if (rest < U(200))
		rest = U(200);
	build_scope(scr, rest * 2 / 5, rest - rest * 2 / 5);
	build_bottom(scr);
	build_overlay(scr);

	return 0;
}

void ui_destroy(void)
{
	ui_meter_destroy(G.vu);
	G.vu = NULL;
	ui_scope_destroy(&G.scope);
	if (G.f_small != NULL)
		lv_tiny_ttf_destroy(G.f_small);
	if (G.f_med != NULL)
		lv_tiny_ttf_destroy(G.f_med);
	if (G.f_large != NULL)
		lv_tiny_ttf_destroy(G.f_large);
	if (G.f_freq != NULL)
		lv_tiny_ttf_destroy(G.f_freq);
	memset(&G, 0, sizeof(G));
}

//
// Meter: S in RX; Po and SWR in TX. Po full scale from the rig file's
// highest Power reading, rounded to 10 / 100 / 200 W
//

static double power_full_scale(void)
{
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);
	double top = 0.0;
	size_t i;

	if (r != NULL)
		for (i = 0; i < r->n_meters; i++)
			if (strcmp(r->meters[i].meter, "Power") == 0 && r->meters[i].actual > top)
				top = r->meters[i].actual;
	if (top <= 15.0)
		return 10.0;
	if (top <= 150.0)
		return 100.0;
	return 200.0;
}

static void update_meter(void)
{
	const bool tx = G.st.transmit.valid && G.st.transmit.value != 0;

	if (G.vu == NULL)
		return;
	if (tx) {
		const struct radio_meter_value *po = &G.st.meter[RADIO_METER_POWER];
		const struct radio_meter_value *swr = &G.st.meter[RADIO_METER_SWR];

		ui_meter_tx(G.vu, po->valid ? po->cal : 0.0, swr->valid ? swr->cal : 1.0, power_full_scale());
	} else {
		ui_meter_rx(G.vu, G.st.meter[RADIO_METER_S].valid, G.st.meter[RADIO_METER_S].cal);
	}
}

static void set_text_if(lv_obj_t *l, const char *txt)
{
	if (strcmp(lv_label_get_text(l), txt) != 0)
		lv_label_set_text(l, txt);
}

static void update_lines(void)
{
	const struct radio_rx *rx = &G.st.rx[0];
	const struct radio_mode *m = &rx->mode[0];
	const struct rig_def *r = G.ops.get_rig(G.ops.ctx);
	const bool tx = G.st.transmit.valid && G.st.transmit.value != 0;
	const bool lock = G.ops.get_lock(G.ops.ctx);
	char buf[64];
	size_t i;

	// Status bar
	set_text_if(G.l_rxtx, tx ? "TX" : "RX");
	lv_obj_set_style_text_color(G.l_rxtx, lv_color_hex(tx ? C_RED : C_GREEN), 0);
	set_text_if(G.l_model, G.st.rig_valid ? G.st.model : G.info.radio_name);
	if (G.info.state == SESSION_CONNECTED)
		snprintf(buf, sizeof(buf), "%d ms", G.info.audio_latency_ms);
	else
		snprintf(buf, sizeof(buf), "%s", session_state_name(G.info.state));
	set_text_if(G.l_link, buf);

	// Frequency, mode, filter, step
	if (rx->freq_valid[0]) {
		fmt_freq(buf, sizeof(buf), rx->freq_hz[0]);
		set_text_if(G.l_freq, buf);
	}
	lv_obj_set_style_text_color(G.l_freq, lv_color_hex(lock ? C_DIM : C_FREQ), 0);
	if (m->valid) {
		snprintf(buf, sizeof(buf), "%s%s", m->name, m->data ? "-D" : "");
		set_text_if(G.l_mode, buf);
		snprintf(buf, sizeof(buf), "%s", m->filter_name[0] != '\0' ? m->filter_name : "FIL");
		set_text_if(G.l_filter, buf);
	}
	fmt_step(buf, sizeof(buf), G.ops.get_step(G.ops.ctx));
	set_text_if(G.l_step, buf);

	// AGC, attenuator, preamp, split: hidden when the rig has none
	if (G.st.agc.valid) {
		lv_obj_remove_flag(G.l_agc, LV_OBJ_FLAG_HIDDEN);
		set_text_if(G.l_agc, agc_name(G.st.agc.value));
	} else {
		lv_obj_add_flag(G.l_agc, LV_OBJ_FLAG_HIDDEN);
	}

	if (G.st.attenuator.valid && r != NULL) {
		const char *name = "ATT";

		for (i = 0; i < r->n_attenuators; i++)
			if (r->attenuators[i].num == G.st.attenuator.value)
				name = r->attenuators[i].name;
		snprintf(buf, sizeof(buf), G.st.attenuator.value ? "ATT %s" : "ATT OFF", name);
		set_text_if(G.l_att, buf);
		lv_obj_set_style_text_color(G.l_att, lv_color_hex(G.st.attenuator.value ? C_AMBER : C_DIM), 0);
		lv_obj_remove_flag(G.l_att, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(G.l_att, LV_OBJ_FLAG_HIDDEN);
	}

	if (G.st.preamp.valid) {
		snprintf(buf, sizeof(buf), G.st.preamp.value ? "PRE %d" : "PRE OFF", G.st.preamp.value);
		set_text_if(G.l_pre, buf);
		lv_obj_set_style_text_color(G.l_pre, lv_color_hex(G.st.preamp.value ? C_AMBER : C_DIM), 0);
		lv_obj_remove_flag(G.l_pre, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(G.l_pre, LV_OBJ_FLAG_HIDDEN);
	}

	set_text_if(G.l_split, G.st.split.valid && G.st.split.value ? "SPLIT" : "");

	// Dual watch: DW dim while off, amber while on; only for a rig with two receivers
	// and the command. The other receiver (Icom 25 01 / 26 01: the unselected one, the
	// sub while main is selected) over the spectrum while it is on
	//
	// Step 2: rx[0] is Icom's selected receiver (the active one: tuning, mode and filter
	// go there), rx[1] the other. The big frequency is the active one, tagged MAIN or
	// SUB; the line over the spectrum the other, a tap swaps. Shown while dual watch is
	// on, or while the sub is the active one (so the way back stays)
	//
	if (rig_has_dw() && G.st.dual_watch.valid) {
		const bool dw = G.st.dual_watch.value != 0;
		const bool on_sub = G.st.rx_selected.valid && G.st.rx_selected.value != 0;
		const struct radio_rx *other = &G.st.rx[1];

		set_text_if(G.l_dw, "DW");
		lv_obj_set_style_text_color(G.l_dw, lv_color_hex(dw ? C_AMBER : C_DIM), 0);
		lv_obj_remove_flag(G.l_dw, LV_OBJ_FLAG_HIDDEN);

		if (dw || on_sub) {
			set_text_if(G.l_active, on_sub ? "SUB" : "MAIN");
			lv_obj_set_style_text_color(G.l_active, lv_color_hex(on_sub ? C_SUB : C_DIM), 0);
			lv_obj_remove_flag(G.l_active, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(G.l_active, LV_OBJ_FLAG_HIDDEN);
		}

		if ((dw || on_sub) && other->freq_valid[0]) {
			char f[32];
			char line[128];

			fmt_freq(f, sizeof(f), other->freq_hz[0]);
			snprintf(line, sizeof(line), "%s  %s  %s%s", on_sub ? "MAIN" : "SUB", f,
					other->mode[0].valid ? other->mode[0].name : "",
					other->mode[0].valid && other->mode[0].data ? "-D" : "");
			set_text_if(G.l_sub, line);
			lv_obj_remove_flag(G.l_sub, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(G.l_sub, LV_OBJ_FLAG_HIDDEN);
		}
	} else {
		lv_obj_add_flag(G.l_dw, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(G.l_sub, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(G.l_active, LV_OBJ_FLAG_HIDDEN);
	}
	set_text_if(G.l_vfo, G.st.split.valid && G.st.split.value ? "VFO A/B" : "VFO A");

	// Scope label
	if (rx->scope.mode.valid && rx->scope.span_hz.valid) {
		fmt_span(buf, sizeof(buf), rx->scope.mode.value, rx->scope.span_hz.value);
		set_text_if(G.l_scope_mode, buf);
	}

	// Bottom bar
	lv_obj_set_style_bg_color(G.b_lock, lv_color_hex(lock ? 0x8d6e1f : 0x263241), 0);
	snprintf(buf, sizeof(buf), "VOL %d", G.volume * 100 / 255);
	set_text_if(G.l_vol, buf);
	// PTT needs TX; the ATU button stays usable (in / out sends nothing on the air; its
	// tune checks TX itself)
	if (!G.info.tx_available)
		lv_obj_add_state(G.b_ptt, LV_STATE_DISABLED);
	else
		lv_obj_remove_state(G.b_ptt, LV_STATE_DISABLED);

	// The rig's tuner: not known yet (ATU ?), out (ATU OFF, plain), in (ATU ON, green),
	// tuning (TUNING, amber)
	{
		const int tu = G.st.tuner.valid ? G.st.tuner.value : -1;

		lv_obj_set_style_bg_color(G.b_tune, lv_color_hex(tu == 2 ? 0x8d6e1f : tu == 1 ? 0x1f5d3a : 0x263241), 0);
		set_text_if(G.l_tune, tu == 2 ? "TUNING" : tu == 1 ? "ATU ON" : tu == 0 ? "ATU OFF" : "ATU ?");
		if (rig_no_tuner())
			lv_obj_add_flag(G.b_tune, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_remove_flag(G.b_tune, LV_OBJ_FLAG_HIDDEN);
	}
	lv_obj_set_style_bg_color(G.b_ptt, lv_color_hex(G.info.ptt ? C_RED : 0x3a2328), 0);
	if (G.info.ptt && G.info.ptt_remaining_s > 0 && G.info.ptt_remaining_s <= 30) {
		snprintf(buf, sizeof(buf), "TX %d", G.info.ptt_remaining_s);
	} else if (G.info.ptt) {
		// The mic peak while keyed, after the mic gain (2026-11-15): held 0.5 s, so it
		// can be read; CLIP at full scale
		const uint32_t t = lv_tick_get();
		const double db = G.info.tx_peak > 1e-5f ? 20.0 * log10((double)G.info.tx_peak) : -99.0;

		if (db > G.tx_hold_db || (uint32_t)(t - G.tx_hold_t) >= 500u) {
			G.tx_hold_db = db;
			G.tx_hold_t = t;
		}
		if (G.tone_on)
			snprintf(buf, sizeof(buf), "TONE %d dB", (int)lround(G.tx_hold_db));
		else if (G.tx_hold_db > -0.1)
			snprintf(buf, sizeof(buf), "%s", "TX CLIP");
		else
			snprintf(buf, sizeof(buf), "TX %d dB", (int)lround(G.tx_hold_db));
	} else {
		snprintf(buf, sizeof(buf), "%s", "PTT");
	}
	set_text_if(G.l_ptt, buf);
}

static void update_overlay(void)
{
	char buf[160];

	if (G.info.state == SESSION_CONNECTED && G.st.rig_valid) {
		lv_obj_add_flag(G.overlay, LV_OBJ_FLAG_HIDDEN);
		G.power_on_sent = false;
		return;
	}

	// Connected, the rig's address known, no state: perhaps in standby; Power on offered
	if (G.info.state == SESSION_CONNECTED && G.info.civ_addr != 0x00 && G.ops.power != NULL)
		lv_obj_remove_flag(G.b_power_on, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(G.b_power_on, LV_OBJ_FLAG_HIDDEN);

	if (G.info.state == SESSION_CONNECTED)
		snprintf(buf, sizeof(buf), "%s\n%s", G.info.radio_name,
				G.power_on_sent ? "switching on ..." : "reading radio state\n(rig off? Power on)");
	else if (G.info.status[0] != '\0')
		snprintf(buf, sizeof(buf), "%s\n%s", session_state_name(G.info.state), G.info.status);
	else
		snprintf(buf, sizeof(buf), "%s", session_state_name(G.info.state));
	set_text_if(G.l_overlay, buf);
	lv_obj_remove_flag(G.overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_poll(void)
{
	G.ops.get_info(G.ops.ctx, &G.info);
	G.ops.get_state(G.ops.ctx, &G.st);

	// Keep a held PTT alive; drop it when the session no longer keys
	if (G.ptt_held) {
		if (!G.info.ptt && G.info.tx_notices != 0 && G.info.state != SESSION_CONNECTED)
			G.ptt_held = false;
		else
			G.ops.ptt_heartbeat(G.ops.ctx);
	}

	if (G.ops.get_scope_row(G.ops.ctx, &G.row) && (!G.have_row || G.row.seq != G.row_seq)) {
		G.have_row = true;
		G.row_seq = G.row.seq;
		ui_scope_push(&G.scope, &G.row, G.st.amp_max > 0 ? G.st.amp_max : 160);
	}

	if (G.st.rx[0].freq_valid[0] && G.st.rx[0].mode[0].valid) {
		const struct radio_mode *m = &G.st.rx[0].mode[0];

		ui_scope_set_passband(&G.scope, G.st.rx[0].freq_hz[0], m->name,
				G.st.filter_width.valid ? G.st.filter_width.value : 0);
	}

	atu_poll();
	rxsel_poll();
	update_lines();
	update_meter();
	update_overlay();
}
