// SPDX-License-Identifier: GPL-3.0-or-later
//
// AAudio playback (RX, float stereo) and capture (TX, float mono).
//
// The data callbacks only copy to / from the session rings. An error
// callback (device disconnected) only sets a flag; the render thread
// closes and reopens the stream in audio_aa_service(), as AAudio does not
// allow that from the callback.
//

#include "platform/android/jni/audio_aaudio.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <aaudio/AAudio.h>
#include <android/log.h>

#include "core/session.h"

#define C_TAG		"libx6100_app"

static AAudioStream *G_OUT;
static AAudioStream *G_IN;
static bool G_WANT_IN;
static bool G_WANT_OUT;		/* playback opened: kept open (the watchdog reopens it) */
static atomic_uint G_OUT_CALLS;	/* output callbacks so far: the watchdog sees them move */
static bool G_VOICE;		/* a Bluetooth headset in call mode: voice streams (2026-11-15) */
static atomic_bool G_OUT_ERR;
static atomic_bool G_IN_ERR;
static atomic_bool G_IN_LOST;
static _Atomic(struct session *) G_SESSION;

// Callbacks running now: a detach (a reconnect, 2026-11-18) waits until none uses the
// session it takes away. Counted before the session is looked at
static atomic_int G_BUSY;

static aaudio_data_callback_result_t out_cb(AAudioStream *st, void *ud, void *data, int32_t frames)
{
	struct session *s;

	(void)st;
	(void)ud;
	atomic_fetch_add(&G_BUSY, 1);
	s = atomic_load_explicit(&G_SESSION, memory_order_acquire);
	if (s != NULL)
		session_audio_read(s, data, (size_t)frames);
	else
		memset(data, 0, (size_t)frames * 2 * sizeof(float));	/* no session: silence */
	atomic_fetch_add_explicit(&G_OUT_CALLS, 1u, memory_order_relaxed);
	atomic_fetch_sub(&G_BUSY, 1);
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static aaudio_data_callback_result_t in_cb(AAudioStream *st, void *ud, void *data, int32_t frames)
{
	struct session *s;

	(void)st;
	(void)ud;
	atomic_fetch_add(&G_BUSY, 1);
	s = atomic_load_explicit(&G_SESSION, memory_order_acquire);
	if (s != NULL)
		tx_capture_write(s, data, (size_t)frames, 1);
	atomic_fetch_sub(&G_BUSY, 1);
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void err_cb(AAudioStream *st, void *ud, aaudio_result_t err)
{
	(void)st;
	(void)err;
	atomic_store((atomic_bool *)ud, true);
}

static AAudioStream *open_stream(bool input)
{
	AAudioStreamBuilder *b = NULL;
	AAudioStream *st = NULL;
	aaudio_result_t rc;

	if (AAudio_createStreamBuilder(&b) != AAUDIO_OK)
		return NULL;

	AAudioStreamBuilder_setDirection(b, input ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
	AAudioStreamBuilder_setSampleRate(b, AUDIO_AA_RATE);
	AAudioStreamBuilder_setChannelCount(b, input ? 1 : 2);
	if (input) {
		// Phone: voice recognition, no echo cancel / noise suppression on the mic path.
		// Bluetooth headset: voice communication, the preset that takes the headset's
		// mic in call mode (with the phone's voice processing and its level control)
		AAudioStreamBuilder_setInputPreset(b, G_VOICE ? AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION
				: AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
		AAudioStreamBuilder_setDataCallback(b, in_cb, NULL);
		AAudioStreamBuilder_setErrorCallback(b, err_cb, &G_IN_ERR);
	} else {
		// Bluetooth headset: RX on the call link too (in call mode media would leave it)
		AAudioStreamBuilder_setUsage(b, G_VOICE ? AAUDIO_USAGE_VOICE_COMMUNICATION : AAUDIO_USAGE_MEDIA);
		AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SPEECH);
		AAudioStreamBuilder_setDataCallback(b, out_cb, NULL);
		AAudioStreamBuilder_setErrorCallback(b, err_cb, &G_OUT_ERR);
	}

	rc = AAudioStreamBuilder_openStream(b, &st);
	AAudioStreamBuilder_delete(b);
	if (rc != AAUDIO_OK) {
		__android_log_print(ANDROID_LOG_ERROR, C_TAG, "AAudio open %s: %s", input ? "input" : "output",
				AAudio_convertResultToText(rc));
		return NULL;
	}

	// Rate is fixed at 48 kHz: the session pipelines are set up for it
	if (AAudioStream_getSampleRate(st) != AUDIO_AA_RATE) {
		__android_log_print(ANDROID_LOG_ERROR, C_TAG, "AAudio %s rate %d, need %d", input ? "input" : "output",
				AAudioStream_getSampleRate(st), AUDIO_AA_RATE);
		AAudioStream_close(st);
		return NULL;
	}

	// Two bursts of buffering, as low as the device allows
	if (!input)
		AAudioStream_setBufferSizeInFrames(st, AAudioStream_getFramesPerBurst(st) * 2);
	return st;
}

void audio_aa_set_voice(bool voice)
{
	G_VOICE = voice;
}

int audio_aa_open_playback(void)
{
	atomic_store(&G_SESSION, NULL);
	atomic_store(&G_OUT_ERR, false);
	G_OUT = open_stream(false);
	G_WANT_OUT = true;
	return G_OUT != NULL ? 0 : -1;
}

int audio_aa_open_capture(void)
{
	atomic_store(&G_IN_ERR, false);
	atomic_store(&G_IN_LOST, false);
	G_IN = open_stream(true);
	G_WANT_IN = G_IN != NULL;
	return G_IN != NULL ? 0 : -1;
}

// The session away from the callbacks (before it is disconnected): the streams go on,
// with silence; returns once no callback uses it any more
void audio_aa_detach(void)
{
	const struct timespec ms = { 0, 1000000 };

	atomic_store_explicit(&G_SESSION, NULL, memory_order_release);
	while (atomic_load(&G_BUSY) != 0)
		nanosleep(&ms, NULL);
}

void audio_aa_attach(struct session *s)
{
	atomic_store_explicit(&G_SESSION, s, memory_order_release);
	if (G_OUT != NULL)
		AAudioStream_requestStart(G_OUT);
	if (G_IN != NULL)
		AAudioStream_requestStart(G_IN);
}

static void close_stream(AAudioStream **st)
{
	if (*st != NULL) {
		AAudioStream_requestStop(*st);
		AAudioStream_close(*st);
		*st = NULL;
	}
}

void audio_aa_close(void)
{
	close_stream(&G_OUT);
	close_stream(&G_IN);
	G_WANT_IN = false;
	G_WANT_OUT = false;
	atomic_store(&G_SESSION, NULL);
}

static uint32_t mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void reopen_out(const char *why)
{
	__android_log_print(ANDROID_LOG_WARN, C_TAG, "playback: %s, reopened", why);
	close_stream(&G_OUT);
	G_OUT = open_stream(false);
	if (G_OUT != NULL && atomic_load(&G_SESSION) != NULL)
		AAudioStream_requestStart(G_OUT);
}

//
// Playback watchdog (2026-11-20: now and then NO OUTPUT, packets came but the phone
// played none, until a reconnect, whose attach starts the stream again): a stream that
// failed to reopen is tried again each second; one whose callback has not run for
// C_OUT_STALL_MS while a session is attached (stopped or paused by the system without
// an error callback) is reopened
//

#define C_OUT_STALL_MS	1000u

static void watch_out(void)
{
	static uint32_t calls;
	static uint32_t t_moved;
	static uint32_t t_retry;
	const uint32_t t = mono_ms();
	const uint32_t c = atomic_load_explicit(&G_OUT_CALLS, memory_order_relaxed);

	if (!G_WANT_OUT)
		return;

	if (G_OUT == NULL) {
		if ((uint32_t)(t - t_retry) >= 1000u) {
			t_retry = t;
			reopen_out("no stream");
			t_moved = t;
		}
		return;
	}

	if (c != calls || atomic_load(&G_SESSION) == NULL || t_moved == 0) {
		calls = c;
		t_moved = t;
		return;
	}
	if ((uint32_t)(t - t_moved) >= C_OUT_STALL_MS) {
		char why[64];

		snprintf(why, sizeof(why), "no callback for %u ms (state %d)", (unsigned)(t - t_moved),
				(int)AAudioStream_getState(G_OUT));
		reopen_out(why);
		t_moved = t;
	}
}

void audio_aa_service(void)
{
	if (atomic_exchange(&G_OUT_ERR, false))
		reopen_out("error callback");
	watch_out();

	if (atomic_exchange(&G_IN_ERR, false)) {
		close_stream(&G_IN);
		// The capture went away: PTT is released by the caller, then reopen
		atomic_store(&G_IN_LOST, true);
		if (G_WANT_IN) {
			G_IN = open_stream(true);
			if (G_IN != NULL && atomic_load(&G_SESSION) != NULL)
				AAudioStream_requestStart(G_IN);
		}
	}
}

//
// False once after the capture stream failed (reported once per failure)
//

bool audio_aa_capture_ok(void)
{
	return !atomic_exchange(&G_IN_LOST, false);
}
