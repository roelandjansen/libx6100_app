// SPDX-License-Identifier: GPL-3.0-or-later
//
// SDL2 playback device for the RX audio pipeline (native target).
//

#pragma once

#include <stdint.h>

struct session;

//
// Device lifecycle
//

int audio_sdl_open(uint32_t want_rate, uint32_t *got_rate);
void audio_sdl_attach(struct session *s);
void audio_sdl_close(void);
