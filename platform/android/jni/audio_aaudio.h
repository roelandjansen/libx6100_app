// SPDX-License-Identifier: GPL-3.0-or-later
//
// AAudio playback (RX, float stereo) and capture (TX, float mono) at a
// fixed 48 kHz. Streams are reopened after a device change (headphones,
// Bluetooth); the session ring is kept.
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_AA_RATE	48000

struct session;

//
// Lifecycle (UI / main thread)
//

int audio_aa_open_playback(void);
int audio_aa_open_capture(void);
void audio_aa_attach(struct session *s);
void audio_aa_close(void);

//
// Called regularly from the render thread: reopens streams that failed
// (error callback), reports a capture that cannot be reopened
//

void audio_aa_service(void);
bool audio_aa_capture_ok(void);
