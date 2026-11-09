// SPDX-License-Identifier: GPL-3.0-or-later
//
// SDL2 playback and capture devices (native target).
//
// Playback float32 stereo. The callback only copies from the session ring
// (session_audio_read): no allocation, no locks, no logging.
//

#include "platform/native/audio_sdl.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>

#include <SDL.h>

#include "core/session.h"

static SDL_AudioDeviceID G_DEV;
static _Atomic(struct session *) G_SESSION;

static void callback(void *ud, Uint8 *stream, int len)
{
	struct session *s = atomic_load_explicit(&G_SESSION, memory_order_acquire);

	(void)ud;
	session_audio_read(s, (float *)(void *)stream, (size_t)len / (2 * sizeof(float)));
}

//
// Open the default device, paused; returns 0 and the actual rate
//

int audio_sdl_open(uint32_t want_rate, uint32_t *got_rate)
{
	SDL_AudioSpec want;
	SDL_AudioSpec have;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		fprintf(stderr, "SDL audio init: %s\n", SDL_GetError());
		return -1;
	}

	SDL_zero(want);
	want.freq = (int)want_rate;
	want.format = AUDIO_F32SYS;
	want.channels = 2;
	want.samples = 512;
	want.callback = callback;

	atomic_store(&G_SESSION, NULL);
	G_DEV = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (G_DEV == 0) {
		fprintf(stderr, "SDL open audio: %s\n", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return -1;
	}

	*got_rate = (uint32_t)have.freq;
	return 0;
}

//
// Start playback from session s
//

void audio_sdl_attach(struct session *s)
{
	atomic_store_explicit(&G_SESSION, s, memory_order_release);
	SDL_PauseAudioDevice(G_DEV, 0);
}

//
// Stop and close; afterwards the callback no longer touches the session
//

void audio_sdl_close(void)
{
	if (G_DEV != 0) {
		SDL_PauseAudioDevice(G_DEV, 1);
		SDL_CloseAudioDevice(G_DEV);
		G_DEV = 0;
	}
	atomic_store(&G_SESSION, NULL);
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

//
// SDL2 capture device for the TX audio pipeline: float32 mono, the
// callback only copies into the session TX ring (tx_capture_write)
//

static SDL_AudioDeviceID G_CAP;
static _Atomic(struct session *) G_CAP_SESSION;

static void capture_callback(void *ud, Uint8 *stream, int len)
{
	struct session *s = atomic_load_explicit(&G_CAP_SESSION, memory_order_acquire);

	(void)ud;
	tx_capture_write(s, (const float *)(const void *)stream, (size_t)len / sizeof(float), 1);
}

//
// Open the default capture device, paused; returns 0 and the actual rate
//

int audio_sdl_capture_open(uint32_t want_rate, uint32_t *got_rate)
{
	SDL_AudioSpec want;
	SDL_AudioSpec have;

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		fprintf(stderr, "SDL audio init: %s\n", SDL_GetError());
		return -1;
	}

	SDL_zero(want);
	want.freq = (int)want_rate;
	want.format = AUDIO_F32SYS;
	want.channels = 1;
	want.samples = 480;
	want.callback = capture_callback;

	atomic_store(&G_CAP_SESSION, NULL);
	G_CAP = SDL_OpenAudioDevice(NULL, 1, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (G_CAP == 0) {
		fprintf(stderr, "SDL open capture: %s\n", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return -1;
	}

	*got_rate = (uint32_t)have.freq;
	return 0;
}

void audio_sdl_capture_attach(struct session *s)
{
	atomic_store_explicit(&G_CAP_SESSION, s, memory_order_release);
	SDL_PauseAudioDevice(G_CAP, 0);
}

//
// True while the capture device runs (false after unplug / failure)
//

bool audio_sdl_capture_ok(void)
{
	return G_CAP != 0 && SDL_GetAudioDeviceStatus(G_CAP) == SDL_AUDIO_PLAYING;
}

void audio_sdl_capture_close(void)
{
	if (G_CAP != 0) {
		SDL_PauseAudioDevice(G_CAP, 1);
		SDL_CloseAudioDevice(G_CAP);
		G_CAP = 0;
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}
	atomic_store(&G_CAP_SESSION, NULL);
}
