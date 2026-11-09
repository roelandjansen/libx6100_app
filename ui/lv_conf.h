// SPDX-License-Identifier: GPL-3.0-or-later
//
// LVGL 9.2 configuration for all targets. Only values that differ from
// LVGL's defaults (lv_conf_internal.h) are set here.
//

#pragma once

// checked by lv_conf_internal.h
#define LV_CONF_H

//
// Colour and memory: RGB565 matches ANativeWindow WINDOW_FORMAT_RGB_565
// and the X6100 framebuffer; allocation through the C library
//
#define LV_COLOR_DEPTH			16
#define LV_USE_STDLIB_MALLOC		LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING		LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF		LV_STDLIB_CLIB

//
// The UI runs on one thread; the platform drives lv_timer_handler()
//
#define LV_USE_OS			LV_OS_NONE
#define LV_DEF_REFR_PERIOD		33
#define LV_DPI_DEF			160

//
// Rendering
//
#define LV_USE_DRAW_SW			1
#define LV_DRAW_SW_COMPLEX		1
#define LV_USE_LOG			0
#define LV_USE_ASSERT_NULL		1
#define LV_USE_ASSERT_MALLOC		1

//
// Fonts: one TrueType font supplied by the platform, rendered at the
// sizes the layout needs (tiny_ttf); Montserrat 14 only as fallback
//
#define LV_FONT_MONTSERRAT_14		1
#define LV_FONT_DEFAULT			&lv_font_montserrat_14
#define LV_USE_TINY_TTF			1
#define LV_TINY_TTF_FILE_SUPPORT	0
#define LV_TINY_TTF_CACHE_GLYPH_CNT	256

//
// Widgets and extras in use
//
#define LV_USE_CANVAS			1
#define LV_USE_SCALE			1
#define LV_USE_OBSERVER			0
#define LV_USE_SYSMON			0
#define LV_USE_THEME_DEFAULT		1
#define LV_THEME_DEFAULT_DARK		1
