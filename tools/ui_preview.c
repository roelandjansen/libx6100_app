// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host preview of the operating screen: renders the UI with made-up radio
// data into memory and writes it as a binary PPM.
//
//	ui-preview <font.ttf> <out.ppm> [width] [height] [mode|keypad|tx]
//

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "core/rigfile.h"
#include "ui/ui.h"
#include "ui/ui_chooser.h"

static struct {
	int32_t w;
	int32_t h;
	uint16_t *fb;
	struct radio_state st;
	struct rig_def rig;
	bool rig_ok;
	uint32_t seq;
	uint32_t step;
	bool tx;
} G;

static uint32_t tick_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	const int32_t w = lv_area_get_width(a);
	int32_t y;

	for (y = a->y1; y <= a->y2; y++)
		memcpy(G.fb + (size_t)y * (size_t)G.w + (size_t)a->x1,
				px + (size_t)(y - a->y1) * (size_t)w * 2u, (size_t)w * 2u);
	lv_display_flush_ready(d);
}

//
// Made-up radio
//

static void op_get_state(void *ctx, struct radio_state *out)
{
	(void)ctx;
	*out = G.st;
}

static void op_get_info(void *ctx, struct session_info *out)
{
	(void)ctx;
	memset(out, 0, sizeof(*out));
	out->state = SESSION_CONNECTED;
	snprintf(out->radio_name, sizeof(out->radio_name), "X6100");
	out->audio_latency_ms = 150;
	out->tx_available = true;
	out->ptt = G.tx;
	out->ptt_remaining_s = G.tx ? 284 : 0;
}

static bool op_get_scope_row(void *ctx, struct radio_scope_row *out)
{
	static const double sig[][3] = {
		{ 7074400, 40, 1500 }, { 7076200, 70, 300 }, { 7078000, 55, 2400 },
		{ 7071500, 30, 900 }, { 7082300, 95, 2700 }, { 7086000, 25, 500 }
	};
	size_t i;
	size_t k;

	(void)ctx;
	memset(out, 0, sizeof(*out));
	out->valid = true;
	out->seq = ++G.seq;
	out->low_hz = 7065000;
	out->high_hz = 7089000;
	out->len = 475;
	for (i = 0; i < out->len; i++) {
		const double hz = 7065000.0 + 24000.0 * (double)i / (double)out->len;
		double v = 18.0 + (double)(rand() % 12);

		for (k = 0; k < sizeof(sig) / sizeof(sig[0]); k++) {
			const double d = (hz - sig[k][0]) / (sig[k][2] / 2.0);

			if (d > -1.0 && d < 1.0 && (G.seq / 7 + k) % 3 != 0)
				v += sig[k][1] * (1.0 - d * d) * (0.7 + (double)(rand() % 30) / 100.0);
		}
		out->bins[i] = (uint8_t)(v > 160 ? 160 : v);
	}
	return true;
}

static const struct rig_def *op_get_rig(void *ctx)
{
	(void)ctx;
	return G.rig_ok ? &G.rig : NULL;
}

static int op_i(void *ctx)
{
	(void)ctx;
	return 0;
}

static int op_set_freq(void *ctx, uint64_t hz)
{
	(void)ctx;
	G.st.rx[0].freq_hz[0] = hz;
	return 0;
}

static int op_tune(void *ctx, int steps)
{
	(void)ctx;
	G.st.rx[0].freq_hz[0] += (uint64_t)((int64_t)steps * G.step);
	return 0;
}

static int op_set_mode(void *ctx, int m, int d, int f)
{
	(void)ctx;
	(void)m;
	(void)d;
	(void)f;
	return 0;
}

static int op_set_value(void *ctx, const char *t, int v)
{
	(void)ctx;
	(void)t;
	(void)v;
	return 0;
}

static int op_set_span(void *ctx, uint32_t h)
{
	(void)ctx;
	(void)h;
	return 0;
}

static void op_set_step(void *ctx, uint32_t hz)
{
	(void)ctx;
	G.step = hz;
}

static uint32_t op_get_step(void *ctx)
{
	(void)ctx;
	return G.step;
}

static void op_set_lock(void *ctx, bool l)
{
	(void)ctx;
	(void)l;
}

static bool op_get_lock(void *ctx)
{
	(void)ctx;
	return false;
}

static void op_set_volume(void *ctx, uint8_t v)
{
	(void)ctx;
	(void)v;
}

static int op_ptt(void *ctx, int on)
{
	(void)ctx;
	(void)on;
	return 0;
}

static void fake_state(void)
{
	struct radio_rx *rx = &G.st.rx[0];

	memset(&G.st, 0, sizeof(G.st));
	G.st.rig_valid = true;
	snprintf(G.st.model, sizeof(G.st.model), "X6100");
	G.st.amp_max = 160;
	rx->freq_valid[0] = true;
	rx->freq_hz[0] = 7077000;
	rx->mode[0].valid = true;
	rx->mode[0].num = 1;
	rx->mode[0].filter = 2;
	snprintf(rx->mode[0].name, sizeof(rx->mode[0].name), "USB");
	snprintf(rx->mode[0].filter_name, sizeof(rx->mode[0].filter_name), "FIL2");
	rx->scope.mode.valid = true;
	rx->scope.mode.value = 0;
	rx->scope.span_hz.valid = true;
	rx->scope.span_hz.value = 12000;
	G.st.meter[RADIO_METER_S].valid = true;
	G.st.meter[RADIO_METER_S].cal = 10.0;
	G.st.agc = (struct radio_value){ true, 3 };
	G.st.preamp = (struct radio_value){ true, 1 };
	G.st.attenuator = (struct radio_value){ true, 0 };
	G.st.transmit = (struct radio_value){ true, G.tx ? 1 : 0 };
	G.st.split = (struct radio_value){ true, 0 };
	if (G.tx) {
		G.st.meter[RADIO_METER_POWER] = (struct radio_meter_value){ true, 180, 8.2, false };
		G.st.meter[RADIO_METER_SWR] = (struct radio_meter_value){ true, 40, 1.3, false };
		G.st.meter[RADIO_METER_ALC] = (struct radio_meter_value){ true, 20, 0.2, false };
	}
}

static int write_ppm(const char *path)
{
	FILE *f = fopen(path, "wb");
	size_t i;

	if (f == NULL)
		return -1;
	fprintf(f, "P6\n%d %d\n255\n", (int)G.w, (int)G.h);
	for (i = 0; i < (size_t)G.w * (size_t)G.h; i++) {
		const uint16_t c = G.fb[i];
		const unsigned char rgb[3] = {
			(unsigned char)(((c >> 11) & 0x1fu) * 255u / 31u),
			(unsigned char)(((c >> 5) & 0x3fu) * 255u / 63u),
			(unsigned char)((c & 0x1fu) * 255u / 31u)
		};

		fwrite(rgb, 1, 3, f);
	}
	return fclose(f);
}

static void *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	void *buf;
	long n;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n);
	if (buf != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*len = (size_t)n;
	return buf;
}

int main(int argc, char **argv)
{
	struct ui_ops ops;
	struct ui_font_data font;
	lv_display_t *d;
	void *dbuf;
	const char *what = argc > 5 ? argv[5] : "";
	char err[256];
	int i;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <font.ttf> <out.ppm> [width] [height] [mode|keypad|tx]\n", argv[0]);
		return EXIT_FAILURE;
	}
	G.w = argc > 3 ? (int32_t)strtol(argv[3], NULL, 10) : 480;
	G.h = argc > 4 ? (int32_t)strtol(argv[4], NULL, 10) : 960;
	G.step = 100;
	G.tx = strcmp(what, "tx") == 0;
	G.rig_ok = rigfile_load(&G.rig, "rigs/X6100.rig", err, sizeof(err)) == 0;
	fake_state();

	font.ttf = read_file(argv[1], &font.len);
	if (font.ttf == NULL) {
		fprintf(stderr, "cannot read font %s\n", argv[1]);
		return EXIT_FAILURE;
	}

	lv_init();
	lv_tick_set_cb(tick_ms);
	G.fb = calloc((size_t)G.w * (size_t)G.h, 2);
	dbuf = malloc((size_t)G.w * (size_t)G.h * 2u);
	d = lv_display_create(G.w, G.h);
	lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(d, dbuf, NULL, (uint32_t)((size_t)G.w * (size_t)G.h * 2u),
			LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(d, flush);

	memset(&ops, 0, sizeof(ops));
	ops.get_state = op_get_state;
	ops.get_info = op_get_info;
	ops.get_scope_row = op_get_scope_row;
	ops.get_rig = op_get_rig;
	ops.set_freq = op_set_freq;
	ops.tune = op_tune;
	ops.set_mode = op_set_mode;
	ops.set_value = op_set_value;
	ops.set_span = op_set_span;
	ops.set_step = op_set_step;
	ops.get_step = op_get_step;
	ops.set_lock = op_set_lock;
	ops.get_lock = op_get_lock;
	ops.set_volume = op_set_volume;
	ops.ptt = op_ptt;
	ops.ptt_heartbeat = op_i;
	ops.atu_tune = op_i;

	if (ui_create(d, &ops, &font) != 0) {
		fprintf(stderr, "ui_create failed\n");
		return EXIT_FAILURE;
	}

	for (i = 0; i < 1500; i++) {
		ui_poll();
		lv_timer_handler();
	}

	if (strcmp(what, "mode") == 0) {
		const char *names[8];
		size_t k;

		for (k = 0; k < G.rig.n_modes && k < 8; k++)
			names[k] = G.rig.modes[k].name;
		ui_chooser_open("Mode", names, k, 1, NULL, NULL);
	} else if (strcmp(what, "keypad") == 0) {
		ui_keypad_open("Frequency (MHz)", NULL, NULL);
	}
	lv_obj_invalidate(lv_screen_active());
	lv_refr_now(d);

	if (write_ppm(argv[2]) != 0) {
		fprintf(stderr, "cannot write %s\n", argv[2]);
		return EXIT_FAILURE;
	}
	printf("wrote %s (%dx%d)\n", argv[2], (int)G.w, (int)G.h);
	ui_destroy();
	return EXIT_SUCCESS;
}
