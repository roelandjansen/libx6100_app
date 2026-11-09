// SPDX-License-Identifier: GPL-3.0-or-later
//
// Modal popups: list chooser, frequency keypad, slider
//

#include "ui/ui_chooser.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_KEYPAD_MAX	12

static struct {
	const lv_font_t *f_item;
	const lv_font_t *f_title;
	float scale;
	lv_obj_t *bg;
	lv_obj_t *ta;
	ui_choice_cb choice_cb;
	ui_freq_cb freq_cb;
	ui_value_cb value_cb;
	void *ud;
	char entry[C_KEYPAD_MAX + 1];
} G;

static int32_t U(float v)
{
	return (int32_t)lrintf(v * G.scale);
}

void ui_chooser_init(const lv_font_t *f_item, const lv_font_t *f_title, float scale)
{
	memset(&G, 0, sizeof(G));
	G.f_item = f_item;
	G.f_title = f_title;
	G.scale = scale;
}

void ui_popup_close(void)
{
	if (G.bg != NULL)
		lv_obj_delete_async(G.bg);
	G.bg = NULL;
	G.ta = NULL;
}

static void on_bg(lv_event_t *e)
{
	// Tap outside the panel closes
	if (lv_event_get_target(e) == lv_event_get_current_target(e))
		ui_popup_close();
}

//
// Dimmed full-screen layer with a centred panel and a title
//

static lv_obj_t *panel(const char *title, int32_t w)
{
	lv_obj_t *p;
	lv_obj_t *t;

	ui_popup_close();
	G.bg = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(G.bg);
	lv_obj_set_size(G.bg, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(G.bg, lv_color_hex(0x000000), 0);
	lv_obj_set_style_bg_opa(G.bg, LV_OPA_60, 0);
	lv_obj_add_flag(G.bg, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(G.bg, on_bg, LV_EVENT_CLICKED, NULL);

	p = lv_obj_create(G.bg);
	lv_obj_set_width(p, w);
	lv_obj_set_height(p, LV_SIZE_CONTENT);
	lv_obj_set_style_max_height(p, LV_PCT(85), 0);
	lv_obj_center(p);
	lv_obj_set_style_bg_color(p, lv_color_hex(0x1d2733), 0);
	lv_obj_set_style_border_width(p, 0, 0);
	lv_obj_set_style_radius(p, U(10), 0);
	lv_obj_set_style_pad_all(p, U(10), 0);
	lv_obj_set_style_pad_row(p, U(6), 0);
	lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);

	t = lv_label_create(p);
	lv_obj_set_style_text_font(t, G.f_title, 0);
	lv_obj_set_style_text_color(t, lv_color_hex(0x4fc3f7), 0);
	lv_label_set_text(t, title);
	return p;
}

static void on_item(lv_event_t *e)
{
	const int idx = (int)(intptr_t)lv_event_get_user_data(e);
	const ui_choice_cb cb = G.choice_cb;
	void *ud = G.ud;

	ui_popup_close();
	if (cb != NULL)
		cb(idx, ud);
}

void ui_chooser_open(const char *title, const char *const *names, size_t n, int current,
		ui_choice_cb cb, void *ud)
{
	lv_obj_t *p = panel(title, U(300));
	size_t i;

	G.choice_cb = cb;
	G.ud = ud;
	for (i = 0; i < n && i < UI_CHOOSER_MAX; i++) {
		lv_obj_t *b = lv_button_create(p);
		lv_obj_t *l = lv_label_create(b);

		lv_obj_set_width(b, LV_PCT(100));
		lv_obj_set_height(b, U(44));
		lv_obj_set_style_shadow_width(b, 0, 0);
		lv_obj_set_style_bg_color(b, lv_color_hex((int)i == current ? 0x2e6d8e : 0x263241), 0);
		lv_obj_set_style_text_font(l, G.f_item, 0);
		lv_label_set_text(l, names[i]);
		lv_obj_center(l);
		lv_obj_add_event_cb(b, on_item, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	}
}

//
// Keypad: MHz with a decimal point, e.g. 7.074 or 14.2
//

static uint64_t parse_mhz(const char *s)
{
	uint64_t whole = 0;
	uint64_t frac = 0;
	uint64_t scale = 1000000;
	bool dot = false;

	for (; *s != '\0'; s++) {
		if (*s == '.') {
			if (dot)
				return 0;
			dot = true;
		} else if (!dot) {
			whole = whole * 10u + (uint64_t)(*s - '0');
		} else if (scale > 1) {
			scale /= 10u;
			frac += (uint64_t)(*s - '0') * scale;
		}
	}
	return whole * 1000000u + frac;
}

static void on_key(lv_event_t *e)
{
	lv_obj_t *bm = lv_event_get_target(e);
	const char *k = lv_buttonmatrix_get_button_text(bm, lv_buttonmatrix_get_selected_button(bm));
	const size_t n = strlen(G.entry);

	if (k == NULL)
		return;

	if (strcmp(k, "DEL") == 0) {
		if (n > 0)
			G.entry[n - 1] = '\0';
	} else if (strcmp(k, "OK") == 0) {
		const uint64_t hz = parse_mhz(G.entry);
		const ui_freq_cb cb = G.freq_cb;
		void *ud = G.ud;

		ui_popup_close();
		if (cb != NULL && hz > 0)
			cb(hz, ud);
		return;
	} else if (n < C_KEYPAD_MAX) {
		G.entry[n] = k[0];
		G.entry[n + 1] = '\0';
	}
	lv_textarea_set_text(G.ta, G.entry);
}

void ui_keypad_open(const char *title, ui_freq_cb cb, void *ud)
{
	static const char *map[] = {
		"1", "2", "3", "\n",
		"4", "5", "6", "\n",
		"7", "8", "9", "\n",
		".", "0", "DEL", "\n",
		"OK", ""
	};
	lv_obj_t *p = panel(title, U(320));
	lv_obj_t *bm;

	G.freq_cb = cb;
	G.ud = ud;
	G.entry[0] = '\0';

	G.ta = lv_textarea_create(p);
	lv_obj_set_width(G.ta, LV_PCT(100));
	lv_textarea_set_one_line(G.ta, true);
	lv_obj_set_style_text_font(G.ta, G.f_title, 0);
	lv_textarea_set_placeholder_text(G.ta, "MHz");
	lv_obj_remove_flag(G.ta, LV_OBJ_FLAG_CLICKABLE);

	bm = lv_buttonmatrix_create(p);
	lv_buttonmatrix_set_map(bm, map);
	lv_obj_set_size(bm, LV_PCT(100), U(300));
	lv_obj_set_style_text_font(bm, G.f_title, 0);
	lv_obj_add_event_cb(bm, on_key, LV_EVENT_VALUE_CHANGED, NULL);
}

//
// Slider
//

static void on_slider(lv_event_t *e)
{
	lv_obj_t *s = lv_event_get_target(e);

	if (G.value_cb != NULL)
		G.value_cb((int)lv_slider_get_value(s), G.ud);
}

void ui_slider_open(const char *title, int min, int max, int value, ui_value_cb cb, void *ud)
{
	lv_obj_t *p = panel(title, U(340));
	lv_obj_t *s;

	G.value_cb = cb;
	G.ud = ud;
	s = lv_slider_create(p);
	lv_obj_set_width(s, LV_PCT(92));
	lv_obj_set_height(s, U(14));
	lv_obj_set_style_margin_top(s, U(18), 0);
	lv_obj_set_style_margin_bottom(s, U(18), 0);
	lv_obj_set_style_margin_left(s, U(10), 0);
	lv_slider_set_range(s, min, max);
	lv_slider_set_value(s, value, LV_ANIM_OFF);
	lv_obj_add_event_cb(s, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
}
