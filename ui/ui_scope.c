// SPDX-License-Identifier: GPL-3.0-or-later
//
// Spectrum and waterfall, drawn straight into RGB565 canvas buffers
//

#include "ui/ui_scope.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_SPEC_BG	0x0a1420
#define C_GRID		0x1e3246
#define C_FILL		0xc0506e
#define C_LINE		0xffd0dc
#define C_PASSBAND	0x2e7d32
#define C_VFO		0xff5252
#define C_DRAG_SLOP	8

static uint16_t rgb565(uint32_t rgb)
{
	const uint32_t r = (rgb >> 16) & 0xffu;
	const uint32_t g = (rgb >> 8) & 0xffu;
	const uint32_t b = rgb & 0xffu;

	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint16_t mix565(uint16_t a, uint16_t b)
{
	// 50 % blend
	return (uint16_t)(((a & 0xf7deu) >> 1) + ((b & 0xf7deu) >> 1));
}

//
// Waterfall palette: black, blue, cyan, yellow, red, white
//

static void build_palette(uint16_t *pal)
{
	static const uint32_t stops[] = { 0x000000, 0x0a1a6e, 0x1e6fd8, 0x22d3ee, 0xfde047, 0xef4444, 0xffffff };
	const int n = (int)(sizeof(stops) / sizeof(stops[0])) - 1;
	int i;

	for (i = 0; i < 256; i++) {
		const int seg = i * n / 256;
		const int t = (i * n % 256);
		const uint32_t a = stops[seg];
		const uint32_t b = stops[seg + 1];
		uint32_t c = 0;
		int sh;

		for (sh = 0; sh <= 16; sh += 8) {
			const int ca = (int)((a >> sh) & 0xffu);
			const int cb = (int)((b >> sh) & 0xffu);

			c |= (uint32_t)(ca + (cb - ca) * t / 256) << sh;
		}
		pal[i] = rgb565(c);
	}
}

static uint64_t x_to_hz(const struct ui_scope *sc, int32_t x)
{
	const uint64_t span = sc->high_hz - sc->low_hz;

	if (x < 0)
		x = 0;
	if (x >= sc->cfg.w)
		x = sc->cfg.w - 1;
	return sc->low_hz + span * (uint64_t)x / (uint64_t)sc->cfg.w;
}

static int32_t hz_to_x(const struct ui_scope *sc, uint64_t hz)
{
	const int64_t span = (int64_t)(sc->high_hz - sc->low_hz);

	if (span <= 0)
		return -1;
	return (int32_t)(((int64_t)hz - (int64_t)sc->low_hz) * sc->cfg.w / span);
}

//
// Touch: a tap tunes to the point, a horizontal drag tunes by steps
// (drag right = lower frequency, the band moves with the finger)
//

static void on_touch(lv_event_t *e)
{
	struct ui_scope *sc = lv_event_get_user_data(e);
	const lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *obj = lv_event_get_current_target(e);
	lv_point_t p;
	lv_area_t a;

	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(obj, &a);
	p.x -= a.x1;

	if (code == LV_EVENT_PRESSED) {
		sc->press_x = p.x;
		sc->drag_acc = 0;
		sc->dragging = false;
	} else if (code == LV_EVENT_PRESSING) {
		const int32_t dx = p.x - sc->press_x;
		const int32_t per = sc->cfg.drag_px_per_step > 0 ? sc->cfg.drag_px_per_step : 10;

		if (!sc->dragging && (dx > C_DRAG_SLOP || dx < -C_DRAG_SLOP))
			sc->dragging = true;
		if (sc->dragging) {
			const int32_t steps = (dx - sc->drag_acc) / per;

			if (steps != 0) {
				sc->drag_acc += steps * per;
				if (sc->cfg.on_drag != NULL)
					sc->cfg.on_drag(-(int)steps, sc->cfg.ud);
			}
		}
	} else if (code == LV_EVENT_RELEASED) {
		if (!sc->dragging && sc->have_row && sc->cfg.on_tap != NULL)
			sc->cfg.on_tap(x_to_hz(sc, p.x), sc->cfg.ud);
		sc->dragging = false;
	}
}

static lv_obj_t *canvas(lv_obj_t *parent, uint16_t **buf, int32_t w, int32_t h, struct ui_scope *sc)
{
	lv_obj_t *c = lv_canvas_create(parent);

	*buf = calloc((size_t)w * (size_t)h, sizeof(uint16_t));
	if (*buf == NULL)
		return NULL;
	lv_canvas_set_buffer(c, *buf, w, h, LV_COLOR_FORMAT_RGB565);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLL_CHAIN);
	lv_obj_add_event_cb(c, on_touch, LV_EVENT_PRESSED, sc);
	lv_obj_add_event_cb(c, on_touch, LV_EVENT_PRESSING, sc);
	lv_obj_add_event_cb(c, on_touch, LV_EVENT_RELEASED, sc);
	return c;
}

static lv_obj_t *edge_label(lv_obj_t *parent, const lv_font_t *f, lv_align_t align)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(0xb0bec5), 0);
	lv_label_set_text(l, "");
	lv_obj_align(l, align, 0, 0);
	return l;
}

int ui_scope_create(struct ui_scope *sc, lv_obj_t *parent, const struct ui_scope_cfg *cfg)
{
	size_t i;

	memset(sc, 0, sizeof(*sc));
	sc->cfg = *cfg;
	if (sc->cfg.w > (int32_t)sizeof(sc->level))
		sc->cfg.w = (int32_t)sizeof(sc->level);
	build_palette(sc->palette);

	sc->spec = canvas(parent, &sc->spec_buf, sc->cfg.w, sc->cfg.spec_h, sc);
	sc->wf = canvas(parent, &sc->wf_buf, sc->cfg.w, sc->cfg.wf_h, sc);
	if (sc->spec == NULL || sc->wf == NULL)
		return -1;

	for (i = 0; i < (size_t)sc->cfg.w * (size_t)sc->cfg.spec_h; i++)
		sc->spec_buf[i] = rgb565(C_SPEC_BG);

	sc->l_low = edge_label(sc->spec, cfg->font, LV_ALIGN_BOTTOM_LEFT);
	sc->l_mid = edge_label(sc->spec, cfg->font, LV_ALIGN_BOTTOM_MID);
	sc->l_high = edge_label(sc->spec, cfg->font, LV_ALIGN_BOTTOM_RIGHT);

	// Marker tags (a letter on the marker's colour) and the out-of-view arrows
	for (i = 0; i < 3; i++) {
		sc->l_tag[i] = edge_label(sc->spec, cfg->font, LV_ALIGN_TOP_LEFT);
		lv_obj_set_style_text_color(sc->l_tag[i], lv_color_hex(0x000000), 0);
		lv_obj_set_style_bg_opa(sc->l_tag[i], LV_OPA_COVER, 0);
		lv_obj_set_style_pad_hor(sc->l_tag[i], 3, 0);
		lv_obj_add_flag(sc->l_tag[i], LV_OBJ_FLAG_HIDDEN);
		sc->l_arrow[i] = edge_label(sc->spec, cfg->font, LV_ALIGN_TOP_LEFT);
		lv_obj_set_style_bg_color(sc->l_arrow[i], lv_color_hex(0x000000), 0);
		lv_obj_set_style_bg_opa(sc->l_arrow[i], LV_OPA_COVER, 0);
		lv_obj_add_flag(sc->l_arrow[i], LV_OBJ_FLAG_HIDDEN);

		// Through the waterfall: 2 px, not clickable (taps and drags stay on the canvas)
		sc->wf_line[i] = lv_obj_create(sc->wf);
		lv_obj_remove_style_all(sc->wf_line[i]);
		lv_obj_set_size(sc->wf_line[i], 2, sc->cfg.wf_h);
		lv_obj_set_style_bg_opa(sc->wf_line[i], LV_OPA_70, 0);
		lv_obj_remove_flag(sc->wf_line[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_flag(sc->wf_line[i], LV_OBJ_FLAG_HIDDEN);
	}
	return 0;
}

//
// Markers (2026-11-18, as x6100_trx's draw_markers): set each poll; the lines go into the
// spectrum at its next row, tags, arrows and the waterfall lines are placed here. Each
// marker has a tag of its own in its colour; markers on one column (2 px) stack them,
// T on top, then M / A, then S / B (as on the IC-7851); a tag that would cover one of
// another column drops below it. After a frequency change the lines go through the
// waterfall for C_WF_LINE_MS, as on x6100_trx
//

#define C_WF_LINE_MS	10000u

void ui_scope_set_markers(struct ui_scope *sc, const struct ui_scope_marker mark[3])
{
	static const int order[3] = { 2, 0, 1 };	/* the stack, top down: T, M / A, S / B */
	const int32_t w = sc->cfg.w;
	const int32_t tag_y = sc->cfg.spec_h / 5;
	const int32_t lh = lv_font_get_line_height(sc->cfg.font) + 2;
	const uint32_t now = lv_tick_get();
	int32_t x[3];
	int32_t box_x[3];
	int32_t box_w[3];
	int box_row[3];
	int nbox = 0;
	bool moved = false;
	int p;
	int i;

	for (i = 0; i < 3; i++)
		if (mark[i].on != sc->mark[i].on || (mark[i].on && mark[i].hz != sc->mark[i].hz))
			moved = true;
	if (moved)
		sc->wf_until = now + C_WF_LINE_MS;

	memcpy(sc->mark, mark, sizeof(sc->mark));
	sc->marks_set = true;

	for (i = 0; i < 3; i++) {
		lv_obj_add_flag(sc->l_tag[i], LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(sc->l_arrow[i], LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(sc->wf_line[i], LV_OBJ_FLAG_HIDDEN);
		x[i] = mark[i].on && sc->have_row ? hz_to_x(sc, mark[i].hz) : -1;
	}
	if (!sc->have_row)
		return;

	for (p = 0; p < 3; p++) {
		const int k = order[p];
		char t[2] = { mark[k].tag, '\0' };
		int32_t tw;
		int32_t tx;
		int row = 0;
		int q;

		if (!mark[k].on)
			continue;

		// Out of view: an arrow at that edge, one row down per marker
		if (x[k] < 0 || x[k] >= w) {
			lv_label_set_text(sc->l_arrow[k], x[k] < 0 ? "<<" : ">>");
			lv_obj_set_style_text_color(sc->l_arrow[k], lv_color_hex(mark[k].color), 0);
			lv_obj_align(sc->l_arrow[k], x[k] < 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0,
					(k - 1) * lv_font_get_line_height(sc->cfg.font));
			lv_obj_remove_flag(sc->l_arrow[k], LV_OBJ_FLAG_HIDDEN);
			continue;
		}

		// Through the waterfall for a while after a change
		if (sc->wf_until != 0 && (int32_t)(sc->wf_until - now) > 0) {
			lv_obj_set_style_bg_color(sc->wf_line[k], lv_color_hex(mark[k].color), 0);
			lv_obj_set_pos(sc->wf_line[k], x[k], 0);
			lv_obj_remove_flag(sc->wf_line[k], LV_OBJ_FLAG_HIDDEN);
		}

		lv_label_set_text(sc->l_tag[k], t);
		lv_obj_set_style_bg_color(sc->l_tag[k], lv_color_hex(mark[k].color), 0);
		lv_obj_update_layout(sc->l_tag[k]);
		tw = lv_obj_get_width(sc->l_tag[k]);
		tx = x[k] - tw / 2;
		tx = tx < 0 ? 0 : tx + tw > w ? w - tw : tx;

		// The lowest free row: below every box it would cover (its own column's, stacked
		// in T / M / S order, and those of a column close by)
		for (q = 0; q < nbox; q++)
			if (tx < box_x[q] + box_w[q] + 2 && box_x[q] < tx + tw + 2 && box_row[q] >= row)
				row = box_row[q] + 1;
		box_x[nbox] = tx;
		box_w[nbox] = tw;
		box_row[nbox++] = row;
		lv_obj_align(sc->l_tag[k], LV_ALIGN_TOP_LEFT, tx, tag_y + row * lh);
		lv_obj_remove_flag(sc->l_tag[k], LV_OBJ_FLAG_HIDDEN);
	}
}

void ui_scope_destroy(struct ui_scope *sc)
{
	free(sc->spec_buf);
	free(sc->wf_buf);
	sc->spec_buf = NULL;
	sc->wf_buf = NULL;
}

void ui_scope_set_passband(struct ui_scope *sc, uint64_t vfo_hz, const char *mode, int width_hz)
{
	uint64_t w = width_hz > 0 ? (uint64_t)width_hz : 0;

	if (w == 0) {
		if (strstr(mode, "CW") != NULL)
			w = 500;
		else if (strstr(mode, "AM") != NULL)
			w = 6000;
		else if (strstr(mode, "FM") != NULL)
			w = 12000;
		else
			w = 2400;
	}

	sc->vfo_hz = vfo_hz;
	if (strcmp(mode, "LSB") == 0) {
		sc->pb_lo = vfo_hz - w;
		sc->pb_hi = vfo_hz;
	} else if (strcmp(mode, "USB") == 0 || strstr(mode, "RTTY") != NULL) {
		sc->pb_lo = vfo_hz;
		sc->pb_hi = vfo_hz + w;
	} else {
		sc->pb_lo = vfo_hz - w / 2;
		sc->pb_hi = vfo_hz + w / 2;
	}
}

static void fmt_khz(char *buf, size_t size, uint64_t hz)
{
	snprintf(buf, size, "%u.%03u", (unsigned int)(hz / 1000000u), (unsigned int)(hz / 1000u % 1000u));
}

//
// Bins to one level per pixel column (peak of the bins in the column)
//

static void columns(struct ui_scope *sc, const struct radio_scope_row *row, int amp_max)
{
	const int32_t w = sc->cfg.w;
	int32_t x;

	for (x = 0; x < w; x++) {
		const size_t a = (size_t)x * row->len / (size_t)w;
		size_t b = (size_t)(x + 1) * row->len / (size_t)w;
		unsigned int peak = 0;
		size_t i;

		if (b <= a)
			b = a + 1;
		for (i = a; i < b && i < row->len; i++)
			if (row->bins[i] > peak)
				peak = row->bins[i];
		peak = peak * 255u / (unsigned int)amp_max;
		sc->level[x] = (uint8_t)(peak > 255u ? 255u : peak);
	}
}

static void draw_spectrum(struct ui_scope *sc)
{
	const int32_t w = sc->cfg.w;
	const int32_t h = sc->cfg.spec_h;
	const uint16_t bg = rgb565(C_SPEC_BG);
	const uint16_t grid = rgb565(C_GRID);
	const uint16_t fill = rgb565(C_FILL);
	const uint16_t line = rgb565(C_LINE);
	const uint16_t pb = rgb565(C_PASSBAND);
	const uint16_t vfo = rgb565(C_VFO);
	const int32_t pb_x0 = hz_to_x(sc, sc->pb_lo);
	const int32_t pb_x1 = hz_to_x(sc, sc->pb_hi);
	const int32_t vfo_x = hz_to_x(sc, sc->vfo_hz);
	int32_t mark_x[3];
	int32_t x;
	int32_t y;
	int k;

	for (k = 0; k < 3; k++)
		mark_x[k] = sc->marks_set && sc->mark[k].on ? hz_to_x(sc, sc->mark[k].hz) : -1;

	for (x = 0; x < w; x++) {
		const int32_t top = h - 1 - (int32_t)sc->level[x] * (h - 1) / 255;
		const bool in_pb = x >= pb_x0 && x <= pb_x1;
		const bool is_vfo = !sc->marks_set && x == vfo_x;
		uint16_t mcol[3];
		int nm = 0;

		// The markers on this column: dashed, 6 px on and 3 off for one, 6 px each in
		// turn for more (as x6100_trx)
		for (k = 0; k < 3; k++)
			if (mark_x[k] == x)
				mcol[nm++] = rgb565(sc->mark[k].color);

		for (y = 0; y < h; y++) {
			uint16_t c = (y % (h / 5 > 0 ? h / 5 : 1) == 0 || x % (w / 8 > 0 ? w / 8 : 1) == 0) ? grid : bg;

			if (y > top)
				c = fill;
			else if (y == top || y == top + 1)
				c = line;
			if (in_pb)
				c = mix565(c, pb);
			if (is_vfo)
				c = vfo;
			if (nm == 1 && y % 9 < 6)
				c = mcol[0];
			else if (nm > 1)
				c = mcol[(y / 6) % nm];
			sc->spec_buf[(size_t)y * (size_t)w + (size_t)x] = c;
		}
	}
	lv_obj_invalidate(sc->spec);
}

//
// New line at the top, wf_line_h pixels high (waterfall speed)
//

static void draw_waterfall_row(struct ui_scope *sc)
{
	const int32_t w = sc->cfg.w;
	const int32_t h = sc->cfg.wf_h;
	const int32_t n = sc->cfg.wf_line_h > 0 && sc->cfg.wf_line_h < h ? sc->cfg.wf_line_h : 1;
	int32_t x;
	int32_t y;

	memmove(sc->wf_buf + (size_t)w * (size_t)n, sc->wf_buf, (size_t)w * (size_t)(h - n) * sizeof(uint16_t));
	for (x = 0; x < w; x++)
		sc->wf_buf[x] = sc->palette[sc->level[x]];
	for (y = 1; y < n; y++)
		memcpy(sc->wf_buf + (size_t)y * (size_t)w, sc->wf_buf, (size_t)w * sizeof(uint16_t));
	lv_obj_invalidate(sc->wf);
}

void ui_scope_push(struct ui_scope *sc, const struct radio_scope_row *row, int amp_max)
{
	char buf[24];

	if (!row->valid || row->len == 0 || row->high_hz <= row->low_hz || amp_max <= 0)
		return;

	if (!sc->have_row || row->low_hz != sc->low_hz || row->high_hz != sc->high_hz) {
		sc->low_hz = row->low_hz;
		sc->high_hz = row->high_hz;
		fmt_khz(buf, sizeof(buf), row->low_hz);
		lv_label_set_text(sc->l_low, buf);
		fmt_khz(buf, sizeof(buf), (row->low_hz + row->high_hz) / 2u);
		lv_label_set_text(sc->l_mid, buf);
		fmt_khz(buf, sizeof(buf), row->high_hz);
		lv_label_set_text(sc->l_high, buf);
	}
	sc->have_row = true;

	columns(sc, row, amp_max);
	draw_spectrum(sc);
	draw_waterfall_row(sc);
}
