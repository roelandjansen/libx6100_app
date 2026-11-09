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
#include "ui/ui_scope.h"

#define C_DESIGN_W	480

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
	lv_obj_t *meter;
	lv_obj_t *needle;
	lv_obj_t *l_meter_main;
	lv_obj_t *l_meter_sub;
	int32_t needle_len;

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
	lv_obj_t *b_vol;
	lv_obj_t *l_vol;
	lv_obj_t *b_ptt;
	lv_obj_t *l_ptt;

	//
	// Not connected overlay
	//
	lv_obj_t *overlay;
	lv_obj_t *l_overlay;

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

static void on_tune(lv_event_t *e)
{
	(void)e;
	G.ops.atu_tune(G.ops.ctx);
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
	G.l_model = label(r, G.f_small, C_DIM, "");
	lv_obj_align(G.l_model, LV_ALIGN_CENTER, 0, 0);
	G.l_link = label(r, G.f_small, C_DIM, "");
	lv_obj_align(G.l_link, LV_ALIGN_RIGHT_MID, -U(8), 0);
}

static void build_meter(lv_obj_t *scr)
{
	static const char *texts[] = { "1", "3", "5", "7", "9", "+20", "+40", "+60", NULL };
	static lv_style_t red_main;
	static lv_style_t red_ticks;
	static lv_style_t red_text;
	lv_obj_t *p = box(scr, U(150), C_BG);
	lv_obj_t *face;
	lv_scale_section_t *sec;
	const int32_t sz = U(300);

	// Meter face as the X6100 analog meter: light panel, arc scale
	face = lv_obj_create(p);
	lv_obj_remove_style_all(face);
	lv_obj_set_size(face, U(330), U(140));
	lv_obj_align(face, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_color(face, lv_color_hex(0x1d2733), 0);
	lv_obj_set_style_bg_opa(face, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(face, U(8), 0);
	lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE);

	G.meter = lv_scale_create(face);
	lv_obj_set_size(G.meter, sz, sz);
	lv_obj_align(G.meter, LV_ALIGN_TOP_MID, 0, U(42));
	lv_scale_set_mode(G.meter, LV_SCALE_MODE_ROUND_OUTER);
	lv_scale_set_range(G.meter, 0, 70);
	lv_scale_set_total_tick_count(G.meter, 15);
	lv_scale_set_major_tick_every(G.meter, 2);
	lv_scale_set_angle_range(G.meter, 70);
	lv_scale_set_rotation(G.meter, 235);
	lv_scale_set_label_show(G.meter, true);
	lv_scale_set_text_src(G.meter, texts);
	lv_obj_set_style_text_font(G.meter, G.f_small, 0);
	lv_obj_set_style_text_color(G.meter, lv_color_hex(C_TEXT), 0);
	lv_obj_set_style_line_color(G.meter, lv_color_hex(C_TEXT), LV_PART_ITEMS);
	lv_obj_set_style_line_color(G.meter, lv_color_hex(C_TEXT), LV_PART_INDICATOR);
	lv_obj_set_style_length(G.meter, U(6), LV_PART_ITEMS);
	lv_obj_set_style_length(G.meter, U(11), LV_PART_INDICATOR);
	lv_obj_set_style_line_width(G.meter, U(2), LV_PART_INDICATOR);
	lv_obj_set_style_line_width(G.meter, U(2), LV_PART_MAIN);
	lv_obj_set_style_arc_color(G.meter, lv_color_hex(C_ACCENT), LV_PART_MAIN);
	lv_obj_set_style_arc_width(G.meter, U(2), LV_PART_MAIN);

	lv_style_init(&red_main);
	lv_style_set_arc_color(&red_main, lv_color_hex(C_RED));
	lv_style_set_arc_width(&red_main, U(3));
	lv_style_init(&red_ticks);
	lv_style_set_line_color(&red_ticks, lv_color_hex(C_RED));
	lv_style_init(&red_text);
	lv_style_set_text_color(&red_text, lv_color_hex(C_RED));
	lv_style_set_line_color(&red_text, lv_color_hex(C_RED));
	sec = lv_scale_add_section(G.meter);
	lv_scale_section_set_range(sec, 40, 70);
	lv_scale_section_set_style(sec, LV_PART_MAIN, &red_main);
	lv_scale_section_set_style(sec, LV_PART_ITEMS, &red_ticks);
	lv_scale_section_set_style(sec, LV_PART_INDICATOR, &red_text);

	G.needle = lv_line_create(G.meter);
	lv_obj_set_style_line_width(G.needle, U(3), 0);
	lv_obj_set_style_line_color(G.needle, lv_color_hex(0xffffff), 0);
	lv_obj_set_style_line_rounded(G.needle, true, 0);
	G.needle_len = sz / 2 - U(4);
	lv_scale_set_line_needle_value(G.meter, G.needle, G.needle_len, 0);

	G.l_meter_main = label(face, G.f_med, C_TEXT, "");
	lv_obj_align(G.l_meter_main, LV_ALIGN_BOTTOM_RIGHT, -U(10), -U(24));
	G.l_meter_sub = label(face, G.f_small, C_DIM, "");
	lv_obj_align(G.l_meter_sub, LV_ALIGN_BOTTOM_RIGHT, -U(10), -U(6));
}

static void build_freq(lv_obj_t *scr)
{
	lv_obj_t *r = box(scr, U(76), C_BG);

	G.l_freq = tap_label(r, G.f_freq, C_FREQ, "-.---.---", on_freq);
	lv_obj_center(G.l_freq);

	r = row(scr, U(32));
	G.l_mode = tap_label(r, G.f_large, C_ACCENT, "---", on_mode);
	G.l_filter = tap_label(r, G.f_large, C_ACCENT, "FIL-", on_filter);
	G.l_step = tap_label(r, G.f_large, C_ACCENT, "STEP", on_step);

	r = row(scr, U(28));
	G.l_agc = tap_label(r, G.f_med, C_TEXT, "", on_agc);
	G.l_att = tap_label(r, G.f_med, C_TEXT, "", on_att);
	G.l_pre = tap_label(r, G.f_med, C_TEXT, "", on_pre);
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
	ui_scope_create(&G.scope, scr, &c);

	G.l_scope_mode = tap_label(G.scope.spec, G.f_small, C_ACCENT, "", on_scope_mode);
	lv_obj_add_event_cb(G.l_scope_mode, on_scope_mode_long, LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_align(G.l_scope_mode, LV_ALIGN_TOP_RIGHT, -U(6), U(2));
}

static void build_bottom(lv_obj_t *scr)
{
	lv_obj_t *r = row(scr, U(72));

	lv_obj_set_style_bg_color(r, lv_color_hex(C_PANEL), 0);
	G.b_lock = button(r, U(84), U(52), "LOCK", 0x263241, &G.l_lock);
	lv_obj_add_event_cb(G.b_lock, on_lock, LV_EVENT_CLICKED, NULL);
	G.b_tune = button(r, U(84), U(52), "TUNE", 0x263241, NULL);
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
}

int ui_create(lv_display_t *disp, const struct ui_ops *ops, const struct ui_font_data *font,
		int32_t inset_top, int32_t inset_bottom)
{
	lv_obj_t *scr;
	int32_t fixed;
	int32_t rest;

	memset(&G, 0, sizeof(G));
	G.ops = *ops;
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
	fixed = U(30) + U(150) + U(76) + U(32) + U(28) + U(72);
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
// Meter: needle position on the 0..70 scale, S1..S9 at 0..40, +60 dB at 70
//

static int32_t s_to_scale(double db_over_s9)
{
	double v;

	if (db_over_s9 <= 0.0)
		v = (9.0 + db_over_s9 / 6.0 - 1.0) * 5.0;
	else
		v = 40.0 + db_over_s9 * 30.0 / 60.0;
	if (v < 0.0)
		v = 0.0;
	if (v > 70.0)
		v = 70.0;
	return (int32_t)lrint(v);
}

static void update_meter(void)
{
	const struct radio_meter_value *s = &G.st.meter[RADIO_METER_S];
	const bool tx = G.st.transmit.valid && G.st.transmit.value != 0;
	char a[32];
	char b[64];

	if (tx) {
		const struct radio_meter_value *po = &G.st.meter[RADIO_METER_POWER];
		const struct radio_meter_value *swr = &G.st.meter[RADIO_METER_SWR];
		const struct radio_meter_value *alc = &G.st.meter[RADIO_METER_ALC];

		// Power on the same needle, full scale at the rig's top power reading
		lv_scale_set_line_needle_value(G.meter, G.needle, G.needle_len,
				po->valid ? (int32_t)lrint(po->raw * 70.0 / 255.0) : 0);
		snprintf(a, sizeof(a), po->valid ? "PO %.1fW" : "PO", po->cal);
		snprintf(b, sizeof(b), "SWR %.1f  ALC %.1f", swr->valid ? swr->cal : 0.0, alc->valid ? alc->cal : 0.0);
	} else if (s->valid) {
		const double db = s->cal;

		lv_scale_set_line_needle_value(G.meter, G.needle, G.needle_len, s_to_scale(db));
		if (db <= 0.0)
			snprintf(a, sizeof(a), "S%d", (int)lrint(9.0 + db / 6.0));
		else
			snprintf(a, sizeof(a), "S9+%d", (int)lrint(db));
		// S9 = -73 dBm (HF)
		snprintf(b, sizeof(b), "%d dBm", (int)lrint(db - 73.0));
	} else {
		a[0] = '\0';
		b[0] = '\0';
	}
	lv_label_set_text(G.l_meter_main, a);
	lv_label_set_text(G.l_meter_sub, b);
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
	if (!G.info.tx_available) {
		lv_obj_add_state(G.b_ptt, LV_STATE_DISABLED);
		lv_obj_add_state(G.b_tune, LV_STATE_DISABLED);
	} else {
		lv_obj_remove_state(G.b_ptt, LV_STATE_DISABLED);
		lv_obj_remove_state(G.b_tune, LV_STATE_DISABLED);
	}
	lv_obj_set_style_bg_color(G.b_ptt, lv_color_hex(G.info.ptt ? C_RED : 0x3a2328), 0);
	if (G.info.ptt && G.info.ptt_remaining_s > 0 && G.info.ptt_remaining_s <= 30)
		snprintf(buf, sizeof(buf), "TX %d", G.info.ptt_remaining_s);
	else
		snprintf(buf, sizeof(buf), "%s", G.info.ptt ? "TX" : "PTT");
	set_text_if(G.l_ptt, buf);
}

static void update_overlay(void)
{
	char buf[160];

	if (G.info.state == SESSION_CONNECTED && G.st.rig_valid) {
		lv_obj_add_flag(G.overlay, LV_OBJ_FLAG_HIDDEN);
		return;
	}

	if (G.info.state == SESSION_CONNECTED)
		snprintf(buf, sizeof(buf), "%s\nreading radio state", G.info.radio_name);
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

	update_lines();
	update_meter();
	update_overlay();
}
