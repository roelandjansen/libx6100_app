// SPDX-License-Identifier: GPL-3.0-or-later
//
// Multi-scale transceiver meter, ported from libx6100 tools/x6100_trx.c
// (draw_vu, vu_update, meter_frac, s_reading, swr_frac): same geometry in
// X6100 pixels (face 296 x 168, pivot 95 below the face, radii 205 / 172 /
// 146 / 138, +-33 degrees), scaled to the face width, drawn with LVGL.
//
// The scales are drawn once into a background copy; each frame copies it
// back and draws the needle, pivot and peak marker.
//

#include "ui/ui_meter.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//
// Geometry (X6100 pixels) and ballistics, as x6100_trx.c
//
#define C_PI		3.14159265358979323846
#define C_FACE_W	296.0
#define C_FACE_H	168.0
#define C_PIVOT_DY	95.0
#define C_R_S		205.0
#define C_R_SWR		172.0
#define C_R_COMP	146.0
#define C_R_ALC		138.0
#define C_ANGLE		33.0
#define C_ATTACK_MS	80.0
#define C_RELEASE_MS	900.0
#define C_PEAK_HOLD_MS	1500.0
#define C_PEAK_FALL	0.0006

//
// Colours
//
#define C_BG		0x000000
#define C_WHITE		0xf0f0f0
#define C_GREY		0x9a9a9a
#define C_RED		0xe02828
#define C_BLUE		0x2e5fd0
#define C_MARK		0x58b8f0

enum scale {
	SCALE_S,
	SCALE_PO
};

struct ui_meter {
	lv_obj_t *canvas;
	lv_obj_t *l_line1;
	lv_obj_t *l_line2;
	lv_font_t *f_small;
	lv_font_t *f_big;
	uint16_t *buf;
	uint16_t *bg;
	int32_t w;
	int32_t h;
	double k;
	double cx;
	double cy;

	//
	// Background state and ballistics
	//
	enum scale scale;
	double full_w;
	bool drawn;
	double needle;
	uint32_t needle_ms;
	double peak;
	uint32_t peak_ms;
	double shown_needle;
	double shown_peak;
};

//
// Fraction 0..1 along the arcs to a point at radius r (X6100 pixels)
//

static lv_point_precise_t point(const struct ui_meter *m, double f, double r)
{
	const double a = (-C_ANGLE + 2.0 * C_ANGLE * f) * C_PI / 180.0;
	lv_point_precise_t p;

	p.x = (lv_value_precise_t)(m->cx + r * m->k * sin(a));
	p.y = (lv_value_precise_t)(m->cy - r * m->k * cos(a));
	return p;
}

static void arc(const struct ui_meter *m, lv_layer_t *layer, double f0, double f1, double r, double width,
		uint32_t rgb)
{
	lv_draw_arc_dsc_t d;

	lv_draw_arc_dsc_init(&d);
	d.color = lv_color_hex(rgb);
	d.width = (int32_t)lround(width * m->k);
	if (d.width < 1)
		d.width = 1;
	d.center.x = (int32_t)lround(m->cx);
	d.center.y = (int32_t)lround(m->cy);
	// LVGL radius is the outer edge; x6100_trx centres the stroke on r
	d.radius = (uint16_t)lround((r + width / 2.0) * m->k);
	d.start_angle = (lv_value_precise_t)(270.0 - C_ANGLE + 2.0 * C_ANGLE * f0);
	d.end_angle = (lv_value_precise_t)(270.0 - C_ANGLE + 2.0 * C_ANGLE * f1);
	lv_draw_arc(layer, &d);
}

static void line(const struct ui_meter *m, lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b,
		double width, uint32_t rgb)
{
	lv_draw_line_dsc_t d;

	lv_draw_line_dsc_init(&d);
	d.color = lv_color_hex(rgb);
	d.width = (int32_t)lround(width * m->k);
	if (d.width < 1)
		d.width = 1;
	d.p1 = a;
	d.p2 = b;
	lv_draw_line(layer, &d);
}

static void tick(const struct ui_meter *m, lv_layer_t *layer, double f, double r0, double r1, uint32_t rgb)
{
	line(m, layer, point(m, f, r0), point(m, f, r1), 2.0, rgb);
}

static void text(const struct ui_meter *m, lv_layer_t *layer, double f, double r, bool big, const char *s,
		uint32_t rgb)
{
	const lv_font_t *font = big ? m->f_big : m->f_small;
	const lv_point_precise_t p = point(m, f, r);
	lv_draw_label_dsc_t d;
	lv_point_t sz;
	lv_area_t a;

	lv_text_get_size(&sz, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	a.x1 = (int32_t)p.x - sz.x / 2;
	a.y1 = (int32_t)p.y - sz.y / 2;
	a.x2 = a.x1 + sz.x;
	a.y2 = a.y1 + sz.y;

	lv_draw_label_dsc_init(&d);
	d.font = font;
	d.color = lv_color_hex(rgb);
	d.text = s;
	// drawn when the layer is finished: keep a copy, s is reused
	d.text_local = 1;
	lv_draw_label(layer, &d, &a);
}

static void fmt_w(char *buf, size_t size, double w, bool unit)
{
	snprintf(buf, size, unit ? "%gW" : "%g", w);
}

//
// Scales, as draw_vu(): RX S scale white to S9, heavy red above, small Po
// labels under it; TX the main arc becomes the Po scale
//

static void draw_scales(struct ui_meter *m, lv_layer_t *layer)
{
	static const struct { double f; const char *s; } swr[] = {
		{ 0.0, "1" }, { 0.22, "1.5" }, { 0.38, "2" }, { 0.56, "3" }, { 0.76, "5" }, { 0.97, "INF" },
	};
	static const struct { double f; const char *s; } comp[] = {
		{ 0.10, "5" }, { 0.20, "10" }, { 0.32, "20" }, { 0.44, "30dB" },
	};
	char s[16];
	int i;

	if (m->scale == SCALE_PO) {
		arc(m, layer, 0.0, 1.0, C_R_S, 2.0, C_WHITE);
		for (i = 0; i <= 10; i++) {
			const double f = i / 10.0 * 0.97;

			tick(m, layer, f, C_R_S, C_R_S + (i % 2 ? 5.0 : 9.0), C_WHITE);
			if (i % 2 == 0) {
				fmt_w(s, sizeof(s), m->full_w * i / 10.0, i == 10);
				text(m, layer, f, C_R_S + 20.0, true, s, C_WHITE);
			}
		}
		text(m, layer, -0.11, C_R_S + 12.0, true, "PO", C_WHITE);
	} else {
		arc(m, layer, 0.0, 0.6, C_R_S, 2.0, C_WHITE);
		arc(m, layer, 0.6, 1.0, C_R_S + 2.0, 5.0, C_RED);
		for (i = 1; i <= 9; i++) {
			const double f = i / 9.0 * 0.6;

			tick(m, layer, f, C_R_S, C_R_S + (i % 2 ? 9.0 : 5.0), C_WHITE);
			if (i % 2) {
				snprintf(s, sizeof(s), "%d", i);
				text(m, layer, f, C_R_S + 20.0, true, s, C_WHITE);
			}
		}
		for (i = 20; i <= 60; i += 20) {
			snprintf(s, sizeof(s), "+%d", i);
			text(m, layer, 0.6 + i / 60.0 * 0.4, C_R_S + 20.0, true, s, C_RED);
		}
		text(m, layer, 0.0, C_R_S + 20.0, true, "S", C_WHITE);
		for (i = 1; i <= 5; i++) {
			const double f = i == 5 ? 0.97 : i * 0.2;

			tick(m, layer, f, C_R_S - 1.0, C_R_S - 5.0, C_GREY);
			fmt_w(s, sizeof(s), m->full_w * i / 5.0, i == 5);
			text(m, layer, f, C_R_S - 13.0, false, s, C_GREY);
		}
	}

	// SWR scale
	arc(m, layer, 0.0, 1.0, C_R_SWR, 2.0, C_GREY);
	for (i = 0; i < (int)(sizeof(swr) / sizeof(swr[0])); i++) {
		tick(m, layer, swr[i].f, C_R_SWR, C_R_SWR + 5.0, C_GREY);
		text(m, layer, swr[i].f, C_R_SWR + 13.0, false, swr[i].s, C_GREY);
	}

	// COMP (blue) and ALC (red) partial arcs
	arc(m, layer, 0.04, 0.46, C_R_COMP, 3.0, C_BLUE);
	for (i = 0; i < (int)(sizeof(comp) / sizeof(comp[0])); i++)
		text(m, layer, comp[i].f, C_R_COMP + 11.0, false, comp[i].s, C_BLUE);
	arc(m, layer, 0.0, 0.34, C_R_ALC, 4.0, C_RED);

	// Scale names at the left
	text(m, layer, -0.08, C_R_S - 13.0, false, "PO", C_GREY);
	text(m, layer, -0.08, C_R_SWR + 2.0, false, "SWR", C_GREY);
	text(m, layer, -0.05, C_R_COMP + 2.0, false, "COMP", C_BLUE);
	text(m, layer, 0.17, C_R_ALC - 10.0, false, "ALC", C_RED);
}

static void fill_bg(struct ui_meter *m)
{
	const size_t n = (size_t)m->w * (size_t)m->h;
	size_t i;

	for (i = 0; i < n; i++)
		m->buf[i] = 0;
}

//
// Redraw the background copy (scale change)
//

static void redraw_scales(struct ui_meter *m)
{
	lv_layer_t layer;

	fill_bg(m);
	lv_canvas_init_layer(m->canvas, &layer);
	draw_scales(m, &layer);
	lv_canvas_finish_layer(m->canvas, &layer);
	memcpy(m->bg, m->buf, (size_t)m->w * (size_t)m->h * sizeof(uint16_t));
	m->drawn = true;
}

//
// Needle from the bottom edge of the face with pivot dot, marker dot on
// the active arc
//

static void draw_needle(struct ui_meter *m)
{
	const double a = (-C_ANGLE + 2.0 * C_ANGLE * m->needle) * C_PI / 180.0;
	const double r_bot = (C_PIVOT_DY + 3.0) / cos(a);
	const double r_mark = m->scale == SCALE_PO ? C_R_S - 3.0 : C_R_S;
	lv_point_precise_t p0 = point(m, m->needle, r_bot);
	lv_point_precise_t p1 = point(m, m->needle, C_R_S + 8.0);
	lv_draw_rect_dsc_t r;
	lv_layer_t layer;
	lv_area_t ar;

	memcpy(m->buf, m->bg, (size_t)m->w * (size_t)m->h * sizeof(uint16_t));
	lv_canvas_init_layer(m->canvas, &layer);
	line(m, &layer, p0, p1, 2.0, C_WHITE);

	lv_draw_rect_dsc_init(&r);
	r.bg_color = lv_color_hex(0xffffff);
	ar.x1 = (int32_t)p0.x - (int32_t)lround(5.0 * m->k);
	ar.y1 = (int32_t)p0.y - (int32_t)lround(5.0 * m->k);
	ar.x2 = ar.x1 + (int32_t)lround(10.0 * m->k);
	ar.y2 = ar.y1 + (int32_t)lround(10.0 * m->k);
	lv_draw_rect(&layer, &r, &ar);

	p0 = point(m, m->peak, r_mark);
	r.bg_color = lv_color_hex(C_BG);
	ar.x1 = (int32_t)p0.x - (int32_t)lround(4.0 * m->k);
	ar.y1 = (int32_t)p0.y - (int32_t)lround(4.0 * m->k);
	ar.x2 = ar.x1 + (int32_t)lround(9.0 * m->k);
	ar.y2 = ar.y1 + (int32_t)lround(9.0 * m->k);
	lv_draw_rect(&layer, &r, &ar);
	r.bg_color = lv_color_hex(C_MARK);
	ar.x1 += (int32_t)lround(m->k);
	ar.y1 += (int32_t)lround(m->k);
	ar.x2 -= (int32_t)lround(m->k);
	ar.y2 -= (int32_t)lround(m->k);
	lv_draw_rect(&layer, &r, &ar);
	lv_canvas_finish_layer(m->canvas, &layer);

	m->shown_needle = m->needle;
	m->shown_peak = m->peak;
}

//
// Readings start at a fixed x (as the fixed-width "S%-4d" of s_reading):
// the widest text ends 8 px from the right edge, the "S" never moves
//

static int32_t left_of(const struct ui_meter *m, const char *widest)
{
	lv_point_t sz;

	lv_text_get_size(&sz, widest, m->f_big, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return m->w - (int32_t)lround(8.0 * m->k) - sz.x;
}

struct ui_meter *ui_meter_create(lv_obj_t *parent, int32_t w, const void *ttf, size_t ttf_len)
{
	struct ui_meter *m = calloc(1, sizeof(*m));

	if (m == NULL)
		return NULL;

	m->w = w;
	m->k = (double)w / C_FACE_W;
	m->h = (int32_t)lround(C_FACE_H * m->k);
	m->cx = (double)w / 2.0 + 6.0 * m->k;
	m->cy = (double)m->h + C_PIVOT_DY * m->k;
	m->full_w = 10.0;
	m->shown_needle = -1.0;
	m->buf = calloc((size_t)m->w * (size_t)m->h, sizeof(uint16_t));
	m->bg = calloc((size_t)m->w * (size_t)m->h, sizeof(uint16_t));
	// x6100_lcd text scale 1 / 2 (5 x 7 font) to TrueType pixel sizes
	m->f_small = lv_tiny_ttf_create_data(ttf, ttf_len, (int32_t)lround(10.0 * m->k));
	m->f_big = lv_tiny_ttf_create_data(ttf, ttf_len, (int32_t)lround(16.0 * m->k));
	if (m->buf == NULL || m->bg == NULL || m->f_small == NULL || m->f_big == NULL) {
		ui_meter_destroy(m);
		return NULL;
	}

	m->canvas = lv_canvas_create(parent);
	lv_canvas_set_buffer(m->canvas, m->buf, m->w, m->h, LV_COLOR_FORMAT_RGB565);

	m->l_line1 = lv_label_create(m->canvas);
	lv_obj_set_style_text_font(m->l_line1, m->f_big, 0);
	lv_obj_set_style_text_color(m->l_line1, lv_color_hex(C_WHITE), 0);
	lv_label_set_text(m->l_line1, "");
	lv_obj_align(m->l_line1, LV_ALIGN_BOTTOM_LEFT, left_of(m, "S9+60"), -(int32_t)lround(22.0 * m->k));
	m->l_line2 = lv_label_create(m->canvas);
	lv_obj_set_style_text_font(m->l_line2, m->f_big, 0);
	lv_obj_set_style_text_color(m->l_line2, lv_color_hex(C_GREY), 0);
	lv_label_set_text(m->l_line2, "");
	lv_obj_align(m->l_line2, LV_ALIGN_BOTTOM_LEFT, left_of(m, "-127dBm"), -(int32_t)lround(4.0 * m->k));

	redraw_scales(m);
	return m;
}

void ui_meter_destroy(struct ui_meter *m)
{
	if (m == NULL)
		return;
	if (m->canvas != NULL)
		lv_obj_delete(m->canvas);
	if (m->f_small != NULL)
		lv_tiny_ttf_destroy(m->f_small);
	if (m->f_big != NULL)
		lv_tiny_ttf_destroy(m->f_big);
	free(m->buf);
	free(m->bg);
	free(m);
}

lv_obj_t *ui_meter_obj(struct ui_meter *m)
{
	return m->canvas;
}

//
// vu_update(): fast attack, slow release; peak jumps up with the needle,
// holds, then falls slowly
//

static void ballistics(struct ui_meter *m, double target)
{
	const uint32_t t = lv_tick_get();
	const double dt = m->needle_ms != 0 ? (double)(t - m->needle_ms) : 0.0;
	const double tau = target > m->needle ? C_ATTACK_MS : C_RELEASE_MS;
	const double k = dt / tau;

	m->needle += (target - m->needle) * (k > 1.0 ? 1.0 : k);
	m->needle_ms = t;

	if (m->needle >= m->peak) {
		m->peak = m->needle;
		m->peak_ms = t;
	} else if ((double)(t - m->peak_ms) > C_PEAK_HOLD_MS) {
		m->peak -= C_PEAK_FALL * dt;
		if (m->peak < m->needle)
			m->peak = m->needle;
	}

	// Redraw only when the needle or marker moved by a pixel or so
	if (fabs(m->needle - m->shown_needle) * 2.0 * C_ANGLE * C_PI / 180.0 * C_R_S * m->k > 0.5 ||
			fabs(m->peak - m->shown_peak) * 2.0 * C_ANGLE * C_PI / 180.0 * C_R_S * m->k > 0.5)
		draw_needle(m);
}

static void set_scale(struct ui_meter *m, enum scale sc, double full_w)
{
	if (sc != m->scale || full_w != m->full_w || !m->drawn) {
		m->scale = sc;
		m->full_w = full_w;
		// no needle reset: it glides to the new reading; the peak starts again
		m->peak = 0.0;
		redraw_scales(m);
		draw_needle(m);
	}
}

static void set_text(lv_obj_t *l, const char *s)
{
	if (strcmp(lv_label_get_text(l), s) != 0)
		lv_label_set_text(l, s);
}

//
// meter_frac() / s_reading() on dBm, S9 = -73 dBm
//

void ui_meter_rx(struct ui_meter *m, bool valid, double db_over_s9)
{
	const double dbm = db_over_s9 - 73.0;
	char l1[16];
	char l2[16];
	double f;

	set_scale(m, SCALE_S, m->full_w);
	if (!valid) {
		ballistics(m, 0.0);
		set_text(m->l_line1, "");
		set_text(m->l_line2, "");
		return;
	}

	if (dbm <= -73.0) {
		const double su = (dbm + 127.0) / 6.0;
		const int sr = (int)((dbm + 127.0) / 6.0 + 0.5);

		f = su < 0.0 ? 0.0 : su / 9.0 * 0.6;
		snprintf(l1, sizeof(l1), "S%d", sr < 0 ? 0 : sr);
	} else {
		const double over = dbm + 73.0;
		int o = (int)(over + 0.5);

		f = 0.6 + (over > 60.0 ? 60.0 : over) / 60.0 * 0.4;
		// 10 dB steps, rounded: +0..5 = S9, +6..15 = S9+10, .. S9+60
		o = (o + 4) / 10 * 10;
		o = o > 60 ? 60 : o;
		if (o < 10)
			snprintf(l1, sizeof(l1), "S9");
		else
			snprintf(l1, sizeof(l1), "S9+%d", o);
	}
	snprintf(l2, sizeof(l2), "%.0fdBm", dbm);

	ballistics(m, f);
	set_text(m->l_line1, l1);
	set_text(m->l_line2, l2);
}

void ui_meter_tx(struct ui_meter *m, double watts, double swr, double full_w)
{
	char l1[16];
	char l2[24];
	double f = watts / full_w * 0.97;

	set_scale(m, SCALE_PO, full_w);
	if (f < 0.0)
		f = 0.0;
	if (f > 1.0)
		f = 1.0;
	snprintf(l1, sizeof(l1), "%.1fW", watts);
	snprintf(l2, sizeof(l2), "SWR %.1f", swr);
	ballistics(m, f);
	set_text(m->l_line1, l1);
	set_text(m->l_line2, l2);
}
