// SPDX-License-Identifier: GPL-3.0-or-later
//
// SDL2 playback and capture devices for the RX and TX audio pipelines (native target).
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct session;

//
// Device lifecycle
//

int audio_sdl_open(uint32_t want_rate, uint32_t *got_rate);
void audio_sdl_attach(struct session *s);
void audio_sdl_close(void);

//
// Capture device lifecycle (TX)
//

int audio_sdl_capture_open(uint32_t want_rate, uint32_t *got_rate);
void audio_sdl_capture_attach(struct session *s);
bool audio_sdl_capture_ok(void);
void audio_sdl_capture_close(void);
