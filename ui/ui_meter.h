// SPDX-License-Identifier: GPL-3.0-or-later
//
// Multi-scale transceiver meter (Icom style) as libx6100 tools/x6100_trx.c
// draw_vu(): S / Po, SWR, COMP, ALC on concentric arcs around a pivot
// below the face, white needle with attack / release ballistics and a
// peak-hold marker (vu_update()).
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

struct ui_meter;

//
// w: face width in pixels (height follows the X6100 face, 296 x 168);
// ttf: font data for the scale labels
//

struct ui_meter *ui_meter_create(lv_obj_t *parent, int32_t w, const void *ttf, size_t ttf_len);
void ui_meter_destroy(struct ui_meter *m);
lv_obj_t *ui_meter_obj(struct ui_meter *m);

//
// Readings, once per frame: RX with the S-meter in dB over S9 (wfview
// calibration, S9 = 0), TX with Po in watts and SWR. full_w: Po full scale.
//

void ui_meter_rx(struct ui_meter *m, bool valid, double db_over_s9);
void ui_meter_tx(struct ui_meter *m, double watts, double swr, double full_w);
